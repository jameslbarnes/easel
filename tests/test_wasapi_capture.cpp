// Standalone WASAPI loopback capture test
// Tests if system audio can be captured via WASAPI loopback
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Get default render endpoint
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        std::cerr << "FAIL: CoCreateInstance for MMDeviceEnumerator (hr=0x" << std::hex << hr << ")" << std::endl;
        return 1;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        std::cerr << "FAIL: GetDefaultAudioEndpoint (hr=0x" << std::hex << hr << ")" << std::endl;
        enumerator->Release();
        return 1;
    }

    // Get device name
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name))) {
            std::wcout << L"Default render device: " << name.pwszVal << std::endl;
            PropVariantClear(&name);
        }
        props->Release();
    }

    // Test IAudioMeterInformation
    IAudioMeterInformation* meter = nullptr;
    hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
    if (SUCCEEDED(hr) && meter) {
        std::cout << "IAudioMeterInformation: OK" << std::endl;
        for (int i = 0; i < 5; i++) {
            float peak = 0;
            meter->GetPeakValue(&peak);
            std::cout << "  Peak level: " << peak << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        meter->Release();
    } else {
        std::cerr << "FAIL: IAudioMeterInformation activation (hr=0x" << std::hex << hr << ")" << std::endl;
    }

    // Test WASAPI loopback capture
    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr) || !audioClient) {
        std::cerr << "FAIL: IAudioClient activation (hr=0x" << std::hex << hr << ")" << std::endl;
        device->Release();
        enumerator->Release();
        return 1;
    }

    WAVEFORMATEX* mixFmt = nullptr;
    audioClient->GetMixFormat(&mixFmt);
    std::cout << "Mix format: " << mixFmt->nSamplesPerSec << "Hz, "
              << mixFmt->nChannels << "ch, "
              << mixFmt->wBitsPerSample << "bit" << std::endl;

    // Check format tag
    if (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        std::cout << "Format: IEEE float" << std::endl;
    } else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* wfx = (WAVEFORMATEXTENSIBLE*)mixFmt;
        if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            std::cout << "Format: EXTENSIBLE (IEEE float)" << std::endl;
        } else if (wfx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            std::cout << "Format: EXTENSIBLE (PCM integer)" << std::endl;
        } else {
            std::cout << "Format: EXTENSIBLE (unknown subformat)" << std::endl;
        }
    } else {
        std::cout << "Format tag: " << mixFmt->wFormatTag << std::endl;
    }

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    REFERENCE_TIME bufferDuration = defaultPeriod > 0 ? defaultPeriod * 4 : 2000000;

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  bufferDuration, 0, mixFmt, nullptr);
    if (FAILED(hr)) {
        std::cerr << "FAIL: IAudioClient::Initialize loopback (hr=0x" << std::hex << hr << ")" << std::endl;
        CoTaskMemFree(mixFmt);
        audioClient->Release();
        device->Release();
        enumerator->Release();
        return 1;
    }

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr) || !captureClient) {
        std::cerr << "FAIL: IAudioCaptureClient::GetService (hr=0x" << std::hex << hr << ")" << std::endl;
        CoTaskMemFree(mixFmt);
        audioClient->Release();
        device->Release();
        enumerator->Release();
        return 1;
    }

    int channels = mixFmt->nChannels;
    CoTaskMemFree(mixFmt);

    audioClient->Start();
    std::cout << "WASAPI loopback capture started. Capturing for 3 seconds..." << std::endl;

    int totalFrames = 0;
    int nonSilentFrames = 0;
    float maxSample = 0;
    float rmsAccum = 0;
    int rmsCount = 0;

    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(3)) {
        UINT32 packetLength = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            totalFrames += numFrames;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::cout << "  [SILENT flag set, " << numFrames << " frames]" << std::endl;
            } else if (data) {
                const float* samples = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < numFrames; i++) {
                    float mono = 0;
                    for (int ch = 0; ch < channels; ch++) {
                        mono += samples[i * channels + ch];
                    }
                    mono /= channels;
                    float absMono = std::abs(mono);
                    if (absMono > 0.001f) nonSilentFrames++;
                    if (absMono > maxSample) maxSample = absMono;
                    rmsAccum += mono * mono;
                    rmsCount++;
                }
            }

            captureClient->ReleaseBuffer(numFrames);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    audioClient->Stop();

    float rms = rmsCount > 0 ? std::sqrt(rmsAccum / rmsCount) : 0;

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total frames captured: " << totalFrames << std::endl;
    std::cout << "Non-silent frames: " << nonSilentFrames << std::endl;
    std::cout << "Max sample magnitude: " << maxSample << std::endl;
    std::cout << "RMS level: " << rms << std::endl;

    if (totalFrames == 0) {
        std::cout << "\nDIAGNOSIS: No frames received at all. WASAPI loopback may not be supported on this device." << std::endl;
    } else if (nonSilentFrames == 0) {
        std::cout << "\nDIAGNOSIS: Frames received but all silent. Audio may be playing through a different device." << std::endl;
    } else {
        std::cout << "\nDIAGNOSIS: Audio capture working! (" << nonSilentFrames << "/" << totalFrames << " non-silent frames)" << std::endl;
    }

    captureClient->Release();
    audioClient->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
    return 0;
}
