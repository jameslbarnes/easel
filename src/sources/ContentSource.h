#pragma once
#include <glad/glad.h>
#include <string>

class ContentSource {
public:
    virtual ~ContentSource() = default;

    virtual void update() {}
    virtual GLuint textureId() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual std::string typeName() const = 0;
    virtual std::string sourcePath() const { return ""; }

    // Type queries
    virtual bool isVideo() const { return false; }
    virtual bool isShader() const { return false; }
    virtual void play() {}
    virtual void pause() {}
    virtual void seek(double) {}
    virtual bool isPlaying() const { return false; }
    virtual double duration() const { return 0.0; }
    virtual double currentTime() const { return 0.0; }
};
