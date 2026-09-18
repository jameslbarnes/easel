#ifdef HAS_FFMPEG
#include "app/VideoRecorder.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <filesystem>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
}

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <propidl.h>
#include <ksmedia.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// waveIn state for an MME-mode capture lane.
struct MmeCapture {
    static constexpr int kBufs = 8;
    static constexpr int kFramesPerBuf = 960;   // 20ms @ 48k stereo
    HWAVEIN h = nullptr;
    WAVEHDR hdr[kBufs] = {};
    std::vector<int16_t> buf[kBufs];
};
#endif

VideoRecorder::~VideoRecorder() {
    stop();
}

bool VideoRecorder::start(const std::string& path, int width, int height, int fps) {
    stop();

    // Ensure parent directory exists
    std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    if (!initEncoder(path, width, height, fps)) {
        cleanup();
        return false;
    }
    m_path = path;
    m_startTime = av_gettime_relative() / 1000000.0;
    m_stopRequested = false;
    m_active = true;
    m_videoFrameIndex = 0;
    m_startPtsUs = 0;          // anchored lazily on the first encoded frame
    m_lastVideoPts = -1;
    m_audioSamplesWritten = 0;
    m_audioStartUs = 0;        // anchored lazily on the first audio drain
    m_statMixed = 0;           // per-session diagnostics: stale values from a
    m_lastStatsUs = 0;         //   previous session poison the stats/stall logs
    m_hbUs = 0;
    m_lastAudioMuxUs = 0;
    m_lastAudioMuxSamples = 0;
    m_stallLogUs = 0;
    m_thread = std::thread(&VideoRecorder::encodeThread, this);
    std::cout << "[REC] Recording to: " << path << std::endl;
    return true;
}

void VideoRecorder::stop() {
    if (m_active) {
        m_stopRequested = true;
        m_cv.notify_all();
        if (m_thread.joinable())
            m_thread.join();
        m_active = false;
        std::cout << "[REC] Recording saved: " << m_path << std::endl;
    }
    cleanup();
}

double VideoRecorder::uptimeSeconds() const {
    if (!m_active) return 0;
    return av_gettime_relative() / 1000000.0 - m_startTime;
}

// ─── WASAPI audio device enumeration ────────────────────────────────

#ifdef _WIN32
std::vector<RecAudioDevice> VideoRecorder::enumerateAudioDevices() {
    std::vector<RecAudioDevice> result;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&enumerator);
    if (FAILED(hr)) return result;

    // Enumerate render devices (output loopback)
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(collection->Item(i, &dev))) {
                RecAudioDevice info;
                info.isCapture = false;

                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid))) {
                    char buf[512];
                    wcstombs(buf, wid, sizeof(buf));
                    info.id = buf;
                    CoTaskMemFree(wid);
                }

                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
                    PROPERTYKEY key;
                    key.fmtid = {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
                    key.pid = 14;
                    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) {
                        char buf[512];
                        wcstombs(buf, pv.pwszVal, sizeof(buf));
                        info.name = buf;
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (info.name.empty()) info.name = "Output " + std::to_string(i);
                info.name += " (loopback)";
                result.push_back(info);
                dev->Release();
            }
        }
        collection->Release();
    }

    // Enumerate capture devices (microphones)
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(collection->Item(i, &dev))) {
                RecAudioDevice info;
                info.isCapture = true;

                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid))) {
                    char buf[512];
                    wcstombs(buf, wid, sizeof(buf));
                    info.id = buf;
                    CoTaskMemFree(wid);
                }

                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    PROPERTYKEY key;
                    key.fmtid = {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
                    key.pid = 14;
                    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR) {
                        char buf[512];
                        wcstombs(buf, pv.pwszVal, sizeof(buf));
                        info.name = buf;
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (info.name.empty()) info.name = "Microphone " + std::to_string(i);
                result.push_back(info);
                dev->Release();
            }
        }
        collection->Release();
    }

    enumerator->Release();
    return result;
}
#elif defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static std::string cfStringToStd(CFStringRef cfStr) {
    if (!cfStr) return "";
    char buf[256] = {};
    if (CFStringGetCString(cfStr, buf, sizeof(buf), kCFStringEncodingUTF8))
        return buf;
    return "";
}

std::vector<RecAudioDevice> VideoRecorder::enumerateAudioDevices() {
    std::vector<RecAudioDevice> result;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    if (status != noErr || dataSize == 0) return result;

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(deviceCount);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, devices.data());
    if (status != noErr) return result;

    for (AudioDeviceID devId : devices) {
        // Get device name
        CFStringRef nameRef = nullptr;
        AudioObjectPropertyAddress nameProp = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 nameSize = sizeof(CFStringRef);
        AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &nameRef);
        std::string name = cfStringToStd(nameRef);
        if (nameRef) CFRelease(nameRef);

        // Get device UID
        CFStringRef uidRef = nullptr;
        AudioObjectPropertyAddress uidProp = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 uidSize = sizeof(CFStringRef);
        AudioObjectGetPropertyData(devId, &uidProp, 0, nullptr, &uidSize, &uidRef);
        std::string uid = cfStringToStd(uidRef);
        if (uidRef) CFRelease(uidRef);

        // Check if device has input channels (microphone/capture)
        AudioObjectPropertyAddress inputProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 inputSize = 0;
        bool hasInput = false;
        if (AudioObjectGetPropertyDataSize(devId, &inputProp, 0, nullptr, &inputSize) == noErr && inputSize > 0) {
            std::vector<uint8_t> buf(inputSize);
            auto* bufList = (AudioBufferList*)buf.data();
            if (AudioObjectGetPropertyData(devId, &inputProp, 0, nullptr, &inputSize, bufList) == noErr) {
                for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
                    if (bufList->mBuffers[i].mNumberChannels > 0) { hasInput = true; break; }
                }
            }
        }

        // Check if device has output channels
        AudioObjectPropertyAddress outputProp = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 outputSize = 0;
        bool hasOutput = false;
        if (AudioObjectGetPropertyDataSize(devId, &outputProp, 0, nullptr, &outputSize) == noErr && outputSize > 0) {
            std::vector<uint8_t> buf(outputSize);
            auto* bufList = (AudioBufferList*)buf.data();
            if (AudioObjectGetPropertyData(devId, &outputProp, 0, nullptr, &outputSize, bufList) == noErr) {
                for (UInt32 i = 0; i < bufList->mNumberBuffers; i++) {
                    if (bufList->mBuffers[i].mNumberChannels > 0) { hasOutput = true; break; }
                }
            }
        }

        if (hasInput) {
            RecAudioDevice dev;
            dev.name = name;
            dev.id = uid;
            dev.isCapture = true;
            result.push_back(dev);
        }
        if (hasOutput) {
            RecAudioDevice dev;
            dev.name = name;
            dev.id = uid;
            dev.isCapture = false;
            result.push_back(dev);
        }
    }

    return result;
}
#else
std::vector<RecAudioDevice> VideoRecorder::enumerateAudioDevices() {
    return {};
}
#endif

