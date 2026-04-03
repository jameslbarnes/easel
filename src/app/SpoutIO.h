#pragma once
// Spout I/O stub - requires Spout2 SDK to be installed
// When HAS_SPOUT is defined, provides SpoutSender and SpoutReceiver wrappers
// For now, this is a conditional feature that can be enabled when the SDK is available

#include "render/Texture.h"
#include <string>

#ifdef HAS_SPOUT
#include "SpoutLibrary.h"
#endif

class SpoutSender {
public:
    bool create(const std::string& name, int width, int height);
    void send(GLuint texture, int width, int height);
    void destroy();
    bool isActive() const { return m_active; }
    const std::string& name() const { return m_name; }

private:
    std::string m_name;
    bool m_active = false;
#ifdef HAS_SPOUT
    SPOUTLIBRARY* m_spout = nullptr;
#endif
};

class SpoutReceiver {
public:
    bool connect(const std::string& name = "");
    GLuint receive(int& width, int& height);
    void disconnect();
    bool isActive() const { return m_active; }
    const std::string& senderName() const { return m_senderName; }

private:
    std::string m_senderName;
    bool m_active = false;
    Texture m_texture;
#ifdef HAS_SPOUT
    SPOUTLIBRARY* m_spout = nullptr;
#endif
};
