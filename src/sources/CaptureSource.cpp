#include "sources/CaptureSource.h"
#include <iostream>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

CaptureSource::~CaptureSource() {
    stop();
}

std::vector<CaptureMonitorInfo> CaptureSource::enumerateMonitors() {
    std::vector<CaptureMonitorInfo> result;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        return result;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ai++) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);

            CaptureMonitorInfo info;
            // Convert wide string to narrow
            char name[128];
            wcstombs(name, desc.DeviceName, sizeof(name));
            info.name = name;
            info.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            info.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            info.index = (int)result.size();
            result.push_back(info);

            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    return result;
}

bool CaptureSource::start(int monitorIndex) {
    stop();

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0,
        D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device" << std::endl;
        return false;
    }

    // Get DXGI output
    IDXGIDevice* dxgiDevice = nullptr;
    m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    // Find the target output
    IDXGIOutput* output = nullptr;
    int currentIndex = 0;
    IDXGIAdapter1* adapter1 = nullptr;
    IDXGIFactory1* factory = nullptr;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        cleanup();
        return false;
    }

    bool found = false;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter1) != DXGI_ERROR_NOT_FOUND; ai++) {
        for (UINT oi = 0; adapter1->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; oi++) {
            if (currentIndex == monitorIndex) {
                found = true;
                break;
            }
            output->Release();
            output = nullptr;
            currentIndex++;
        }
        adapter1->Release();
        if (found) break;
    }
    factory->Release();

    if (!found || !output) {
        std::cerr << "Monitor index " << monitorIndex << " not found" << std::endl;
        if (adapter) adapter->Release();
        cleanup();
        return false;
    }

    // Get output dimensions
    DXGI_OUTPUT_DESC outputDesc;
    output->GetDesc(&outputDesc);
    m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    // Get IDXGIOutput1 for duplication
    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();

    if (FAILED(hr)) {
        std::cerr << "Failed to get IDXGIOutput1" << std::endl;
        if (adapter) adapter->Release();
        cleanup();
        return false;
    }

    // Create desktop duplication
    hr = output1->DuplicateOutput(m_device, &m_duplication);
    output1->Release();
    if (adapter) adapter->Release();

    if (FAILED(hr)) {
        std::cerr << "Failed to create desktop duplication (hr=0x"
                  << std::hex << hr << ")" << std::endl;
        cleanup();
        return false;
    }

    // Create staging texture for CPU read
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Failed to create staging texture" << std::endl;
        cleanup();
        return false;
    }

    // Allocate pixel buffer (RGBA)
    m_pixelBuffer.resize(m_width * m_height * 4);

    // Create GL texture
    m_texture.createEmpty(m_width, m_height);

    m_active = true;
    return true;
}

void CaptureSource::stop() {
    m_active = false;
    cleanup();
}

void CaptureSource::cleanup() {
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

void CaptureSource::update() {
    if (!m_active || !m_duplication) return;

    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return; // No new frame
    }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Desktop duplication lost, would need to recreate
            std::cerr << "Desktop duplication access lost" << std::endl;
            m_active = false;
        }
        return;
    }

    // Get the desktop texture
    ID3D11Texture2D* desktopTexture = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
    desktopResource->Release();

    if (SUCCEEDED(hr)) {
        // Copy to staging texture
        m_context->CopyResource(m_stagingTexture, desktopTexture);
        desktopTexture->Release();

        // Map staging texture to read pixels
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Convert BGRA to RGBA, flipping vertically for OpenGL
            const uint8_t* src = (const uint8_t*)mapped.pData;
            uint8_t* dst = m_pixelBuffer.data();

            for (int y = 0; y < m_height; y++) {
                const uint8_t* srcRow = src + y * mapped.RowPitch;
                uint8_t* dstRow = dst + (m_height - 1 - y) * m_width * 4;
                for (int x = 0; x < m_width; x++) {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // R
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // B
                    dstRow[x * 4 + 3] = 255;                // A
                }
            }

            m_context->Unmap(m_stagingTexture, 0);

            m_texture.updateData(m_pixelBuffer.data(), m_width, m_height);
        }
    }

    m_duplication->ReleaseFrame();
}