// ─── WASAPI capture ─────────────────────────────────────────────────
//
// Two lanes are recorded and mixed into the single AAC track: the system audio
// (render loopback) and the microphone. Each lane resamples independently to a
// fixed 48kHz stereo mix format; drainAudio() mixes them sample-aligned. A lane
// is silence-padded against wall-clock while its device is idle or dead, and a
// dead device is re-opened every couple of seconds — a Focusrite endpoint
// restart mid-recording costs a gap of silence, not the rest of the track.

static constexpr int kMixRate = 48000;
static constexpr int kMixCh   = 2;

#ifdef _WIN32

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool VideoRecorder::initAudioCapture() {
    m_wasapiSampleRate = kMixRate;
    m_wasapiChannels = kMixCh;
    m_audioSources.clear();

    auto devices = enumerateAudioDevices();
    const RecAudioDevice* selected = nullptr;
    if (m_selectedAudioDevice >= 0 && m_selectedAudioDevice < (int)devices.size())
        selected = &devices[m_selectedAudioDevice];

    // System lane — a user-selected render device overrides the default output.
    // EASEL_RECORD_SYS picks any device by name substring instead (a render
    // device is loopback-captured; a capture device — e.g. a virtual cable's
    // output side — is read directly); =0/off/none disables the lane.
    bool wantSys = true;
    std::string sysMatch;
    if (const char* sv = std::getenv("EASEL_RECORD_SYS")) {
        std::string v = lowerCopy(sv);
        if (v == "0" || v == "off" || v == "none" || v == "false") wantSys = false;
        else sysMatch = v;
    }
    if (wantSys) {
        auto sysPtr = std::make_unique<AudioSource>();
        AudioSource& sys = *sysPtr;
        sys.isCapture = false;
        sys.label = "system";
        if (!sysMatch.empty()) {
            for (auto& d : devices) {
                if (lowerCopy(d.name).find(sysMatch) != std::string::npos) {
                    sys.id = d.id; sys.name = d.name; sys.isCapture = d.isCapture; break;
                }
            }
            if (sys.id.empty())
                std::cerr << "[REC] Audio: no device matches EASEL_RECORD_SYS='"
                          << sysMatch << "', using default output (loopback)" << std::endl;
        }
        if (sys.id.empty() && selected && !selected->isCapture) {
            sys.id = selected->id; sys.name = selected->name;
        }
        if (const char* g = std::getenv("EASEL_RECORD_SYS_GAIN")) sys.gain = (float)atof(g);
        if (const char* a = std::getenv("EASEL_RECORD_SYS_API")) {
            std::string v = lowerCopy(a);
            if (v == "mme") { sys.useMme = true; sys.apiForce = 2; }
            else if (v == "wasapi") sys.apiForce = 1;
        }
        m_audioSources.push_back(std::move(sysPtr));
    }

    // Mic lane — a user-selected capture device overrides; EASEL_RECORD_MIC picks
    // by name substring, and =0/off/none disables the lane entirely.
    bool wantMic = true;
    std::string micMatch;
    if (const char* mv = std::getenv("EASEL_RECORD_MIC")) {
        std::string v = lowerCopy(mv);
        if (v == "0" || v == "off" || v == "none" || v == "false") wantMic = false;
        else micMatch = v;
    }
    if (wantMic) {
        auto micPtr = std::make_unique<AudioSource>();
        AudioSource& mic = *micPtr;
        mic.isCapture = true;
        mic.label = "mic";
        if (selected && selected->isCapture) {
            mic.id = selected->id; mic.name = selected->name;
        } else if (!micMatch.empty()) {
            for (auto& d : devices) {
                if (d.isCapture && lowerCopy(d.name).find(micMatch) != std::string::npos) {
                    mic.id = d.id; mic.name = d.name; break;
                }
            }
            if (mic.id.empty())
                std::cerr << "[REC] Audio: no capture device matches EASEL_RECORD_MIC='"
                          << micMatch << "', using default microphone" << std::endl;
        }
        if (const char* g = std::getenv("EASEL_RECORD_MIC_GAIN")) mic.gain = (float)atof(g);
        if (const char* a = std::getenv("EASEL_RECORD_MIC_API")) {
            std::string v = lowerCopy(a);
            if (v == "mme") { mic.useMme = true; mic.apiForce = 2; }
            else if (v == "wasapi") mic.apiForce = 1;
        }
        m_audioSources.push_back(std::move(micPtr));
    }

    // Open lanes now; a lane that fails here is retried during recording, so a
    // device that is mid-restart at record start still comes up. WASAPI opens
    // return promptly even on this box's degraded devices; only MME opens can
    // hang, and those are deferred to the async reinit workers.
    for (auto& s : m_audioSources)
        if (!s->useMme) initAudioSource(*s);
    return !m_audioSources.empty();
}

