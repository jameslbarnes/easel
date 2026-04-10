#ifdef __APPLE__
#include "sources/WindowCaptureSource_mac.h"

// Suppress deprecation warnings for CGWindowListCreateImage/CGWindowListCopyWindowInfo
// TODO: migrate to ScreenCaptureKit for window capture
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreFoundation/CoreFoundation.h>
#include <iostream>

WindowCaptureSource::~WindowCaptureSource() {
    stop();
}

std::vector<WindowInfo> WindowCaptureSource::enumerateWindows() {
    std::vector<WindowInfo> result;

    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!windowList) return result;

    CFIndex count = CFArrayGetCount(windowList);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef windowInfo = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);

        // Get window ID
        CGWindowID windowID = 0;
        CFNumberRef windowIDRef = (CFNumberRef)CFDictionaryGetValue(windowInfo, kCGWindowNumber);
        if (windowIDRef) CFNumberGetValue(windowIDRef, kCFNumberIntType, &windowID);

        // Get window name
        CFStringRef nameRef = (CFStringRef)CFDictionaryGetValue(windowInfo, kCGWindowName);
        std::string name;
        if (nameRef) {
            char buf[512];
            if (CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8)) {
                name = buf;
            }
        }

        // Get owner name as fallback
        if (name.empty()) {
            CFStringRef ownerRef = (CFStringRef)CFDictionaryGetValue(windowInfo, kCGWindowOwnerName);
            if (ownerRef) {
                char buf[512];
                if (CFStringGetCString(ownerRef, buf, sizeof(buf), kCFStringEncodingUTF8)) {
                    name = buf;
                }
            }
        }

        // Skip windows with no name
        if (name.empty()) continue;

        // Get bounds
        CFDictionaryRef boundsRef = (CFDictionaryRef)CFDictionaryGetValue(windowInfo, kCGWindowBounds);
        CGRect bounds = {};
        if (boundsRef) CGRectMakeWithDictionaryRepresentation(boundsRef, &bounds);

        // Skip tiny windows
        if (bounds.size.width < 50 || bounds.size.height < 50) continue;

        WindowInfo info;
        info.windowID = windowID;
        info.title = name;
        info.width = (int)bounds.size.width;
        info.height = (int)bounds.size.height;
        result.push_back(info);
    }

    CFRelease(windowList);
    return result;
}

bool WindowCaptureSource::start(uint32_t windowID) {
    m_windowID = windowID;

    // Get initial window bounds
    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionIncludingWindow, windowID
    );
    if (!windowList || CFArrayGetCount(windowList) == 0) {
        if (windowList) CFRelease(windowList);
        return false;
    }

    CFDictionaryRef windowInfo = (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, 0);
    CFDictionaryRef boundsRef = (CFDictionaryRef)CFDictionaryGetValue(windowInfo, kCGWindowBounds);
    CGRect bounds = {};
    if (boundsRef) CGRectMakeWithDictionaryRepresentation(boundsRef, &bounds);

    m_width = (int)bounds.size.width;
    m_height = (int)bounds.size.height;
    if (m_width <= 0 || m_height <= 0) {
        CFRelease(windowList);
        return false;
    }

    // Get title
    CFStringRef nameRef = (CFStringRef)CFDictionaryGetValue(windowInfo, kCGWindowName);
    if (nameRef) {
        char buf[512];
        if (CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            m_title = buf;
        }
    }

    CFRelease(windowList);

    m_pixelBuffer.resize(m_width * m_height * 4);
    m_active = true;

    std::cout << "[WindowCapture] Started capture of '" << m_title << "' " << m_width << "x" << m_height << std::endl;
    return true;
}

void WindowCaptureSource::stop() {
    m_active = false;
}

// CGWindowListCreateImage is marked unavailable in macOS 15 SDK but still works at runtime.
// Load it dynamically to bypass the compile-time check.
#include <dlfcn.h>
typedef CGImageRef (*CGWindowListCreateImageFunc)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
static CGWindowListCreateImageFunc getCGWindowListCreateImage() {
    static auto fn = (CGWindowListCreateImageFunc)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    return fn;
}

void WindowCaptureSource::update() {
    if (!m_active) return;

    auto createImage = getCGWindowListCreateImage();
    if (!createImage) return;

    CGImageRef image = createImage(
        CGRectNull,
        kCGWindowListOptionIncludingWindow,
        m_windowID,
        kCGWindowImageBoundsIgnoreFraming
    );
    if (!image) return;

    size_t w = CGImageGetWidth(image);
    size_t h = CGImageGetHeight(image);

    if ((int)w != m_width || (int)h != m_height) {
        m_width = (int)w;
        m_height = (int)h;
        m_pixelBuffer.resize(m_width * m_height * 4);
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        m_pixelBuffer.data(), w, h, 8, w * 4,
        colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
    CGContextRelease(ctx);
    CGColorSpaceRelease(colorSpace);
    CGImageRelease(image);

    if (!m_texture.id()) {
        m_texture.createEmpty(m_width, m_height, GL_RGBA8);
    }
    m_texture.updateData(m_pixelBuffer.data(), m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE);
}

#pragma clang diagnostic pop
#endif
