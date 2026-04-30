#ifdef HAS_WHEP
#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#include "sources/WHEPSource.h"
#include <iostream>
#include <fstream>
#include <mach/mach.h>
#include <objc/runtime.h>

extern void whepLog(const std::string& msg);

// Resident memory in MB — used to verify WKWebView teardown actually frees
// across reconnects. Logged at startWebView/stopWebView so a grep of
// whep_debug.log for "RSS:" shows whether the process is leaking.
static size_t getProcessRSS_MB() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size / (1024 * 1024);
    }
    return 0;
}

// WHEP player HTML — replicates etherea dashboard's WebRTC flow exactly:
// 1. Fetch ICE servers from etherea
// 2. Create PeerConnection with data channel (for Scope parameters)
// 3. Send offer to etherea's /api/scope/webrtc/offer proxy
// 4. Receive video, render to canvas, extract pixels
static NSString* whepPlayerHTML(const std::string& whepUrl, const std::string& iceServersJson) {
    return [NSString stringWithFormat:@R"HTML(
<!DOCTYPE html>
<html><body>
<canvas id="c" style="display:none"></canvas>
<video id="v" autoplay playsinline muted style="display:none"></video>
<script>
const whepUrl = '%@';
const fallbackIceServers = %@;
const S = (msg) => window.webkit.messageHandlers.status.postMessage(msg);

async function connect() {
    // Step 1: Fetch ICE servers from etherea (same as ScopeModule.getIceServers)
    let iceServers = fallbackIceServers;
    try {
        const iceResp = await fetch('http://localhost:7860/api/scope/webrtc/ice-servers');
        if (iceResp.ok) {
            const d = await iceResp.json();
            if (d.ice_servers && d.ice_servers.length > 0) iceServers = d.ice_servers;
            S('got-ice-servers: ' + iceServers.length);
        }
    } catch(e) { S('ice-fallback'); }

    // Step 2: Create PeerConnection (same config as etherea ScopeModule)
    const pc = new RTCPeerConnection({
        iceServers,
        iceCandidatePoolSize: 10,
        bundlePolicy: 'max-bundle',
        rtcpMuxPolicy: 'require'
    });

    // No data channel — we're a passive viewer only

    // Step 4: Add video transceiver (recvonly — we only receive)
    pc.addTransceiver('video', { direction: 'recvonly' });

    // Step 5: Handle incoming video track
    pc.ontrack = (e) => {
        S('got-track: ' + e.track.kind);
        if (e.track.kind === 'video') {
            document.getElementById('v').srcObject = e.streams[0];
            scheduleNextFrame();
        }
    };

    pc.onconnectionstatechange = () => {
        S('pc:' + pc.connectionState);
        if (pc.connectionState === 'failed') setTimeout(connect, 3000);
    };
    pc.oniceconnectionstatechange = () => S('ice:' + pc.iceConnectionState);

    // Step 6: Create offer.
    // Match etherea dashboard's ScopeModule.preferH264Codec: reorder the
    // m=video payload types so H264 comes first. Scope's LTX2 pipeline
    // emits H264 — without this, Safari's default codec preference may
    // negotiate VP8/VP9 in the answer, the track event still fires, but
    // no decodable frames flow.
    const offer = await pc.createOffer();
    let sdp = offer.sdp;
    const lines = sdp.split('\r\n');
    const out = [];
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        if (line.startsWith('m=video')) {
            const h264 = [];
            for (let j = i + 1; j < lines.length; j++) {
                if (lines[j].startsWith('m=')) break;
                const m = lines[j].match(/^a=rtpmap:(\d+) H264\//i);
                if (m) h264.push(m[1]);
            }
            if (h264.length) {
                const parts = line.split(' ');
                const others = parts.slice(3).filter(pt => !h264.includes(pt));
                out.push(parts.slice(0, 3).concat(h264, others).join(' '));
                S('codec-prefer-h264: ' + h264.join(','));
                continue;
            }
        }
        out.push(line);
    }
    sdp = out.join('\r\n');
    await pc.setLocalDescription({ type: 'offer', sdp });

    // Step 7: Wait for ICE gathering (5s timeout, same as etherea)
    await new Promise(r => {
        if (pc.iceGatheringState === 'complete') return r();
        pc.onicegatheringstatechange = () => {
            if (pc.iceGatheringState === 'complete') r();
        };
        setTimeout(r, 5000);
    });
    S('ice-gathered');

    // Step 8: Prefer local mediamtx WHEP when Etherea is relaying Scope video.
    const localUrl = 'http://localhost:8889/easel_scope/whep';
    S('trying-local: ' + localUrl);
    let answer = null;

    // Retry a few times (relay might take a moment to start)
    for (let attempt = 0; attempt < 5 && !answer; attempt++) {
        try {
            const resp = await fetch(localUrl, {
                method: 'POST',
                headers: { 'Content-Type': 'application/sdp' },
                body: pc.localDescription.sdp
            });
            if (resp.ok) {
                answer = await resp.text();
                S('local-whep-ok: ' + answer.length + ' bytes');
            } else {
                S('local-retry ' + (attempt+1) + ': ' + resp.status);
                await new Promise(r => setTimeout(r, 2000));
            }
        } catch(e) {
            S('local-retry ' + (attempt+1) + ': ' + e.message);
            await new Promise(r => setTimeout(r, 2000));
        }
    }

    // Fallback: connect directly to the WHEP URL provided by the source.
    // Cue-published Scope outputs use this path without requiring Etherea.
    if (!answer && whepUrl) {
        S('trying-direct: ' + whepUrl);
        try {
            const resp = await fetch(whepUrl, {
                method: 'POST',
                headers: { 'Content-Type': 'application/sdp' },
                body: pc.localDescription.sdp
            });
            if (resp.ok) {
                answer = await resp.text();
                S('direct-whep-ok: ' + answer.length + ' bytes');
            } else {
                S('direct-whep-failed: ' + resp.status);
            }
        } catch(e) {
            S('direct-whep-error: ' + e.message);
        }
    }

    // Final fallback: Etherea Scope WebRTC proxy.
    if (!answer) {
        S('fallback-etherea-scope');
        try {
            const resp = await fetch('http://localhost:7860/api/scope/webrtc/offer', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sdp: pc.localDescription.sdp, type: 'offer' })
            });
            if (resp.ok) {
                const data = await resp.json();
                if (data.sdp) { answer = data.sdp; S('scope-fallback-ok'); }
            }
        } catch(e) {}
    }

    if (!answer) {
        S('error: no answer from any endpoint');
        return;
    }

    // Step 9: Set remote description
    await pc.setRemoteDescription({ type: 'answer', sdp: answer });
    S('remote-description-set');
}