void VideoRecorder::teardownAudioSource(AudioSource& s) {
    if (s.client) { ((IAudioClient*)s.client)->Stop(); ((IAudioClient*)s.client)->Release(); s.client = nullptr; }
    if (s.capture) { ((IAudioCaptureClient*)s.capture)->Release(); s.capture = nullptr; }
    if (s.device) { ((IMMDevice*)s.device)->Release(); s.device = nullptr; }
    if (s.mme) {
        MmeCapture* m = (MmeCapture*)s.mme;
        if (m->h) {
            waveInStop(m->h);
            waveInReset(m->h);
            for (auto& h : m->hdr)
                if (h.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(m->h, &h, sizeof(WAVEHDR));
            waveInClose(m->h);
        }
        delete m;
        s.mme = nullptr;
    }
    if (s.swr) swr_free(&s.swr);
    s.ok = false;
}

// MME open for a capture lane. Device chosen by matching the lane's resolved
// endpoint name against MME's (31-char-truncated) device names; falls back to
// the wave mapper. Fixed 48k/16-bit/stereo — the shared engine formats MME
// streams regardless of the device's native layout.
bool VideoRecorder::initAudioSourceMme(AudioSource& s) {
    UINT devId = WAVE_MAPPER;
    std::string want = lowerCopy(s.name);
    UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::string nm = lowerCopy(caps.szPname);
        if (!want.empty() && !nm.empty() && want.compare(0, nm.size(), nm) == 0) { devId = i; break; }
    }
    if (devId == WAVE_MAPPER && !want.empty())
        std::cerr << "[REC] Audio: " << s.label << " MME: no device named like '"
                  << s.name << "', using wave mapper (default)" << std::endl;

    MmeCapture* m = new MmeCapture();
    s.mme = m;

    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kMixRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = kMixRate * 4;
    MMRESULT mr = waveInOpen(&m->h, devId, &wf, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        if (s.lastFailLogUs == 0 || s.lastReinitUs - s.lastFailLogUs > 30000000) {
            s.lastFailLogUs = s.lastReinitUs;
            std::cerr << "[REC] Audio: " << s.label << " MME waveInOpen failed (mr=" << mr << ")" << std::endl;
        }
        teardownAudioSource(s);
        return false;
    }
    for (int i = 0; i < MmeCapture::kBufs; i++) {
        m->buf[i].resize((size_t)MmeCapture::kFramesPerBuf * 2);
        m->hdr[i].lpData = (LPSTR)m->buf[i].data();
        m->hdr[i].dwBufferLength = (DWORD)(m->buf[i].size() * sizeof(int16_t));
        waveInPrepareHeader(m->h, &m->hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(m->h, &m->hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(m->h);

    s.rate = kMixRate;
    s.ch = 2;
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, kMixCh);
    int ret = swr_alloc_set_opts2(&s.swr,
        &stereo, AV_SAMPLE_FMT_FLT, kMixRate,
        &stereo, AV_SAMPLE_FMT_FLT, s.rate,
        0, nullptr);
    av_channel_layout_uninit(&stereo);
    if (ret < 0 || !s.swr || swr_init(s.swr) < 0) { teardownAudioSource(s); return false; }

    s.ok = true;
    s.openedUs = av_gettime_relative();
    s.lastDataUs = 0;
    std::cout << "[REC] Audio: " << s.label << " capture via MME on '"
              << (s.name.empty() ? "default" : s.name) << "'" << std::endl;
    return true;
}

bool VideoRecorder::readAudioSourceMme(AudioSource& s) {
    MmeCapture* m = (MmeCapture*)s.mme;
    bool gotData = false;
    std::vector<float>& fbuf = s.extractBuf;   // s.ch==2 on MME lanes, so appendResampled won't alias it
    for (int i = 0; i < MmeCapture::kBufs; i++) {
        WAVEHDR& h = m->hdr[i];
        if (!(h.dwFlags & WHDR_DONE)) continue;
        int frames = (int)(h.dwBytesRecorded / 4);
        if (frames > 0) {
            gotData = true;
            const int16_t* src = (const int16_t*)h.lpData;
            fbuf.resize((size_t)frames * 2);
            bool nonSilent = false;
            for (int j = 0; j < frames * 2; j++) {
                fbuf[j] = src[j] / 32768.0f;
                if (!nonSilent && (src[j] > 64 || src[j] < -64)) nonSilent = true;
            }
            if (nonSilent && !s.sawSignal) {
                s.sawSignal = true;
                std::cout << "[REC] Audio: first non-silent " << s.label
                          << " packet (MME, " << frames << " frames)" << std::endl;
            }
            appendResampled(s, fbuf.data(), frames);
        }
        h.dwFlags &= ~WHDR_DONE;
        h.dwBytesRecorded = 0;
        waveInAddBuffer(m->h, &h, sizeof(WAVEHDR));
    }
    return gotData;
}

bool VideoRecorder::initAudioSource(AudioSource& s) {
    teardownAudioSource(s);
    s.lastReinitUs = av_gettime_relative();

    if (s.useMme) {
        if (initAudioSourceMme(s)) return true;
        if (s.apiForce == 0) s.useMme = false;   // auto mode alternates APIs across retries
        return false;
    }

    // Failures are expected transients (device mid-restart) retried every ~2s;
    // log the stage+hr so a PERSISTENT failure is diagnosable, throttled to 30s.
    auto fail = [&](const char* stage, HRESULT fhr) {
        if (s.lastReinitUs - s.lastFailLogUs > 30000000) {
            s.lastFailLogUs = s.lastReinitUs;
            std::cerr << "[REC] Audio: " << s.label << " open failed at " << stage
                      << " (hr=0x" << std::hex << (unsigned long)fhr << std::dec
                      << "), retrying every 2s" << std::endl;
        }
        teardownAudioSource(s);
        return false;
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&enumerator);
    if (FAILED(hr)) return fail("enumerator", hr);

    IMMDevice* device = nullptr;
    if (!s.id.empty()) {
        int idLen = (int)s.id.size() + 1;
        std::vector<wchar_t> wid(idLen);
        mbstowcs(wid.data(), s.id.c_str(), idLen);
        hr = enumerator->GetDevice(wid.data(), &device);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(s.isCapture ? eCapture : eRender, eConsole, &device);
    }
    enumerator->Release();
    if (FAILED(hr) || !device) return fail("get-device", hr);
    s.device = device;

    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) return fail("activate", hr);
    s.client = client;

    WAVEFORMATEX* mixFmt = nullptr;
    hr = client->GetMixFormat(&mixFmt);
    if (FAILED(hr)) return fail("mix-format", hr);

    s.rate = mixFmt->nSamplesPerSec;
    s.ch = mixFmt->nChannels;

    bool isFloat = false;
    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFmt->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mixFmt;
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    if (!isFloat)
        std::cerr << "[REC] Audio: " << s.label << " device format is not IEEE float — audio may not work correctly" << std::endl;

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defaultPeriod, &minPeriod);
    REFERENCE_TIME bufferDuration = defaultPeriod > 0 ? defaultPeriod * 4 : 2000000;

    DWORD streamFlags = s.isCapture ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, mixFmt, nullptr);
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) return fail("initialize", hr);

    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr)) return fail("capture-service", hr);
    s.capture = capture;

    // The resampler always sees stereo input: appendResampled() extracts the
    // first two channels from >2ch devices (no default FFmpeg layout exists for
    // some counts, e.g. virtual cables exposing 8/16ch).
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, kMixCh);
    av_channel_layout_default(&outLayout, kMixCh);
    int ret = swr_alloc_set_opts2(&s.swr,
        &outLayout, AV_SAMPLE_FMT_FLT, kMixRate,
        &inLayout, AV_SAMPLE_FMT_FLT, s.rate,
        0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || !s.swr || swr_init(s.swr) < 0) return fail("resampler", ret);

    hr = client->Start();
    if (FAILED(hr)) return fail("start", hr);
    s.ok = true;
    s.openedUs = av_gettime_relative();
    s.lastDataUs = 0;
    std::cout << "[REC] Audio: " << s.label << " capture on '"
              << (s.name.empty() ? std::string("default ") + (s.isCapture ? "microphone" : "output (loopback)") : s.name)
              << "' rate=" << s.rate << " ch=" << s.ch
              << " gain=" << s.gain << std::endl;
    return true;
}

