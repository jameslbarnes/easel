#ifdef HAS_WEBVIEW2
#include "speech/SpeechBridge.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

// Embedded HTML page that runs Web Speech API
static const wchar_t* kSpeechHTML = LR"HTML(
<!DOCTYPE html><html><body>
<script>
let recognition = null;
function start() {
    if (recognition) recognition.abort();
    recognition = new webkitSpeechRecognition();
    recognition.continuous = true;
    recognition.interimResults = true;
    recognition.lang = 'en-US';
    recognition.onresult = function(e) {
        let interim = '', final = '';
        for (let i = e.resultIndex; i < e.results.length; i++) {
            let t = e.results[i][0].transcript;
            if (e.results[i].isFinal) {
                final += t;
            } else {
                interim += t;
            }
        }
        if (final) {
            chrome.webview.postMessage(JSON.stringify({type:'transcript', text:final, isFinal:true}));
        } else if (interim) {
            chrome.webview.postMessage(JSON.stringify({type:'transcript', text:interim, isFinal:false}));
        }
    };
    recognition.onerror = function(e) {
        chrome.webview.postMessage(JSON.stringify({type:'error', error:e.error}));
    };
    recognition.onend = function() {
        chrome.webview.postMessage(JSON.stringify({type:'ended'}));
    };
    recognition.start();
}
function stop() {
    if (recognition) { recognition.stop(); recognition = null; }
}
window.chrome.webview.addEventListener('message', function(e) {
    try {
        let msg = JSON.parse(e.data);
        if (msg.cmd === 'start') start();
        else if (msg.cmd === 'stop') stop();
    } catch(ex) {}
});
</script>
</body></html>
)HTML";

void SpeechBridge::init(HWND parentHwnd) {
    if (m_state != State::Uninitialized) return;
    m_state = State::Creating;

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, parentHwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    std::cerr << "SpeechBridge: failed to create WebView2 environment" << std::endl;
                    m_state = State::Error;
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(parentHwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                std::cerr << "SpeechBridge: failed to create WebView2 controller" << std::endl;
                                m_state = State::Error;
                                return S_OK;
                            }

                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webview);

                            // Hide the WebView (0x0 bounds)
                            RECT bounds = {0, 0, 0, 0};
                            m_controller->put_Bounds(bounds);
                            m_controller->put_IsVisible(FALSE);

                            // Auto-grant microphone permission
                            wil::com_ptr<ICoreWebView2_4> wv4;
                            m_webview->QueryInterface(IID_PPV_ARGS(&wv4));
                            if (wv4) {
                                wv4->add_PermissionRequested(
                                    Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                        [](ICoreWebView2* sender, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                            COREWEBVIEW2_PERMISSION_KIND kind;
                                            args->get_PermissionKind(&kind);
                                            if (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) {
                                                args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                            }
                                            return S_OK;
                                        }).Get(), nullptr);
                            }

                            // Handle messages from the page
                            m_webview->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string msgRaw;
                                        args->TryGetWebMessageAsString(&msgRaw);
                                        if (msgRaw) {
                                            onMessageReceived(msgRaw.get());
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // Navigate to our embedded HTML
                            m_webview->NavigateToString(kSpeechHTML);
                            m_state = State::Ready;
                            std::cout << "SpeechBridge: WebView2 ready" << std::endl;

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void SpeechBridge::shutdown() {
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webview = nullptr;
    m_state = State::Uninitialized;
    m_listening = false;
}

void SpeechBridge::startListening() {
    if (m_state != State::Ready || !m_webview) return;
    m_webview->PostWebMessageAsString(L"{\"cmd\":\"start\"}");
    m_listening = true;
    m_finalText.clear();
}

void SpeechBridge::stopListening() {
    if (!m_webview) return;
    m_webview->PostWebMessageAsString(L"{\"cmd\":\"stop\"}");
    m_listening = false;
}

void SpeechBridge::onMessageReceived(const std::wstring& message) {
    // Convert wstring to string (ASCII-safe for our JSON)
    std::string msg(message.begin(), message.end());

    try {
        json j = json::parse(msg);
        std::string type = j.value("type", "");

        if (type == "transcript") {
            std::string text = j.value("text", "");
            bool isFinal = j.value("isFinal", false);

            // Uppercase the text for shader encoding
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return (char)toupper(c); });

            if (isFinal) {
                m_finalText += text;
                m_currentText = m_finalText;
            } else {
                m_currentText = m_finalText + text;
            }

            if (m_callback) {
                m_callback(m_currentText, isFinal);
            }
        } else if (type == "ended") {
            // Recognition ended (e.g. timeout) — restart if still supposed to be listening
            if (m_listening && m_webview) {
                m_webview->PostWebMessageAsString(L"{\"cmd\":\"start\"}");
            }
        } else if (type == "error") {
            std::cerr << "SpeechBridge: recognition error: " << j.value("error", "unknown") << std::endl;
        }
    } catch (const json::exception& e) {
        std::cerr << "SpeechBridge: JSON parse error: " << e.what() << std::endl;
    }
}

#endif // HAS_WEBVIEW2
