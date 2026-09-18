#pragma once
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include "speech/CaptionSegment.h"

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
    // Consecutive failed connect attempts (reset to 0 on a successful
    // connect) — lets the UI tell "still dialing in" apart from "genuinely
    // can't reach it, retrying with backoff" instead of showing a perpetual,
    // seemingly-frozen "Connecting…" the whole time Etherea's backend is down.
    int wsFailedAttempts() const { return m_wsFailedAttempts.load(); }

    // Called each frame on main thread — dispatches queued updates
    void poll();

    // --- Transcript callbacks ---
    // onTranscript(segment) — fired for every interim/final Deepgram segment.
    // Etherea's raw WS "transcript" frame is snake_case (is_final, speech_final,
    // utterance_id, revision, words[], full) and is parsed into CaptionSegment;
    // legacy {type,text,is_final} frames still work (utteranceId empty).
    void setTranscriptCallback(std::function<void(const CaptionSegment&)> cb) {
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
    std::function<void(const CaptionSegment&)> m_transcriptCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_wsConnected{false};
    std::atomic<bool> m_sseConnected{false};
    std::atomic<int> m_wsFailedAttempts{0};

    std::thread m_wsThread;
    std::thread m_sseThread;
    // Live socket handles — stored so disconnect() can closesocket() them
    // immediately, which unblocks any select()/recv() in progress and lets
    // the threads exit in <50ms instead of waiting for a timeout to fire.
    std::atomic<uintptr_t> m_wsSock{(uintptr_t)~0};  // INVALID_SOCKET
    std::atomic<uintptr_t> m_sseSock{(uintptr_t)~0};

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
    using TranscriptEvent = CaptionSegment;
    std::mutex m_eventMutex;
    std::vector<TranscriptEvent> m_pendingEvents;

    void parseUrl();
    void wsLoop();   // WebSocket thread
    void sseLoop();  // SSE thread
};