void VideoRecorder::appendResampled(AudioSource& s, const float* data, int frames) {
    // Fold the device's channel count to stereo before the resampler (which is
    // pinned to a stereo input layout): mono duplicates, >2ch takes the first pair.
    if (s.ch != kMixCh) {
        s.extractBuf.resize((size_t)frames * kMixCh);
        for (int i = 0; i < frames; i++) {
            const float* f = data + (size_t)i * s.ch;
            s.extractBuf[(size_t)i * 2]     = f[0];
            s.extractBuf[(size_t)i * 2 + 1] = s.ch > 1 ? f[1] : f[0];
        }
        data = s.extractBuf.data();
    }

    int outCap = (int)av_rescale_rnd(swr_get_delay(s.swr, s.rate) + frames,
                                     kMixRate, s.rate, AV_ROUND_UP) + 64;
    size_t base = s.fifo.size();
    s.fifo.resize(base + (size_t)outCap * kMixCh);
    uint8_t* outPlanes[1] = { (uint8_t*)(s.fifo.data() + base) };
    const uint8_t* inPlanes[1] = { (const uint8_t*)data };
    int got = swr_convert(s.swr, outPlanes, outCap, inPlanes, frames);
    if (got < 0) {
        if (s.statSwrErrLogs < 3) {
            s.statSwrErrLogs++;
            char err[128]; av_strerror(got, err, sizeof(err));
            std::cerr << "[REC] Audio: " << s.label << " swr_convert failed: " << err << std::endl;
        }
        got = 0;
    }
    s.fifo.resize(base + (size_t)got * kMixCh);
    s.produced += got;
    s.statAppended += got;
}

bool VideoRecorder::readAudioSource(AudioSource& s) {
    if (s.useMme) return readAudioSourceMme(s);

    IAudioCaptureClient* capture = (IAudioCaptureClient*)s.capture;
    bool gotData = false;

    UINT32 packetLength = 0;
    HRESULT hr = capture->GetNextPacketSize(&packetLength);
    while (SUCCEEDED(hr) && packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        hr = capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (numFrames > 0) {
            gotData = true;
            std::vector<float> silence;
            const float* src = (const float*)data;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                silence.assign((size_t)numFrames * s.ch, 0.0f);
                src = silence.data();
            } else if (!s.sawSignal) {
                s.sawSignal = true;
                std::cout << "[REC] Audio: first non-silent " << s.label
                          << " packet (" << numFrames << " frames)" << std::endl;
            }
            appendResampled(s, src, (int)numFrames);
        }
        capture->ReleaseBuffer(numFrames);
        hr = capture->GetNextPacketSize(&packetLength);
    }

    if (FAILED(hr)) {
        // Endpoint died (device restart / driver reset). Keep the lane's config
        // and let drainAudio() re-open it; silence-fill covers the gap.
        std::cerr << "[REC] Audio: " << s.label << " capture lost (hr=0x"
                  << std::hex << hr << std::dec << "), will reconnect" << std::endl;
        teardownAudioSource(s);
    }
    return gotData;
}
#else
bool VideoRecorder::initAudioCapture() {
    std::cerr << "[REC] Audio capture not implemented on this platform" << std::endl;
    return false;
}
#endif

// ─── FFmpeg encoder ─────────────────────────────────────────────────

