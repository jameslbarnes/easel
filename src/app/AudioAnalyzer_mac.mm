#ifdef __APPLE__
#include "app/AudioAnalyzer.h"
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <CoreMedia/CoreMedia.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <iostream>
#include <mutex>
#include <vector>

// ─── ScreenCaptureKit audio delegate (for system audio loopback) ──

@interface EaselAudioCaptureDelegate : NSObject <SCStreamOutput>
@property (nonatomic) std::mutex* bufferMutex;
@property (nonatomic) std::vector<float>* sampleBuffer;
@end

@implementation EaselAudioCaptureDelegate
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio) return;

    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!formatDesc) return;

    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc);
    if (!asbd) return;

    size_t totalBytes = 0;
    char* dataPointer = nullptr;
    OSStatus status = CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr, &totalBytes, &dataPointer);
    if (status != noErr || !dataPointer || totalBytes == 0) return;

    int channels = (int)asbd->mChannelsPerFrame;
    if (channels < 1) channels = 1;

    const float* floatData = (const float*)dataPointer;
    int totalSamples = (int)(totalBytes / sizeof(float));
    int frames = totalSamples / channels;

    std::lock_guard<std::mutex> lock(*self.bufferMutex);
    for (int i = 0; i < frames; i++) {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ch++) {
            mono += floatData[i * channels + ch];
        }
        mono /= channels;
        self.sampleBuffer->push_back(mono);
    }
}
@end

// ─── Internal state ──────────────────────────────────────────────

struct MacAudioState {
    // ScreenCaptureKit (system audio loopback)
    SCStream* stream = nil;
    EaselAudioCaptureDelegate* delegate = nil;
    dispatch_queue_t queue = nil;

    // CoreAudio AudioUnit (microphone input)
    AudioComponentInstance audioUnit = nullptr;

    // Shared buffer
    std::mutex bufferMutex;
    std::vector<float> sampleBuffer;

    bool isInputDevice = false;
};

// ─── CoreAudio input callback ───────────────────────────────────

static OSStatus audioInputCallback(void* inRefCon,
                                    AudioUnitRenderActionFlags* ioActionFlags,
                                    const AudioTimeStamp* inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList* ioData) {
    auto* state = (MacAudioState*)inRefCon;
    if (!state || !state->audioUnit) return noErr;

    // Allocate buffer for input
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = 1; // mono
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    std::vector<float> tempBuf(inNumberFrames);
    bufferList.mBuffers[0].mData = tempBuf.data();

    OSStatus status = AudioUnitRender(state->audioUnit, ioActionFlags, inTimeStamp,
                                       inBusNumber, inNumberFrames, &bufferList);
    if (status != noErr) return status;

    std::lock_guard<std::mutex> lock(state->bufferMutex);
    float* samples = (float*)bufferList.mBuffers[0].mData;
    for (UInt32 i = 0; i < inNumberFrames; i++) {
        state->sampleBuffer.push_back(samples[i]);
    }

    return noErr;
}

// ─── Helper: find AudioDeviceID from UID string ─────────────────

static AudioDeviceID findDeviceByUID(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;

    CFStringRef uidStr = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (!uidStr) return kAudioObjectUnknown;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID deviceId = kAudioObjectUnknown;
    UInt32 size = sizeof(AudioDeviceID);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, sizeof(CFStringRef), &uidStr, &size, &deviceId);
    CFRelease(uidStr);
    return deviceId;
}

// ─── initCapture: CoreAudio for mic, ScreenCaptureKit for system ─

