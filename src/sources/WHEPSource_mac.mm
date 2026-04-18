#ifdef HAS_WHEP
#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#include "sources/WHEPSource.h"
#include <iostream>
#include <fstream>
#include <objc/runtime.h>

extern void whepLog(const std::string& msg);

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
            requestAnimationFrame(captureFrame);
        }
    };

    pc.onconnectionstatechange = () => {
        S('pc:' + pc.connectionState);
        if (pc.connectionState === 'failed') setTimeout(connect, 3000);
    };
    pc.oniceconnectionstatechange = () => S('ice:' + pc.iceConnectionState);

    // Step 6: Create offer
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);

    // Step 7: Wait for ICE gathering (5s timeout, same as etherea)
    await new Promise(r => {
        if (pc.iceGatheringState === 'complete') return r();
        pc.onicegatheringstatechange = () => {
            if (pc.iceGatheringState === 'complete') r();
        };
        setTimeout(r, 5000);
    });
    S('ice-gathered');

    // Step 8: Connect to local mediamtx WHEP
    // Etherea's browser relays the received Scope video to local mediamtx via WHIP
    // so we read it locally — no second remote WebRTC session, zero jitter
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

    // Fallback: Scope WebRTC (creates a second session — causes jitter)
    if (!answer) {
        S('fallback-scope');
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

// Frame capture: draw video to canvas, extract raw RGBA, send to native
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d', { willReadFrequently: true });
const video = document.getElementById('v');

function captureFrame() {
    if (video.videoWidth > 0 && video.videoHeight > 0) {
        if (canvas.width !== video.videoWidth || canvas.height !== video.videoHeight) {
            canvas.width = video.videoWidth;
            canvas.height = video.videoHeight;
            S('video-size: ' + canvas.width + 'x' + canvas.height);
        }
        ctx.drawImage(video, 0, 0);
        const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const bytes = imageData.data;
        let binary = '';
        const len = bytes.byteLength;
        for (let i = 0; i < len; i += 8192) {
            binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + 8192, len)));
        }
        window.webkit.messageHandlers.frame.postMessage(
            canvas.width + ',' + canvas.height + ',' + btoa(binary)
        );
    }
    requestAnimationFrame(captureFrame);
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
        }
    } else if ([message.name isEqualToString:@"status"]) {
        NSString* status = message.body;
        _source->onWebViewStatus([status UTF8String]);
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
            WKWebView* oldWv = (__bridge_transfer WKWebView*)m_webView;
            [oldWv stopLoading];
            [oldWv.configuration.userContentController removeAllScriptMessageHandlers];
            [oldWv removeFromSuperview];
            m_webView = nullptr;
        }
        if (m_webViewHandler) {
            WHEPFrameHandler* oldHandler = (__bridge_transfer WHEPFrameHandler*)m_webViewHandler;
            oldHandler.source = nullptr;
            oldHandler = nil;
            m_webViewHandler = nullptr;
        }

        whepLog("[WHEP-WebView] Starting WKWebView for: " + url);

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

        WHEPFrameHandler* handler = [[WHEPFrameHandler alloc] init];
        handler.source = this;
        [config.userContentController addScriptMessageHandler:handler name:@"frame"];
        [config.userContentController addScriptMessageHandler:handler name:@"status"];

        m_webViewHandler = (__bridge_retained void*)handler;

        WKWebView* webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240) configuration:config];
        m_webView = (__bridge_retained void*)webView;

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

        whepLog("[WHEP-WebView] WKWebView created and loading");
    });
}

void WHEPSource::stopWebView() {
    // Always async — avoids deadlocks. The cleanup block nulls the source pointer
    // first to prevent any callbacks from firing on the old object.
    if (m_webViewHandler) {
        WHEPFrameHandler* handler = (__bridge WHEPFrameHandler*)m_webViewHandler;
        handler.source = nullptr; // immediate, thread-safe (prevents stale access)
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (m_webView) {
            WKWebView* wv = (__bridge_transfer WKWebView*)m_webView;
            [wv stopLoading];
            [wv.configuration.userContentController removeAllScriptMessageHandlers];
            [wv removeFromSuperview];
            m_webView = nullptr;
        }
        if (m_webViewHandler) {
            WHEPFrameHandler* handler = (__bridge_transfer WHEPFrameHandler*)m_webViewHandler;
            handler = nil;
            m_webViewHandler = nullptr;
        }
    });
}

void WHEPSource::onWebViewFrame(int w, int h, const uint8_t* rgba, int dataLen) {
    if (dataLen != w * h * 4) return;

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