bool VideoRecorder::initEncoder(const std::string& path, int width, int height, int fps) {
    m_width = width;
    m_height = height;

#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Auto-detect container from extension (mp4, mkv, etc.)
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, nullptr, path.c_str());
    if (ret < 0 || !m_fmtCtx) {
        std::cerr << "[REC] Failed to create output context" << std::endl;
        return false;
    }

    // ── Video stream (H.264) ──
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) return false;

    m_videoStream = avformat_new_stream(m_fmtCtx, videoCodec);
    m_videoCodecCtx = avcodec_alloc_context3(videoCodec);
    if (!m_videoStream || !m_videoCodecCtx) return false;

    // Ensure even dimensions
    m_width = width & ~1;
    m_height = height & ~1;

    m_videoCodecCtx->width = m_width;
    m_videoCodecCtx->height = m_height;
    // Fine time_base for true VFR: PTS is wall-clock-derived (1/90000 ticks), NOT a
    // frame counter, so playback runs at correct speed at whatever rate the app renders.
    // framerate stays as a hint (max expected rate) feeding gop cadence + metadata.
    m_videoCodecCtx->time_base = {1, 90000};
    m_videoCodecCtx->framerate = {fps, 1};
    m_videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_videoCodecCtx->gop_size = fps * 2;
    m_videoCodecCtx->max_b_frames = 0;   // no reorder → pts==dts (VFR-safe) + lower latency
    m_videoCodecCtx->bit_rate = 8000000; // 8 Mbps for local recording

    // Real-time feed: match the live-encode sibling (RTMPOutput) rather than an
    // offline-transcode config, so the encoder keeps pace with the render rate.
    av_opt_set(m_videoCodecCtx->priv_data, "preset", "veryfast", 0);
    av_opt_set(m_videoCodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_videoCodecCtx->priv_data, "profile", "high", 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_videoCodecCtx, videoCodec, nullptr);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to open video encoder: " << err << std::endl;
        return false;
    }

    avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCodecCtx);
    m_videoStream->time_base = m_videoCodecCtx->time_base;

    m_videoFrame = av_frame_alloc();
    m_videoFrame->format = AV_PIX_FMT_YUV420P;
    m_videoFrame->width = m_width;
    m_videoFrame->height = m_height;
    av_frame_get_buffer(m_videoFrame, 0);

    m_srcW = m_width;
    m_srcH = m_height;
    m_swsCtx = sws_getContext(m_srcW, m_srcH, AV_PIX_FMT_RGBA,
                               m_width, m_height, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) return false;

    // ── Audio stream (AAC) ──
    bool hasAudio = initAudioCapture();

#ifdef _WIN32
    if (hasAudio) {
        const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (audioCodec) {
            m_audioStream = avformat_new_stream(m_fmtCtx, audioCodec);
            m_audioCodecCtx = avcodec_alloc_context3(audioCodec);

            m_audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            m_audioCodecCtx->sample_rate = m_wasapiSampleRate;
            av_channel_layout_default(&m_audioCodecCtx->ch_layout, 2);
            m_audioCodecCtx->bit_rate = 192000;

            if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                m_audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            ret = avcodec_open2(m_audioCodecCtx, audioCodec, nullptr);
            if (ret < 0) {
                avcodec_free_context(&m_audioCodecCtx);
                m_audioCodecCtx = nullptr;
                m_audioStream = nullptr;
                hasAudio = false;
            }
        } else {
            hasAudio = false;
        }

        if (hasAudio) {
            avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx);
            m_audioStream->time_base = {1, m_audioCodecCtx->sample_rate};

            m_audioFrameSize = m_audioCodecCtx->frame_size;

            m_audioFrame = av_frame_alloc();
            m_audioFrame->format = AV_SAMPLE_FMT_FLTP;
            m_audioFrame->sample_rate = m_audioCodecCtx->sample_rate;
            av_channel_layout_copy(&m_audioFrame->ch_layout, &m_audioCodecCtx->ch_layout);
            m_audioFrame->nb_samples = m_audioFrameSize;
            av_frame_get_buffer(m_audioFrame, 0);

            // Setup resampler: WASAPI interleaved float -> AAC planar float
            // Always use actual WASAPI channel count as input
            AVChannelLayout inLayout;
            av_channel_layout_default(&inLayout, m_wasapiChannels);
            ret = swr_alloc_set_opts2(&m_swrCtx,
                &m_audioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, m_wasapiSampleRate,
                &inLayout, AV_SAMPLE_FMT_FLT, m_wasapiSampleRate,
                0, nullptr);
            av_channel_layout_uninit(&inLayout);

            if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
                std::cerr << "[REC] Audio: swr_init failed" << std::endl;
                cleanupAudio();
                hasAudio = false;
            } else {
                std::cout << "[REC] Audio encoder ready: AAC " << m_wasapiSampleRate << "Hz, "
                          << m_wasapiChannels << "ch -> stereo" << std::endl;
            }
            m_audioAccum.clear();
        }
    }
#else
    (void)hasAudio;
    hasAudio = false;
#endif

    if (!hasAudio) {
        std::cerr << "[REC] WARNING: Recording without audio!" << std::endl;
    }

    // ── Open file and write header ──
    m_packet = av_packet_alloc();

    ret = avio_open(&m_fmtCtx->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to open file: " << err << std::endl;
        return false;
    }

    // Fragmented MP4: a plain mp4 only gets its moov index at stop(), so an
    // app/OS death mid-recording (this box has a Kernel-Power history) loses
    // the ENTIRE file. Fragments make everything up to the last GOP (~2s)
    // playable. mkv/other containers don't take these flags — skip them there.
    AVDictionary* muxOpts = nullptr;
    if (m_fmtCtx->oformat && m_fmtCtx->oformat->name &&
        std::string(m_fmtCtx->oformat->name) == "mp4") {
        av_dict_set(&muxOpts, "movflags",
                    "+frag_keyframe+empty_moov+default_base_moof", 0);
    }
    ret = avformat_write_header(m_fmtCtx, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0) {
        char err[128]; av_strerror(ret, err, sizeof(err));
        std::cerr << "[REC] Failed to write header: " << err << std::endl;
        return false;
    }

    return true;
}

// ─── GL readback ────────────────────────────────────────────────────