void AudioAnalyzer::initCapture() {
    cleanupCapture();

    auto* state = new MacAudioState();
    m_macAudioImpl = state;

    // If a specific capture device is selected, use CoreAudio AudioUnit
    if (m_requestedIsCapture && !m_deviceId.empty()) {
        AudioDeviceID deviceId = findDeviceByUID(m_deviceId);
        if (deviceId == kAudioObjectUnknown) {
            std::cerr << "[AudioAnalyzer] Device not found: " << m_deviceId << std::endl;
            cleanupCapture();
            return;
        }

        state->isInputDevice = true;

        // Create HAL audio unit for input
        AudioComponentDescription desc = {};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            std::cerr << "[AudioAnalyzer] Failed to find HAL audio component" << std::endl;
            cleanupCapture();
            return;
        }

        OSStatus status = AudioComponentInstanceNew(comp, &state->audioUnit);
        if (status != noErr) {
            std::cerr << "[AudioAnalyzer] Failed to create audio unit: " << status << std::endl;
            cleanupCapture();
            return;
        }

        // Enable input, disable output
        UInt32 enableIO = 1;
        AudioUnitSetProperty(state->audioUnit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
        enableIO = 0;
        AudioUnitSetProperty(state->audioUnit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));

        // Set the input device
        AudioUnitSetProperty(state->audioUnit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &deviceId, sizeof(deviceId));

        // Set format: 48kHz mono float
        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate = 48000;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBitsPerChannel = 32;
        fmt.mChannelsPerFrame = 1;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = sizeof(float);
        fmt.mBytesPerPacket = sizeof(float);

        AudioUnitSetProperty(state->audioUnit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

        // Set input callback
        AURenderCallbackStruct callbackStruct = {};
        callbackStruct.inputProc = audioInputCallback;
        callbackStruct.inputProcRefCon = state;
        AudioUnitSetProperty(state->audioUnit, kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global, 0, &callbackStruct, sizeof(callbackStruct));

        status = AudioUnitInitialize(state->audioUnit);
        if (status != noErr) {
            std::cerr << "[AudioAnalyzer] Failed to initialize audio unit: " << status << std::endl;
            cleanupCapture();
            return;
        }

        status = AudioOutputUnitStart(state->audioUnit);
        if (status != noErr) {
            std::cerr << "[AudioAnalyzer] Failed to start audio unit: " << status << std::endl;
            cleanupCapture();
            return;
        }

        m_initialized = true;
        m_sampleRate = 48000;
        m_channels = 1;
        std::cout << "[AudioAnalyzer] CoreAudio input capture started (device: " << m_deviceId << ")" << std::endl;
        return;
    }

    // Default: ScreenCaptureKit system audio loopback. Gated behind an
    // explicit opt-in so the macOS "Screen Recording" TCC prompt only fires
    // when the user actually needs system-audio capture (picks System Audio
    // in the dropdown, starts recording, etc.) — otherwise self-signed dev
    // builds would prompt on every launch because the cdhash changes.
    if (!m_wantsSystemAudio) {
        cleanupCapture();
        return;
    }

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool success = false;

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
        if (error || !content) {
            std::cerr << "[AudioAnalyzer] Failed to get shareable content: "
                      << (error ? error.localizedDescription.UTF8String : "unknown") << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        SCDisplay* display = content.displays.firstObject;
        if (!display) {
            std::cerr << "[AudioAnalyzer] No displays found for audio capture" << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.capturesAudio = YES;
        config.excludesCurrentProcessAudio = NO;
        config.sampleRate = 48000;
        config.channelCount = 2;
        config.width = 2;
        config.height = 2;
        config.minimumFrameInterval = CMTimeMake(1, 1);

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

        state->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
        state->delegate = [[EaselAudioCaptureDelegate alloc] init];
        state->delegate.bufferMutex = &state->bufferMutex;
        state->delegate.sampleBuffer = &state->sampleBuffer;

        state->queue = dispatch_queue_create("com.easel.audiocapture", DISPATCH_QUEUE_SERIAL);

        NSError* addError = nil;
        [state->stream addStreamOutput:state->delegate type:SCStreamOutputTypeAudio sampleHandlerQueue:state->queue error:&addError];
        if (addError) {
            std::cerr << "[AudioAnalyzer] Failed to add audio output: " << addError.localizedDescription.UTF8String << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        [state->stream startCaptureWithCompletionHandler:^(NSError* startError) {
            if (startError) {
                std::cerr << "[AudioAnalyzer] Failed to start audio capture: " << startError.localizedDescription.UTF8String << std::endl;
            } else {
                success = true;
                std::cout << "[AudioAnalyzer] macOS system audio capture started (48kHz stereo -> mono)" << std::endl;
            }
            dispatch_semaphore_signal(sem);
        }];
    }];

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    if (success) {
        m_initialized = true;
        m_sampleRate = 48000;
        m_channels = 2;
    } else {
        std::cerr << "[AudioAnalyzer] macOS audio capture failed — FFT will not update" << std::endl;
        std::cerr << "[AudioAnalyzer] Grant Screen Recording permission in System Settings > Privacy & Security" << std::endl;
        cleanupCapture();
    }
}

void AudioAnalyzer::cleanupCapture() {
    if (m_macAudioImpl) {
        auto* state = (MacAudioState*)m_macAudioImpl;
        if (state->audioUnit) {
            AudioOutputUnitStop(state->audioUnit);
            AudioUnitUninitialize(state->audioUnit);
            AudioComponentInstanceDispose(state->audioUnit);
        }
        if (state->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [state->stream stopCaptureWithCompletionHandler:^(NSError* error) {
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        delete state;
        m_macAudioImpl = nullptr;
    }
    m_initialized = false;
    m_samplesAccumulated = 0;
}

void AudioAnalyzer::drainPackets() {
    if (!m_macAudioImpl) return;
    auto* state = (MacAudioState*)m_macAudioImpl;

    std::vector<float> samples;
    {
        std::lock_guard<std::mutex> lock(state->bufferMutex);
        samples.swap(state->sampleBuffer);
    }

    for (float s : samples) {
        m_ringBuf[m_ringPos] = s;
        m_ringPos = (m_ringPos + 1) % kFFTSize;
        m_samplesAccumulated++;
    }
}
#endif
