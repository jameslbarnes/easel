#ifdef __APPLE__
#include "app/MIDIManager.h"
#include <CoreMIDI/CoreMIDI.h>
#include <iostream>

struct MacMIDIImpl {
    MIDIClientRef client = 0;
    MIDIPortRef inputPort = 0;
    MIDIManager* manager = nullptr;
};

static void midiNotifyProc(const MIDINotification* notification, void* refCon) {
    // Device connection/disconnection notifications - ignore for now
}

static void midiReadProc(const MIDIPacketList* pktList, void* readProcRefCon, void* srcConnRefCon) {
    auto* impl = static_cast<MacMIDIImpl*>(readProcRefCon);
    if (!impl || !impl->manager) return;

    const MIDIPacket* packet = &pktList->packet[0];
    for (UInt32 i = 0; i < pktList->numPackets; i++) {
        for (UInt16 j = 0; j < packet->length; ) {
            uint8_t status = packet->data[j];
            if (status < 0x80) { j++; continue; } // skip data bytes

            uint8_t channel = status & 0x0F;
            uint8_t msgType = status & 0xF0;

            MIDIEvent ev;
            ev.channel = channel;

            if (msgType == 0xB0 && j + 2 < packet->length) { // CC
                ev.type = 0;
                ev.number = packet->data[j + 1];
                ev.value = packet->data[j + 2];
                impl->manager->pushEvent(ev);
                j += 3;
            } else if (msgType == 0x90 && j + 2 < packet->length) { // Note On
                ev.type = (packet->data[j + 2] > 0) ? 1 : 2;
                ev.number = packet->data[j + 1];
                ev.value = packet->data[j + 2];
                impl->manager->pushEvent(ev);
                j += 3;
            } else if (msgType == 0x80 && j + 2 < packet->length) { // Note Off
                ev.type = 2;
                ev.number = packet->data[j + 1];
                ev.value = packet->data[j + 2];
                impl->manager->pushEvent(ev);
                j += 3;
            } else {
                j++;
                continue;
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

std::vector<std::string> MIDIManager::listDevices() {
    std::vector<std::string> devices;
    ItemCount sourceCount = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < sourceCount; i++) {
        MIDIEndpointRef src = MIDIGetSource(i);
        CFStringRef name = nullptr;
        MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name);
        if (name) {
            char buf[256];
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            devices.push_back(buf);
            CFRelease(name);
        } else {
            devices.push_back("MIDI Source " + std::to_string(i));
        }
    }
    return devices;
}

bool MIDIManager::openDevice(int index) {
    closeDevice();

    ItemCount sourceCount = MIDIGetNumberOfSources();
    if (index < 0 || index >= (int)sourceCount) return false;

    auto* impl = new MacMIDIImpl();
    impl->manager = this;
    m_macMidiImpl = impl;

    OSStatus status = MIDIClientCreate(CFSTR("Easel"), midiNotifyProc, impl, &impl->client);
    if (status != noErr) {
        std::cerr << "[MIDI] Failed to create MIDI client: " << status << std::endl;
        delete impl;
        m_macMidiImpl = nullptr;
        return false;
    }

    status = MIDIInputPortCreate(impl->client, CFSTR("Easel Input"), midiReadProc, impl, &impl->inputPort);
    if (status != noErr) {
        std::cerr << "[MIDI] Failed to create input port: " << status << std::endl;
        MIDIClientDispose(impl->client);
        delete impl;
        m_macMidiImpl = nullptr;
        return false;
    }

    MIDIEndpointRef src = MIDIGetSource(index);
    status = MIDIPortConnectSource(impl->inputPort, src, nullptr);
    if (status != noErr) {
        std::cerr << "[MIDI] Failed to connect source: " << status << std::endl;
        MIDIClientDispose(impl->client);
        delete impl;
        m_macMidiImpl = nullptr;
        return false;
    }

    m_open = true;
    m_deviceIdx = index;
    std::cout << "[MIDI] Opened device " << index << std::endl;
    return true;
}

void MIDIManager::closeDevice() {
    if (m_macMidiImpl) {
        auto* impl = static_cast<MacMIDIImpl*>(m_macMidiImpl);
        if (impl->client) MIDIClientDispose(impl->client);
        delete impl;
        m_macMidiImpl = nullptr;
    }
    m_open = false;
    m_deviceIdx = -1;
}
#endif