void VideoRecorder::sendFrame(GLuint texture, int w, int h) {
    // Resize tolerance: never bail on a size change. The recorder is pinned to its
    // start size while the live zone can be resized (canvas preset, projector native
    // res, zone switch); bailing here silently killed the recording. Read back
    // whatever size the zone is now — the encode thread rescales to the fixed size.
    if (!m_active || w <= 0 || h <= 0) return;

    size_t bytes = (size_t)w * h * 4;

    // (Re)create PBOs on first use or whenever the incoming size changes.
    if (!m_pbo[0] || bytes != m_pboBytes) {
        if (m_pbo[0]) glDeleteBuffers(2, m_pbo);
        glGenBuffers(2, m_pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pboIndex = 0;
        m_pboReady = false;
        m_pboBytes = bytes;
    }

    int readPBO = m_pboIndex;
    int mapPBO = 1 - m_pboIndex;

    // Kick off async readback into current PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readPBO]);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Map previous PBO (already complete) and push it onto the encode queue.
    // The mapped PBO holds the frame from the PREVIOUS call, hence m_prevW/m_prevH.
    if (m_pboReady) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[mapPBO]);
        void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (ptr) {
            size_t prevBytes = (size_t)m_prevW * m_prevH * 4;
            int64_t captureUs = av_gettime_relative();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Recycle a pooled buffer to avoid a per-frame multi-MB allocation.
                std::vector<uint8_t> buf;
                if (!m_bufPool.empty()) { buf = std::move(m_bufPool.back()); m_bufPool.pop_back(); }
                buf.resize(prevBytes);
                std::memcpy(buf.data(), ptr, prevBytes);
                // Bound the queue: drop the OLDEST frame under sustained overload,
                // recycling its buffer. Wall-clock PTS keeps playback speed correct
                // across the gap (it just shows fewer unique frames there).
                if (m_frameQueue.size() >= m_maxQueue) {
                    m_bufPool.push_back(std::move(m_frameQueue.front().pixels));
                    m_frameQueue.pop_front();
                }
                RecFrame f;
                f.w = m_prevW;
                f.h = m_prevH;
                f.captureUs = captureUs;
                f.pixels = std::move(buf);
                m_frameQueue.push_back(std::move(f));
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            m_cv.notify_one();
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_prevW = w;
    m_prevH = h;
    m_pboIndex = 1 - m_pboIndex;
    m_pboReady = true;
}

// ─── Audio drain ────────────────────────────────────────────────────

void VideoRecorder::drainAudio() {
    // Heartbeat BEFORE the guard: proves whether this function runs and what
    // state the guard actually sees (diagnosing silent audio-death in-app).
    {
        int64_t hb = av_gettime_relative();
        if (m_hbUs == 0) m_hbUs = hb;
        if (hb - m_hbUs > 10000000) {
            m_hbUs = hb;
            std::cout << "[REC] drain hb: this=" << (const void*)this
                      << " sources=" << m_audioSources.size()
                      << " codec=" << (const void*)m_audioCodecCtx
                      << " active=" << m_active.load() << std::endl;
        }
    }
    if (m_audioSources.empty() || !m_audioCodecCtx) return;

#ifdef _WIN32
    int64_t nowUs = av_gettime_relative();
    if (m_audioStartUs == 0) m_audioStartUs = nowUs;
    int64_t target = (nowUs - m_audioStartUs) * (int64_t)kMixRate / 1000000;

    for (auto& sp : m_audioSources) {
        AudioSource& s = *sp;

        // A capture endpoint that is open and "running" but delivers nothing is
        // this box's known Focusrite defect — mark the lane for the MME path.
        // Loopback lanes are exempt: an idle render device legitimately delivers
        // no packets, and MME has no loopback at all.
        if (s.ok.load() && s.isCapture && !s.useMme && s.apiForce == 0) {
            int64_t ref = s.lastDataUs ? s.lastDataUs : s.openedUs;
            if (ref && nowUs - ref > 5000000) {
                std::cerr << "[REC] Audio: " << s.label
                          << " capture silent for 5s — switching to MME" << std::endl;
                s.useMme = true;
                s.ok = false;
                s.lastReinitUs = 0;   // reinit worker picks it up promptly
            }
        }

        // Re-open a dead lane every ~2s — covers a device restart mid-recording.
        // The open runs on a DETACHED worker: driver calls on this box can hang
        // outright (a wedged Focusrite blocks waveInOpen forever), and a hung
        // open must park the lane on silence-fill, never the encode thread.
        if (!s.ok.load() && s.initState.load() == 0 && nowUs - s.lastReinitUs > 2000000) {
            s.initState = 1;
            s.lastReinitUs = nowUs;
            AudioSource* lane = &s;
            std::thread([this, lane]() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (initAudioSource(*lane))
                    std::cout << "[REC] Audio: " << lane->label << " capture reconnected ("
                              << (lane->useMme ? "MME" : "WASAPI") << ")" << std::endl;
                lane->initState = 0;
                CoUninitialize();
            }).detach();
        }

        bool gotData = s.ok.load() ? readAudioSource(s) : false;
        if (gotData) { s.statGot++; s.lastDataUs = nowUs; }

        // WASAPI loopback delivers NO packets while the audio engine is idle, and a
        // dead lane delivers nothing at all. Without this the lane's timeline falls
        // behind wall-clock, desyncing the mix and shortening the track. When no
        // packets arrived this cycle, top up with silence.
        if (!gotData) {
            int64_t deficit = target - s.produced;
            if (deficit >= kMixRate / 50) {                        // only fill gaps >= ~20ms
                int n = (int)std::min<int64_t>(deficit, kMixRate); // cap a single fill to 1s
                s.fifo.insert(s.fifo.end(), (size_t)n * kMixCh, 0.0f);
                s.produced += n;
            }
        }

        // Safety valve: a lane running far ahead of the others (clock drift gone
        // wrong) would otherwise grow its FIFO without bound.
        size_t maxBacklog = (size_t)kMixRate * 3 * kMixCh;
        if (s.fifo.size() > maxBacklog) {
            std::cerr << "[REC] Audio: " << s.label << " backlog trim ("
                      << s.fifo.size() / kMixCh << " frames)" << std::endl;
            s.fifo.erase(s.fifo.begin(), s.fifo.end() - maxBacklog);
        }
    }

    // Periodic per-lane truth dump while the path is under diagnosis.
    if (m_lastStatsUs == 0) m_lastStatsUs = nowUs;
    if (nowUs - m_lastStatsUs > 10000000) {
        m_lastStatsUs = nowUs;
        std::cout << "[REC] Audio stats: target=" << target << " mixed=" << m_statMixed
                  << " encoded=" << m_audioSamplesWritten;
        for (auto& sp : m_audioSources) {
            AudioSource& s = *sp;
            std::cout << " | " << s.label << " ok=" << s.ok.load()
                      << " mme=" << s.useMme
                      << " produced=" << s.produced
                      << " fifo=" << s.fifo.size() / kMixCh
                      << " gotCycles=" << s.statGot
                      << " appended=" << s.statAppended;
            s.statGot = 0; s.statAppended = 0;
        }
        std::cout << std::endl;
    }

    // Mix as many sample-aligned frames as every lane can supply.
    size_t mixFrames = SIZE_MAX;
    for (auto& sp : m_audioSources)
        mixFrames = std::min(mixFrames, sp->fifo.size() / kMixCh);
    if (mixFrames == 0 || mixFrames == SIZE_MAX) return;

    size_t mixFloats = mixFrames * kMixCh;
    m_mixScratch.assign(mixFloats, 0.0f);
    for (auto& sp : m_audioSources) {
        AudioSource& s = *sp;
        for (size_t i = 0; i < mixFloats; i++)
            m_mixScratch[i] += s.fifo[i] * s.gain;
        s.fifo.erase(s.fifo.begin(), s.fifo.begin() + mixFloats);
    }
    for (auto& v : m_mixScratch)
        v = std::clamp(v, -1.0f, 1.0f);

    if (!m_loggedFirstMix) {
        m_loggedFirstMix = true;
        std::cout << "[REC] Audio: first mix (" << mixFrames << " frames, "
                  << m_audioSources.size() << " lanes)" << std::endl;
    }
    m_statMixed += (int64_t)mixFrames;
    encodeAudioSamples(m_mixScratch.data(), (int)mixFrames, kMixCh);
#endif
}

