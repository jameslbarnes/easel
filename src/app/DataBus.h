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
    // --- Writers ---
    void set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_values[key] = value;
    }

    std::string get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_values.find(key);
        return it != m_values.end() ? it->second : "";
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
            {"etherea.hint.0",       "Hint 1"},
            {"etherea.hint.1",       "Hint 2"},
            {"etherea.hint.2",       "Hint 3"},
            {"etherea.prompt",       "Prompt"},
            {"cue.transcript",       "Cue Transcript"},
            {"cue.latest",           "Cue Latest Words"},
            {"cue.prompt",           "Cue Prompt"},
            {"cue.prompt.reset",     "Cue Prompt Reset"},
            {"cue.vision.description", "Cue Vision Description"},
            {"cue.session",          "Cue Session"},
        };
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_values;
    std::unordered_map<std::string, std::string> m_bindings;
};
