#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <mmeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

// A MIDI CC/Note event
struct MIDIEvent {
    int channel = 0;    // 0-15
    int type = 0;       // 0=CC, 1=NoteOn, 2=NoteOff
    int number = 0;     // CC number or note number
    int value = 0;      // 0-127
};

// A MIDI mapping: CC/note -> target
struct MIDIMapping {
    int channel = -1;   // -1 = any channel
    int type = 0;       // 0=CC, 1=Note
    int number = 0;     // CC or note number

    // Target
    enum class Target {
        LayerOpacity, LayerVisible, LayerPosX, LayerPosY, LayerScale, LayerRotation,
        SceneRecall, BPMSet, BPMTap
    };
    Target target = Target::LayerOpacity;
    int layerIndex = 0; // for layer targets
    int sceneIndex = 0; // for scene recall
};

class MIDIManager {
public:
    ~MIDIManager();

    // Device management
    std::vector<std::string> listDevices();
    bool openDevice(int index);
    void closeDevice();
    bool isOpen() const { return m_open; }
    int deviceIndex() const { return m_deviceIdx; }

    // Poll events (call from main thread)
    std::vector<MIDIEvent> pollEvents();

    // MIDI Learn mode
    void startLearn() { m_learning = true; m_lastLearnEvent = {}; }
    void stopLearn() { m_learning = false; }
    bool isLearning() const { return m_learning; }
    MIDIEvent lastLearnEvent() {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        return m_lastLearnEvent;
    }

    // Mappings
    void addMapping(const MIDIMapping& mapping);
    void removeMapping(int index);
    std::vector<MIDIMapping>& mappings() { return m_mappings; }
    const std::vector<MIDIMapping>& mappings() const { return m_mappings; }

    // Process mappings against events, returns actions to apply
    struct MappingAction {
        MIDIMapping::Target target;
        int layerIndex;
        int sceneIndex;
        float value; // 0-1 normalized
    };
    std::vector<MappingAction> processEvents(const std::vector<MIDIEvent>& events);

private:
#ifdef _WIN32
    HMIDIIN m_midiIn = nullptr;
    static void CALLBACK midiCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance,
                                       DWORD_PTR dwParam1, DWORD_PTR dwParam2);
#endif
    bool m_open = false;
    int m_deviceIdx = -1;

    std::mutex m_eventMutex;
    std::vector<MIDIEvent> m_pendingEvents;

    bool m_learning = false;
    MIDIEvent m_lastLearnEvent;

    std::vector<MIDIMapping> m_mappings;
};