void VideoRecorder::encodeAudioSamples(const float* data, int numSamples, int channels) {
    int totalFloats = numSamples * channels;
    m_audioAccum.insert(m_audioAccum.end(), data, data + totalFloats);

    int samplesPerFrame = m_audioFrameSize;
    int floatsPerFrame = samplesPerFrame * channels;

    while ((int)m_audioAccum.size() >= floatsPerFrame) {
        av_frame_make_writable(m_audioFrame);
        m_audioFrame->nb_samples = samplesPerFrame;

        const uint8_t* inData[1] = { (const uint8_t*)m_audioAccum.data() };
        int converted = swr_convert(m_swrCtx,
            m_audioFrame->data, samplesPerFrame,
            inData, samplesPerFrame);

        if (converted > 0) {
            m_audioFrame->pts = m_audioSamplesWritten;
            m_audioSamplesWritten += converted;

            int ret = avcodec_send_frame(m_audioCodecCtx, m_audioFrame);
            if (ret < 0 && m_audioSendErrLogs < 3) {
                m_audioSendErrLogs++;
                char err[128]; av_strerror(ret, err, sizeof(err));
                std::cerr << "[REC] Audio: send_frame failed: " << err << std::endl;
            }
            while (ret >= 0) {
                ret = avcodec_receive_packet(m_audioCodecCtx, m_packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;
                av_packet_rescale_ts(m_packet,
                    m_audioCodecCtx->time_base, m_audioStream->time_base);
                m_packet->stream_index = m_audioStream->index;
                if (m_audioPktLogs < 2) {
                    m_audioPktLogs++;
                    std::cout << "[REC] Audio: muxing packet pts=" << m_packet->pts
                              << " (codec_tb=" << m_audioCodecCtx->time_base.num << "/" << m_audioCodecCtx->time_base.den
                              << " stream_tb=" << m_audioStream->time_base.num << "/" << m_audioStream->time_base.den
                              << ")" << std::endl;
                }
                int wret = av_interleaved_write_frame(m_fmtCtx, m_packet);
                if (wret < 0 && m_audioWriteErrLogs < 3) {
                    m_audioWriteErrLogs++;
                    char err[128]; av_strerror(wret, err, sizeof(err));
                    std::cerr << "[REC] Audio: write_frame failed: " << err << std::endl;
                }
                av_packet_unref(m_packet);
            }
        }
        m_audioAccum.erase(m_audioAccum.begin(), m_audioAccum.begin() + floatsPerFrame);
    }
}

// ─── Encode thread ──────────────────────────────────────────────────

void VideoRecorder::encodeThread() {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Pull one frame off the queue (returns false if empty). Encode happens
    // outside the lock so the GL thread is never blocked on the encoder.
    auto popFrame = [this](RecFrame& out) {
        std::lock_guard<std::mutex> g(m_mutex);
        if (m_frameQueue.empty()) return false;
        out = std::move(m_frameQueue.front());
        m_frameQueue.pop_front();
        return true;
    };
    auto recycle = [this](RecFrame& f) {
        std::lock_guard<std::mutex> g(m_mutex);
        if (m_bufPool.size() < m_maxQueue + 2) m_bufPool.push_back(std::move(f.pixels));
    };

    while (!m_stopRequested) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !m_frameQueue.empty() || m_stopRequested.load();
            });
        }

        // Always drain audio, even on the stop iteration
        drainAudio();

        // Drain ALL queued video frames this wakeup (no one-frame-per-wakeup
        // throttle) — but keep draining audio at a bounded cadence while doing
        // so. Under CPU contention the GL thread refills the queue faster than
        // frames encode, so this loop can run for minutes without exiting; if
        // audio only drains on queue-empty, the audio track silently dies.
        RecFrame f;
        int64_t lastDrainUs = av_gettime_relative();
        while (popFrame(f)) {
            encodeVideoFrame(f);
            recycle(f);
            int64_t nowUs = av_gettime_relative();
            if (nowUs - lastDrainUs > 20000) {   // ≥ every 20ms
                drainAudio();
                lastDrainUs = nowUs;
            }
        }
    }

    // Encode any frames still queued at stop time so the tail isn't lost.
    {
        RecFrame f;
        while (popFrame(f)) {
            encodeVideoFrame(f);
            recycle(f);
        }
    }

    // Final audio drain
    drainAudio();

    std::cout << "[REC] Flushing encoders (audio samples written: " << m_audioSamplesWritten << ")" << std::endl;

    // Flush encoders
    if (m_videoCodecCtx) {
        avcodec_send_frame(m_videoCodecCtx, nullptr);
        while (avcodec_receive_packet(m_videoCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
            m_packet->stream_index = m_videoStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }
    if (m_audioCodecCtx) {
        avcodec_send_frame(m_audioCodecCtx, nullptr);
        while (avcodec_receive_packet(m_audioCodecCtx, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_audioCodecCtx->time_base, m_audioStream->time_base);
            m_packet->stream_index = m_audioStream->index;
            av_interleaved_write_frame(m_fmtCtx, m_packet);
            av_packet_unref(m_packet);
        }
    }

    if (m_fmtCtx && m_fmtCtx->pb)
        av_write_trailer(m_fmtCtx);

#ifdef _WIN32
    CoUninitialize();
#endif
}

void VideoRecorder::encodeVideoFrame(const RecFrame& f) {
    if (f.w <= 0 || f.h <= 0 || f.pixels.empty()) return;
    av_frame_make_writable(m_videoFrame);

    // Rebuild the scaler if the incoming frame size changed (zone/canvas resize
    // mid-recording). We always scale to the fixed encoder size m_width x m_height.
    if (f.w != m_srcW || f.h != m_srcH || !m_swsCtx) {
        if (m_swsCtx) sws_freeContext(m_swsCtx);
        m_srcW = f.w;
        m_srcH = f.h;
        m_swsCtx = sws_getContext(m_srcW, m_srcH, AV_PIX_FMT_RGBA,
                                  m_width, m_height, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_swsCtx) return;
    }

    const uint8_t* srcSlice[1] = { f.pixels.data() };
    int srcStride[1] = { m_srcW * 4 };
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_srcH,
              m_videoFrame->data, m_videoFrame->linesize);

    // Wall-clock VFR PTS: stamp each frame at its real elapsed capture time (anchored
    // on the first frame) so playback runs at true speed and tracks the live render fps.
    if (m_startPtsUs == 0) m_startPtsUs = f.captureUs;
    int64_t elapsedUs = f.captureUs - m_startPtsUs;
    if (elapsedUs < 0) elapsedUs = 0;
    AVRational usTb = {1, 1000000};
    int64_t pts = av_rescale_q(elapsedUs, usTb, m_videoCodecCtx->time_base);
    if (pts <= m_lastVideoPts) pts = m_lastVideoPts + 1; // strictly monotonic
    m_lastVideoPts = pts;
    m_videoFrame->pts = pts;
    m_videoFrameIndex++;

    int ret = avcodec_send_frame(m_videoCodecCtx, m_videoFrame);
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_videoCodecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        av_packet_rescale_ts(m_packet, m_videoCodecCtx->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;
        av_interleaved_write_frame(m_fmtCtx, m_packet);
        av_packet_unref(m_packet);
    }

    // Feed-stall detector: we just muxed video, so a stagnant encoded-sample
    // count means the audio feed to the muxer has died (starved drain, wedged
    // driver read, ...) — the exact failure that previously left zero log
    // trace for hours. Warn after 5s of stall, throttled to once per 30s.
    if (m_audioCodecCtx && m_audioStartUs != 0) {
        int64_t nowUs = av_gettime_relative();
        if (m_audioSamplesWritten != m_lastAudioMuxSamples || m_lastAudioMuxUs == 0) {
            m_lastAudioMuxSamples = m_audioSamplesWritten;
            m_lastAudioMuxUs = nowUs;
        } else if (nowUs - m_lastAudioMuxUs > 5000000 &&
                   nowUs - m_stallLogUs > 30000000) {
            m_stallLogUs = nowUs;
            int64_t target = (nowUs - m_audioStartUs) * (int64_t)kMixRate / 1000000;
            std::cerr << "[REC] Audio: feed stalled — no samples muxed for "
                      << (nowUs - m_lastAudioMuxUs) / 1000000 << "s"
                      << " (encoded=" << m_audioSamplesWritten
                      << " mixed=" << m_statMixed
                      << " target=" << target
                      << " videoFrames=" << m_videoFrameIndex << ")" << std::endl;
        }
    }
}

