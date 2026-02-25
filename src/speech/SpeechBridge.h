#pragma once
#ifdef HAS_WEBVIEW2

#include <string>
#include <functional>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

class SpeechBridge {
public:
    enum class State { Uninitialized, Creating, Ready, Error };

    void init(HWND parentHwnd);
    void shutdown();

    void startListening();
    void stopListening();
    bool isListening() const { return m_listening; }

    const std::string& currentText() const { return m_currentText; }
    const std::string& finalText() const { return m_finalText; }

    State state() const { return m_state; }

    // Callback: (text, isFinal)
    void setCallback(std::function<void(const std::string&, bool)> cb) { m_callback = std::move(cb); }

private:
    State m_state = State::Uninitialized;
    bool m_listening = false;
    std::string m_currentText;
    std::string m_finalText;
    std::function<void(const std::string&, bool)> m_callback;

    wil::com_ptr<ICoreWebView2Controller> m_controller;
    wil::com_ptr<ICoreWebView2> m_webview;

    void onMessageReceived(const std::wstring& message);
};

#endif // HAS_WEBVIEW2
