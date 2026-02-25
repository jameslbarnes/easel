#pragma once
#ifdef HAS_NDI

#include "sources/ContentSource.h"
#include "sources/NDIRuntime.h"
#include "render/Texture.h"
#include <string>
#include <vector>

struct NDISenderInfo {
    std::string name;
    std::string url;
};

class NDISource : public ContentSource {
public:
    ~NDISource();

    // Discover NDI senders on the network
    static std::vector<NDISenderInfo> findSources(uint32_t waitMs = 500);

    bool connect(const std::string& senderName);
    void disconnect();
    bool isConnected() const { return m_recv != nullptr; }

    void update() override;
    GLuint textureId() const override { return m_texture.id(); }
    int width() const override { return m_width; }
    int height() const override { return m_height; }
    std::string typeName() const override { return "NDI"; }
    std::string sourcePath() const override { return m_senderName; }

private:
    NDIlib_recv_instance_t m_recv = nullptr;
    std::string m_senderName;
    Texture m_texture;
    int m_width = 0;
    int m_height = 0;
    std::vector<uint8_t> m_pixelBuffer;
};

#endif // HAS_NDI
