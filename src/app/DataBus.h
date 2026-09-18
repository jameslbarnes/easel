#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

// Simple named-value store for routing data to shader parameters.
// Writers (EthereaClient, MIDI, OSC, etc.) push values by key.
// Consumers (shader text params) bind to a key and receive updates.
class DataBus {
public:
    // Hard cap on distinct keys. A 24/7 install can otherwise leak a key
    // forever per unique inbound OSC address (Application routes raw OSC
    // addresses through set()). Once full, only existing keys update.
    static constexpr size_t kMaxKeys = 4096;

    // --- Writers ---
    void set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_values.size() >= kMaxKeys && m_values.find(key) == m_values.end())
            return;
        m_values[key] = value;
    }

    // Append `text` to the string at `key` (with separator), bounded to the
    // last `maxChars` characters. Live transcript accumulators (etherea/cue)
    // would otherwise grow without limit while the mic runs — the leak that
    // sinks a permanent installation. Keeps the recent tail (what shaders
    // showing "recent words" actually want). Locks once; does not call
    // set()/get() (would deadlock the non-recursive mutex).
    void appendCapped(const std::string& key, const std::string& text,
                      const char* sep = " ", size_t maxChars = 16000) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string& v = m_values[key];
        if (!v.empty() && sep && *sep) v += sep;
        v += text;
        if (v.size() > maxChars) v.erase(0, v.size() - maxChars);
    }

    std::string get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_values.find(key);
        return it != m_values.end() ? it->second : "";
    }

    // Numeric writes / reads — separate map so callers don't have to
    // round-trip floats through string parsing every frame. Used by
    // VisionSource (pose/face/hand normalized landmarks) and any other
    // numeric live-input route (MIDI CC, OSC).
    void setNum(const std::string& key, float value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_numbers.size() >= kMaxKeys && m_numbers.find(key) == m_numbers.end())
            return;
        m_numbers[key] = value;
    }

    float getNum(const std::string& key, float defaultValue = 0.0f) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_numbers.find(key);
        return it != m_numbers.end() ? it->second : defaultValue;
    }

    bool hasNum(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_numbers.find(key) != m_numbers.end();
    }

    // --- Bindings (layerId:paramName -> data key) ---
    void bind(uint32_t layerId, const std::string& param, const std::string& dataKey) {
        std::string k = std::to_string(layerId) + ":" + param;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (dataKey.empty())
            m_bindings.erase(k);
        else
            m_bindings[k] = dataKey;
    }

    std::string binding(uint32_t layerId, const std::string& param) const {
        std::string k = std::to_string(layerId) + ":" + param;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_bindings.find(k);
        return it != m_bindings.end() ? it->second : "";
    }

    const std::unordered_map<std::string, std::string>& bindings() const { return m_bindings; }

    // --- Available data keys (for UI dropdown) ---
    struct DataKey {
        std::string key;   // e.g. "etherea.transcript"
        std::string label; // e.g. "Transcript"
    };

    static std::vector<DataKey> availableKeys() {
        return {
            {"",                     "Manual"},
            {"etherea.transcript",   "Transcript"},
            {"etherea.latest",       "Latest Words"},
            {"etherea.recent",       "Recent Words (FIFO)"},
            {"etherea.words",        "New Words"},
            {"etherea.caption",      "Caption (utterance)"},
            {"etherea.speaker",      "Speaker"},
            {"etherea.hint.0",       "Hint 1"},
            {"etherea.hint.1",       "Hint 2"},
            {"etherea.hint.2",       "Hint 3"},
            {"etherea.prompt",       "Prompt"},
            {"cue.latest",           "Cue: Latest"},
            {"cue.recent",           "Cue: Recent (FIFO)"},
            {"cue.words",            "Cue: New Words"},
            {"cue.transcript",       "Cue: Transcript"},
            {"cue.speaker",          "Cue: Speaker"},
            {"cue.caption",          "Cue: Caption (utterance)"},
            {"cue.prompt",           "Cue: Prompt"},
            {"cue.coach.headline",   "Cue Coach: Headline"},
            {"cue.coach.quote",      "Cue Coach: Quote"},
            {"cue.coach.feedback",   "Cue Coach: Feedback"},
            {"cue.coach.alternative","Cue Coach: Alternative"},
        };
    }

    // Numeric data-key catalogue (for binding UI dropdowns where the
    // target shader parameter is a float). Vision keys map 1:1 to the
    // landmarks pushed by VisionSource (Apple Vision pose/face/hands).
    static std::vector<DataKey> availableNumericKeys() {
        return {
            {"",                          "Manual"},
            {"vision.pose.confidence",    "Pose Confidence"},
            {"vision.pose.head.x",        "Head X"},
            {"vision.pose.head.y",        "Head Y"},
            {"vision.face.detected",      "Face Detected"},
            {"vision.face.smile",         "Smile"},
            {"vision.hand.count",         "Hand Count"},
            {"vision.hand.left.x",        "Left Hand X"},
            {"vision.hand.left.y",        "Left Hand Y"},
            {"vision.hand.right.x",       "Right Hand X"},
            {"vision.hand.right.y",       "Right Hand Y"},
            {"vision.hand.pinch",         "Pinch"},
        };
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_values;
    std::unordered_map<std::string, std::string> m_bindings;
    std::unordered_map<std::string, float>       m_numbers;
};