// ─── Cleanup ────────────────────────────────────────────────────────

void VideoRecorder::cleanupAudio() {
#ifdef _WIN32
    // A lane whose async init worker is hung inside a wedged driver call must
    // NOT be destroyed (the worker holds a pointer to it) and must not be torn
    // down here (teardown of a wedged device can hang too). Leak it knowingly.
    static std::vector<std::unique_ptr<AudioSource>> s_zombieLanes;
    for (auto& sp : m_audioSources) {
        if (sp->initState.load() != 0) {
            std::cerr << "[REC] Audio: " << sp->label
                      << " lane leaked (init worker hung in driver call)" << std::endl;
            s_zombieLanes.push_back(std::move(sp));
        } else {
            teardownAudioSource(*sp);
        }
    }
#endif
    m_audioSources.clear();
    m_mixScratch.clear();
    m_audioSendErrLogs = m_audioWriteErrLogs = m_audioPktLogs = 0;
    m_loggedFirstMix = false;
    if (m_swrCtx) { swr_free(&m_swrCtx); m_swrCtx = nullptr; }
    if (m_audioFrame) { av_frame_free(&m_audioFrame); }
    if (m_audioCodecCtx) { avcodec_free_context(&m_audioCodecCtx); }
    m_audioStream = nullptr;
    m_audioAccum.clear();
}

void VideoRecorder::cleanup() {
    cleanupAudio();
    if (m_pbo[0]) { glDeleteBuffers(2, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
    m_pboReady = false;
    m_pboIndex = 0;
    m_pboBytes = 0;
    m_prevW = m_prevH = 0;
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_videoFrame) { av_frame_free(&m_videoFrame); }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); }
    if (m_fmtCtx) {
        if (m_fmtCtx->pb) avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStream = nullptr;
    m_frameQueue.clear();
    m_bufPool.clear();
}

#endif // HAS_FFMPEG
