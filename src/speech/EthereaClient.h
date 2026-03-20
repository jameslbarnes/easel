#pragma once
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

struct EthereaSession {
    std::string id;
    float idleSeconds = 0;
    int transcriptLength = 0;
    bool isPaused = false;
};

// Unified Etherea client: WebSocket for real-time transcript, SSE for hints/state.
class EthereaClient {
public:
    ~EthereaClient();

    // Connect both channels to Etherea server
    bool connect(const std::string& baseUrl = "http://localhost:7860",
                 const std::string& sessionId = "");
    void disconnect();

    bool isConnected() const { return m_wsConnected.load() || m_sseConnected.load(); }
    bool isRunning() const { return m_running.load(); }
    bool wsConnected() const { return m_wsConnected.load(); }
    bool sseConnected() const { return m_sseConnected.load(); }

    // Called each frame on main thread — dispatches queued updates
    void poll();

    // --- Transcript callbacks ---
    // onTranscript(text, isFinal) — fired for every interim/final Deepgram segment
    void setTranscriptCallback(std::function<void(const std::string&, bool)> cb) {
        m_transcriptCb = std::move(cb);
    }

    // --- Data accessors (thread-safe) ---
    std::string fullTranscript() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_fullTranscript; }
    std::string latestWords() const    { std::lock_guard<std::mutex> lk(m_dataMutex); return m_latestWords; }
    std::vector<std::string> hints() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_hints; }
    std::string prompt() const         { std::lock_guard<std::mutex> lk(m_dataMutex); return m_prompt; }
    bool promptResetCache() const      { return m_resetCache.load(); }

    // Fetch available sessions (blocking HTTP — call sparingly)
    static std::vector<EthereaSession> fetchSessions(const std::string& baseUrl = "http://localhost:7860");

private:
    std::function<void(const std::string&, bool)> m_transcriptCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_wsConnected{false};
    std::atomic<bool> m_sseConnected{false};

    std::thread m_wsThread;
    std::thread m_sseThread;

    std::string m_baseUrl;
    std::string m_sessionId;
    std::string m_host;
    int m_port = 7860;

    // Shared data (written by threads, read by main thread)
    mutable std::mutex m_dataMutex;
    std::string m_fullTranscript;
    std::string m_latestWords;
    std::vector<std::string> m_hints;
    std::string m_prompt;
    std::atomic<bool> m_resetCache{false};

    // Pending transcript segments for main-thread dispatch
    struct TranscriptEvent {
        std::string text;
        bool isFinal;
    };
    std::mutex m_eventMutex;
    std::vector<TranscriptEvent> m_pendingEvents;

    void parseUrl();
    void wsLoop();   // WebSocket thread
    void sseLoop();  // SSE thread
};