// Frame capture: draw video to canvas, extract raw RGBA, send to native.
// Uses requestVideoFrameCallback so we fire once per *new* video frame
// (~30Hz) instead of once per display refresh (60-120Hz on Pro Display).
// This ~halves JS allocation churn and stops capturing duplicate frames.
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d', { willReadFrequently: true });
const video = document.getElementById('v');
const captureIntervalMs = 125;
let lastCaptureMs = 0;

// Fast base64: prefers Uint8Array.toBase64() (Safari 18+) which avoids the
// huge intermediate `binary` string and skips the chunked apply(). Falls
// back to the chunked path on older WebKit. Output is byte-identical.
const hasFastB64 = typeof Uint8Array.prototype.toBase64 === 'function';
function bytesToBase64(bytes) {
    if (hasFastB64) {
        const view = bytes instanceof Uint8Array
            ? bytes
            : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        return view.toBase64();
    }
    let binary = '';
    const len = bytes.byteLength;
    for (let i = 0; i < len; i += 8192) {
        binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + 8192, len)));
    }
    return btoa(binary);
}

function captureFrame() {
    const now = performance.now();
    if (now - lastCaptureMs < captureIntervalMs) {
        scheduleNextFrame();
        return;
    }
    lastCaptureMs = now;

    if (video.videoWidth > 0 && video.videoHeight > 0) {
        if (canvas.width !== video.videoWidth || canvas.height !== video.videoHeight) {
            canvas.width = video.videoWidth;
            canvas.height = video.videoHeight;
            S('video-size: ' + canvas.width + 'x' + canvas.height +
              (hasFastB64 ? ' (fast-b64)' : ' (slow-b64)'));
        }
        ctx.drawImage(video, 0, 0);
        const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
        window.webkit.messageHandlers.frame.postMessage(
            canvas.width + ',' + canvas.height + ',' + bytesToBase64(imageData.data)
        );
    }
    scheduleNextFrame();
}

function scheduleNextFrame() {
    if (video.requestVideoFrameCallback) {
        video.requestVideoFrameCallback(captureFrame);
    } else {
        requestAnimationFrame(captureFrame);
    }
}

window.onerror = (msg, src, line) => S('js-error: ' + msg + ' line:' + line);
connect().catch(e => S('error: ' + e.message));
</script>
</body></html>
)HTML",
    [NSString stringWithUTF8String:whepUrl.c_str()],
    [NSString stringWithUTF8String:iceServersJson.c_str()]
    ];
}

// Message handler for receiving frames from WKWebView
@interface WHEPFrameHandler : NSObject <WKScriptMessageHandler>
@property (nonatomic, assign) WHEPSource* source;
@end

