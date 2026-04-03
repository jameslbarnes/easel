#include "app/SpoutIO.h"
#include <iostream>

// --- SpoutSender ---

bool SpoutSender::create(const std::string& name, int width, int height) {
#ifdef HAS_SPOUT
    m_spout = GetSpout();
    if (!m_spout) return false;
    m_spout->CreateSender(name.c_str(), width, height);
    m_name = name;
    m_active = true;
    std::cout << "[Spout] Sender created: " << name << std::endl;
    return true;
#else
    (void)name; (void)width; (void)height;
    std::cout << "[Spout] Not available (Spout2 SDK not installed)" << std::endl;
    return false;
#endif
}

void SpoutSender::send(GLuint texture, int width, int height) {
#ifdef HAS_SPOUT
    if (m_spout && m_active) {
        m_spout->SendTexture(texture, GL_TEXTURE_2D, width, height);
    }
#else
    (void)texture; (void)width; (void)height;
#endif
}

void SpoutSender::destroy() {
#ifdef HAS_SPOUT
    if (m_spout) {
        m_spout->ReleaseSender();
        m_spout->Release();
        m_spout = nullptr;
    }
#endif
    m_active = false;
}

// --- SpoutReceiver ---

bool SpoutReceiver::connect(const std::string& name) {
#ifdef HAS_SPOUT
    m_spout = GetSpout();
    if (!m_spout) return false;
    char senderName[256] = {};
    if (!name.empty()) {
        strncpy(senderName, name.c_str(), sizeof(senderName) - 1);
    }
    unsigned int w = 0, h = 0;
    m_spout->CreateReceiver(senderName, w, h);
    m_senderName = senderName;
    m_active = true;
    if (w > 0 && h > 0) {
        m_texture.createEmpty(w, h);
    }
    std::cout << "[Spout] Receiver connected: " << m_senderName << std::endl;
    return true;
#else
    (void)name;
    std::cout << "[Spout] Not available (Spout2 SDK not installed)" << std::endl;
    return false;
#endif
}

GLuint SpoutReceiver::receive(int& width, int& height) {
#ifdef HAS_SPOUT
    if (!m_spout || !m_active) return 0;
    unsigned int w = m_texture.width(), h = m_texture.height();
    bool newFrame = m_spout->ReceiveTexture(m_texture.id(), GL_TEXTURE_2D, false, w, h);
    if (w != (unsigned int)m_texture.width() || h != (unsigned int)m_texture.height()) {
        m_texture.createEmpty(w, h);
    }
    width = w;
    height = h;
    return newFrame ? m_texture.id() : 0;
#else
    (void)width; (void)height;
    return 0;
#endif
}

void SpoutReceiver::disconnect() {
#ifdef HAS_SPOUT
    if (m_spout) {
        m_spout->ReleaseReceiver();
        m_spout->Release();
        m_spout = nullptr;
    }
#endif
    m_active = false;
}
