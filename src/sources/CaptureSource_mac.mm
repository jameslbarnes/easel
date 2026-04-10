#ifdef __APPLE__
#include "sources/CaptureSource_mac.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <iostream>
#include <mutex>

// ─── ScreenCaptureKit delegate ──────────────────────────────────────

@interface EaselCaptureDelegate : NSObject <SCStreamOutput>
@property (nonatomic) std::vector<uint8_t>* pixelBuffer;
@property (nonatomic) int* captureWidth;
@property (nonatomic) int* captureHeight;
@property (nonatomic) std::mutex* bufferMutex;
@property (nonatomic) bool* hasNewFrame;
@end

@implementation EaselCaptureDelegate
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    size_t w = CVPixelBufferGetWidth(imageBuffer);
    size_t h = CVPixelBufferGetHeight(imageBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
    uint8_t* baseAddress = (uint8_t*)CVPixelBufferGetBaseAddress(imageBuffer);

    if (baseAddress && w > 0 && h > 0) {
        std::lock_guard<std::mutex> lock(*self.bufferMutex);
        *self.captureWidth = (int)w;
        *self.captureHeight = (int)h;
        self.pixelBuffer->resize(w * h * 4);

        // Copy row-by-row (bytesPerRow may include padding)
        for (size_t y = 0; y < h; y++) {
            memcpy(self.pixelBuffer->data() + y * w * 4,
                   baseAddress + y * bytesPerRow,
                   w * 4);
        }
        *self.hasNewFrame = true;
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}
@end

// ─── Internal state stored via m_impl ───────────────────────────────

struct MacCaptureState {
    SCStream* stream = nil;
    EaselCaptureDelegate* delegate = nil;
    dispatch_queue_t queue = nil;
    std::mutex bufferMutex;
    bool hasNewFrame = false;
};

// ─── CaptureSource implementation ──────────────────────────────────

CaptureSource::~CaptureSource() {
    cleanup();
}

std::vector<CaptureMonitorInfo> CaptureSource::enumerateMonitors() {
    std::vector<CaptureMonitorInfo> monitors;

    uint32_t displayCount = 0;
    CGGetActiveDisplayList(0, nullptr, &displayCount);
    if (displayCount == 0) return monitors;

    std::vector<CGDirectDisplayID> displays(displayCount);
    CGGetActiveDisplayList(displayCount, displays.data(), &displayCount);

    for (uint32_t i = 0; i < displayCount; i++) {
        CaptureMonitorInfo info;
        info.index = i;
        info.width = (int)CGDisplayPixelsWide(displays[i]);
        info.height = (int)CGDisplayPixelsHigh(displays[i]);
        info.name = "Display " + std::to_string(i);
        if (CGDisplayIsMain(displays[i])) info.name += " (Main)";
        monitors.push_back(info);
    }

    return monitors;
}

bool CaptureSource::start(int monitorIndex) {
    // Get display list
    uint32_t displayCount = 0;
    CGGetActiveDisplayList(0, nullptr, &displayCount);
    std::vector<CGDirectDisplayID> displays(displayCount);
    CGGetActiveDisplayList(displayCount, displays.data(), &displayCount);

    if (monitorIndex < 0 || monitorIndex >= (int)displayCount) return false;

    CGDirectDisplayID displayID = displays[monitorIndex];
    m_width = (int)CGDisplayPixelsWide(displayID);
    m_height = (int)CGDisplayPixelsHigh(displayID);
    m_pixelBuffer.resize(m_width * m_height * 4);

    // Create internal state
    auto* state = new MacCaptureState();
    m_impl = state;

    // Use ScreenCaptureKit to capture the display
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool success = false;
    int captureW = m_width;
    int captureH = m_height;
    auto* pixBuf = &m_pixelBuffer;
    auto* widthPtr = &m_width;
    auto* heightPtr = &m_height;

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
        if (error || !content) {
            std::cerr << "[CaptureSource] Failed to get shareable content" << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        // Find the matching display
        SCDisplay* targetDisplay = nil;
        for (SCDisplay* d in content.displays) {
            if (d.displayID == displayID) {
                targetDisplay = d;
                break;
            }
        }

        if (!targetDisplay) {
            std::cerr << "[CaptureSource] Display not found in ScreenCaptureKit" << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        // Configure stream
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.width = captureW;
        config.height = captureH;
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, 30); // 30 fps
        config.showsCursor = YES;

        // Create content filter for this display (all windows)
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:targetDisplay excludingWindows:@[]];

        // Create stream
        state->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
        state->delegate = [[EaselCaptureDelegate alloc] init];
        state->delegate.pixelBuffer = pixBuf;
        state->delegate.captureWidth = widthPtr;
        state->delegate.captureHeight = heightPtr;
        state->delegate.bufferMutex = &state->bufferMutex;
        state->delegate.hasNewFrame = &state->hasNewFrame;

        state->queue = dispatch_queue_create("com.easel.capture", DISPATCH_QUEUE_SERIAL);

        NSError* addError = nil;
        [state->stream addStreamOutput:state->delegate type:SCStreamOutputTypeScreen sampleHandlerQueue:state->queue error:&addError];
        if (addError) {
            std::cerr << "[CaptureSource] Failed to add stream output: " << addError.localizedDescription.UTF8String << std::endl;
            dispatch_semaphore_signal(sem);
            return;
        }

        [state->stream startCaptureWithCompletionHandler:^(NSError* startError) {
            if (startError) {
                std::cerr << "[CaptureSource] Failed to start capture: " << startError.localizedDescription.UTF8String << std::endl;
            } else {
                success = true;
                std::cout << "[CaptureSource] Started macOS screen capture " << captureW << "x" << captureH << std::endl;
            }
            dispatch_semaphore_signal(sem);
        }];
    }];

    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    if (success) {
        m_active = true;
    } else {
        cleanup();
    }
    return success;
}

void CaptureSource::stop() {
    cleanup();
}

void CaptureSource::update() {
    if (!m_active || !m_impl) return;

    auto* state = (MacCaptureState*)m_impl;

    std::lock_guard<std::mutex> lock(state->bufferMutex);
    if (!state->hasNewFrame) return;
    state->hasNewFrame = false;

    // Upload to OpenGL texture
    if (!m_texture.id()) {
        m_texture.createEmpty(m_width, m_height, GL_RGBA8);
    }

    // BGRA -> need to swizzle or use GL_BGRA
    m_texture.updateData(m_pixelBuffer.data(), m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE);
}

void CaptureSource::cleanup() {
    if (m_impl) {
        auto* state = (MacCaptureState*)m_impl;
        if (state->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [state->stream stopCaptureWithCompletionHandler:^(NSError* error) {
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        delete state;
        m_impl = nullptr;
    }
    m_active = false;
}
#endif