@implementation WHEPFrameHandler
- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)message {
    @autoreleasepool {
    if (!_source) return; // guard against stale handler after disconnect
    if ([message.name isEqualToString:@"frame"]) {
        NSString* payload = message.body;
        // Format: "width,height,base64rgbadata"
        NSArray* parts = [payload componentsSeparatedByString:@","];
        if (parts.count >= 3) {
            int w = [parts[0] intValue];
            int h = [parts[1] intValue];
            NSString* b64 = parts[2];
            NSData* data = [[NSData alloc] initWithBase64EncodedString:b64 options:0];
            if (data && w > 0 && h > 0) {
                _source->onWebViewFrame(w, h, (const uint8_t*)data.bytes, (int)data.length);
            }
            [data release];
        }
    } else if ([message.name isEqualToString:@"status"]) {
        NSString* status = message.body;
        _source->onWebViewStatus([status UTF8String]);
    }
    }
}
@end

// C++ implementation
void WHEPSource::startWebView(const std::string& whepUrl, const std::string& iceServersJson) {

    std::string url = whepUrl;
    std::string iceJson = iceServersJson;
    dispatch_async(dispatch_get_main_queue(), ^{
        // Clean up any existing WebView first (inline, already on main thread)
        if (m_webView) {
            WKWebView* oldWv = (WKWebView*)m_webView;
            [oldWv stopLoading];
            [oldWv.configuration.userContentController removeAllScriptMessageHandlers];
            [oldWv removeFromSuperview];
            [oldWv release];
            m_webView = nullptr;
        }
        if (m_webViewHandler) {
            WHEPFrameHandler* oldHandler = (WHEPFrameHandler*)m_webViewHandler;
            oldHandler.source = nullptr;
            [oldHandler release];
            m_webViewHandler = nullptr;
        }

        whepLog("[WHEP-WebView] Starting WKWebView (RSS: " +
                std::to_string(getProcessRSS_MB()) + "MB) for: " + url);

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

        WHEPFrameHandler* handler = [[WHEPFrameHandler alloc] init];
        handler.source = this;
        [config.userContentController addScriptMessageHandler:handler name:@"frame"];
        [config.userContentController addScriptMessageHandler:handler name:@"status"];

        m_webViewHandler = (void*)handler;

        WKWebView* webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240) configuration:config];
        m_webView = (void*)webView;
        [config release];

        // Attach to main app window as a 1x1 subview behind everything
        NSWindow* mainWindow = [NSApp mainWindow];
        if (!mainWindow) mainWindow = [[NSApp windows] firstObject];
        if (mainWindow) {
            [webView setFrame:NSMakeRect(0, 0, 1, 1)];
            [mainWindow.contentView addSubview:webView positioned:NSWindowBelow relativeTo:nil];
        }

        NSString* html = whepPlayerHTML(url, iceJson);

        // Use localhost as baseURL so fetch to localhost:7860 works without CORS
        [webView loadHTMLString:html baseURL:[NSURL URLWithString:@"http://localhost:7860"]];

        whepLog("[WHEP-WebView] WKWebView created and loading (RSS: " +
                std::to_string(getProcessRSS_MB()) + "MB)");
    });
}

void WHEPSource::stopWebView() {
    // Always async — avoids deadlocks. The cleanup block nulls the source pointer
    // first to prevent any callbacks from firing on the old object.
    if (m_webViewHandler) {
        WHEPFrameHandler* handler = (WHEPFrameHandler*)m_webViewHandler;
        handler.source = nullptr; // immediate, thread-safe (prevents stale access)
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (m_webView) {
            WKWebView* wv = (WKWebView*)m_webView;
            [wv stopLoading];
            [wv.configuration.userContentController removeAllScriptMessageHandlers];
            [wv removeFromSuperview];
            [wv release];
            m_webView = nullptr;
        }
        if (m_webViewHandler) {
            WHEPFrameHandler* handler = (WHEPFrameHandler*)m_webViewHandler;
            [handler release];
            m_webViewHandler = nullptr;
        }
        whepLog("[WHEP-WebView] stopWebView complete (RSS: " +
                std::to_string(getProcessRSS_MB()) + "MB)");
    });
}

void WHEPSource::onWebViewFrame(int w, int h, const uint8_t* rgba, int dataLen) {
    if (dataLen != w * h * 4) return;
    bool firstFrame = m_width <= 0 || m_height <= 0;

    int writeIdx = (m_writeIndex.load() + 1) % 3;
    auto& buf = m_buffers[writeIdx];
    buf.data.assign(rgba, rgba + dataLen);
    buf.width = w;
    buf.height = h;
    buf.ready.store(true);
    m_writeIndex.store(writeIdx);

    m_width = w;
    m_height = h;
    m_connected.store(true);
    if (firstFrame) {
        whepLog("[WHEP-WebView] first-frame: " + std::to_string(w) + "x" + std::to_string(h));
    }
}

void WHEPSource::onWebViewStatus(const std::string& status) {
    whepLog("[WHEP-WebView] Status: " + status);
    m_statusText = status;
    if (status.find("pc:connected") != std::string::npos || status.find("got-track") != std::string::npos) {
        m_connected.store(true);
    } else if (status.find("pc:failed") != std::string::npos || status.find("error") != std::string::npos) {
        m_failed.store(true);
    }
}

#endif // HAS_WHEP
