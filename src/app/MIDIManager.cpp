#include "app/MIDIManager.h"
#include <iostream>
#include <algorithm>

MIDIManager::~MIDIManager() {
    closeDevice();
}

std::vector<std::string> MIDIManager::listDevices() {
    std::vector<std::string> devices;
#ifdef _WIN32
    int count = midiInGetNumDevs();
    for (int i = 0; i < count; i++) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            devices.push_back(caps.szPname);
        }
    }
#endif
    return devices;
}

bool MIDIManager::openDevice(int index) {
#ifdef _WIN32
    closeDevice();
    MMRESULT result = midiInOpen(&m_midiIn, index, (DWORD_PTR)midiCallback,
                                  (DWORD_PTR)this, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        std::cerr << "[MIDI] Failed to open device " << index << std::endl;
        return false;
    }
    midiInStart(m_midiIn);
    m_open = true;
    m_deviceIdx = index;
    std::cout << "[MIDI] Opened device " << index << std::endl;
    return true;
#else
    return false;
#endif
}

void MIDIManager::closeDevice() {
#ifdef _WIN32
    if (m_midiIn) {
        midiInStop(m_midiIn);
        midiInClose(m_midiIn);
        m_midiIn = nullptr;
    }
#endif
    m_open = false;
    m_deviceIdx = -1;
}

#ifdef _WIN32
void CALLBACK MIDIManager::midiCallback(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance,
                                          DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (wMsg != MIM_DATA) return;
    auto* self = (MIDIManager*)dwInstance;

    uint8_t status = dwParam1 & 0xFF;
    uint8_t data1 = (dwParam1 >> 8) & 0xFF;
    uint8_t data2 = (dwParam1 >> 16) & 0xFF;

    MIDIEvent evt;
    evt.channel = status & 0x0F;

    uint8_t msgType = status & 0xF0;
    if (msgType == 0xB0) {
        // Control Change
        evt.type = 0;
        evt.number = data1;
        evt.value = data2;
    } else if (msgType == 0x90) {
        // Note On (velocity 0 = note off)
        evt.type = (data2 > 0) ? 1 : 2;
        evt.number = data1;
        evt.value = data2;
    } else if (msgType == 0x80) {
        // Note Off
        evt.type = 2;
        evt.number = data1;
        evt.value = 0;
    } else {
        return; // ignore other message types
    }

    std::lock_guard<std::mutex> lock(self->m_eventMutex);
    self->m_pendingEvents.push_back(evt);

    if (self->m_learning) {
        self->m_lastLearnEvent = evt;
    }
}
#endif

std::vector<MIDIEvent> MIDIManager::pollEvents() {
    std::lock_guard<std::mutex> lock(m_eventMutex);
    auto result = std::move(m_pendingEvents);
    m_pendingEvents.clear();
    return result;
}

void MIDIManager::addMapping(const MIDIMapping& mapping) {
    m_mappings.push_back(mapping);
}

void MIDIManager::removeMapping(int index) {
    if (index >= 0 && index < (int)m_mappings.size()) {
        m_mappings.erase(m_mappings.begin() + index);
    }
}

std::vector<MIDIManager::MappingAction> MIDIManager::processEvents(const std::vector<MIDIEvent>& events) {
    std::vector<MappingAction> actions;
    for (const auto& evt : events) {
        for (const auto& map : m_mappings) {
            // Match channel (-1 = any)
            if (map.channel >= 0 && map.channel != evt.channel) continue;
            // Match type (CC=0, Note=1)
            if (map.type == 0 && evt.type != 0) continue;
            if (map.type == 1 && evt.type != 1 && evt.type != 2) continue;
            // Match number
            if (map.number != evt.number) continue;

            MappingAction action;
            action.target = map.target;
            action.layerIndex = map.layerIndex;
            action.sceneIndex = map.sceneIndex;
            action.value = evt.value / 127.0f;
            actions.push_back(action);
        }
    }
    return actions;
}
