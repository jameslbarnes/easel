#pragma once
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include "speech/CaptionSegment.h"

// Client for a Cue server (https://github.com/jameslbarnes/cue) running locally
// or on the LAN. Subscribes to /sessions/:id/events (a single WebSocket) and
// surfaces transcript segments, model-selected actions, and prompt updates to
// the main thread via setX callbacks + poll().
//
// Protocol reference (see external/cue/AGENTS.md):
//   ready, state.snapshot, observation, transcript, actions, action,
//   prompt, vision.description, signal, output.available, source.available,
//   output.status, caption.segment
//
// Caption contract (Phase B): the unified "transcript" event carries text,
// isFinal, speechFinal, live, speaker, utteranceId, revision, startMs/endMs,
// words[] and (after ingest) fullTranscript — all optional except text/isFinal,
// so legacy {type,text,isFinal} servers keep working. It is parsed into a
// CaptionSegment and dispatched via setTranscriptCallback. "caption.segment"
// (emitted by Cue's captionOutputProvider for kind:"text" outputs) carries the
// same fields; it is parsed into lastCaptionSegment() but deliberately NOT
// dispatched to the transcript callback — the transcript event already carries
// everything, and dispatching both would double-push every segment. Reserved
// for a future managed captions layer.
//
// CueClient is read-only: it does not push observations. Audio/frame ingest
// belongs in dedicated providers connected to /sessions/:id/transcription
// or /sessions/:id/vlm.

struct CueAction {
    std::string type;       // e.g. "coach.feedback", "video.update_prompt"
    std::string payload;    // raw JSON object string
};

class CueClient {
public:
    ~CueClient();

    // Connect to events websocket. baseUrl: http://host:port. sessionId is
    // any client-chosen string; the server creates the session on demand.
    bool connect(const std::string& baseUrl = "http://localhost:8791",
                 const std::string& sessionId = "easel");
    void disconnect();

    bool isConnected() const { return m_wsConnected.load(); }
    bool isRunning() const   { return m_running.load(); }

    // Drain queued events on the main thread; invokes registered callbacks.
    void poll();

    // ─── Callbacks (fired from poll(), main thread) ────────────────────
    void setTranscriptCallback(std::function<void(const CaptionSegment&)> cb) {
        m_transcriptCb = std::move(cb);
    }
    // Fired for every action the model selects (filter on action.type yourself).
    void setActionCallback(std::function<void(const CueAction&)> cb) {
        m_actionCb = std::move(cb);
    }
    // Fired when an action carries a "prompt" string in its payload (e.g. video.update_prompt).
    void setPromptCallback(std::function<void(const std::string& prompt, bool reset)> cb) {
        m_promptCb = std::move(cb);
    }

    // ─── Thread-safe accessors ─────────────────────────────────────────
    std::string fullTranscript() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_fullTranscript; }
    std::string latestWords() const    { std::lock_guard<std::mutex> lk(m_dataMutex); return m_latestWords; }
    std::string prompt() const         { std::lock_guard<std::mutex> lk(m_dataMutex); return m_prompt; }
    std::string lastActionType() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_lastActionType; }
    std::string lastActionPayload() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_lastActionPayload; }
    // Most recent "caption.segment" event (not dispatched — see header comment).
    CaptionSegment lastCaptionSegment() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_lastCaptionSegment; }

private:
    std::function<void(const CaptionSegment&)>                         m_transcriptCb;
    std::function<void(const CueAction&)>                              m_actionCb;
    std::function<void(const std::string&, bool)>                      m_promptCb;

    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_wsConnected{false};
    std::thread           m_wsThread;
    std::atomic<uintptr_t> m_wsSock{(uintptr_t)~0}; // INVALID_SOCKET sentinel

    std::string m_baseUrl;
    std::string m_sessionId;
    std::string m_host;
    int m_port = 8791;

    mutable std::mutex m_dataMutex;
    std::string m_fullTranscript;
    std::string m_latestWords;
    std::string m_prompt;
    std::string m_lastActionType;
    std::string m_lastActionPayload;
    CaptionSegment m_lastCaptionSegment;

    // Pending events for main-thread dispatch
    using PendingTranscript = CaptionSegment;
    struct PendingAction     { std::string type; std::string payload; };
    struct PendingPrompt     { std::string prompt; bool reset; };

    std::mutex m_eventMutex;
    std::vector<PendingTranscript> m_pendingTranscripts;
    std::vector<PendingAction>     m_pendingActions;
    std::vector<PendingPrompt>     m_pendingPrompts;

    void parseUrl();
    void wsLoop();
};
