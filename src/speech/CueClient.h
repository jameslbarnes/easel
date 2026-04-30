#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class CueClient {
public:
    struct SourceOutput {
        std::string id;
        std::string kind;
        std::string label;
        std::string provider;
        std::string transport;
        std::string url;
    };

    ~CueClient();

    bool connect(const std::string& baseUrl = "http://localhost:8792",
                 const std::string& sessionId = "demo");
    void disconnect();

    bool isConnected() const { return m_connected.load(); }
    bool transcriptionConnected() const { return m_transcriptionConnected.load(); }
    bool isRunning() const { return m_running.load(); }

    void poll();
    void setTranscriptionEnabled(bool enabled);
    bool transcriptionEnabled() const { return m_transcriptionEnabled.load(); }
    void feedAudioSamples(const float* mono, int count, int sampleRate);

    void setTranscriptCallback(std::function<void(const std::string&, bool)> cb) {
        m_transcriptCb = std::move(cb);
    }

    std::string sessionId() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_sessionId; }
    std::string fullTranscript() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_fullTranscript; }
    std::string latestWords() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_latestWords; }
    std::string prompt() const { std::lock_guard<std::mutex> lk(m_dataMutex); return m_prompt; }
    bool promptReset() const { return m_promptReset.load(); }
    std::string latestVisionDescription() const {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        return m_latestVisionDescription;
    }
    std::unordered_map<std::string, std::string> metadata() const {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        return m_metadata;
    }
    std::vector<SourceOutput> sources() const {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        std::vector<SourceOutput> out;
        out.reserve(m_sources.size());
        for (const auto& kv : m_sources) out.push_back(kv.second);
        return out;
    }

private:
    struct TranscriptEvent {
        std::string text;
        bool isFinal = true;
    };

    std::function<void(const std::string&, bool)> m_transcriptCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_transcriptionEnabled{false};
    std::atomic<bool> m_transcriptionRunning{false};
    std::atomic<bool> m_transcriptionConnected{false};
    std::atomic<bool> m_promptReset{false};
    std::thread m_thread;
    std::thread m_transcriptionThread;

    std::string m_baseUrl;
    std::string m_sessionId = "demo";
    std::string m_host = "localhost";
    int m_port = 8792;

    mutable std::mutex m_dataMutex;
    std::string m_fullTranscript;
    std::string m_latestWords;
    std::string m_prompt;
    std::string m_latestVisionDescription;
    std::unordered_map<std::string, std::string> m_metadata;
    std::unordered_map<std::string, SourceOutput> m_sources;

    std::mutex m_eventMutex;
    std::vector<TranscriptEvent> m_pendingTranscriptEvents;
    std::mutex m_audioMutex;
    std::condition_variable m_audioCv;
    std::vector<std::string> m_pendingAudioChunks;

    void parseUrl();
    void eventLoop();
    void transcriptionLoop();
    void handleMessage(const std::string& payload);
};
