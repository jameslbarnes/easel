// Exercises the real shader uniform upload and binding paths with a hidden GL
// context. In particular, Off must clear features populated before the toggle.
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "sources/ShaderSource.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static void expectPixel(ShaderSource& shader, float r, float g, float b) {
    shader.update();
    unsigned char pixels[4 * 4 * 4]{};
    glBindTexture(GL_TEXTURE_2D, shader.textureId());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    require(std::abs(pixels[0] / 255.0f - r) < 0.02f, "legacy bass uniform mismatch");
    require(std::abs(pixels[1] / 255.0f - g) < 0.02f, "extended audio clock mismatch");
    require(std::abs(pixels[2] / 255.0f - b) < 0.02f, "extended beat uniform mismatch");
}

int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(4, 4, "shader audio regression", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 1;
    int result = 0;
    try {
        ShaderSource shader;
        shader.setResolution(4, 4);
        require(shader.loadFromCode(R"(/*{"ISFVSN":"2","INPUTS":[
            {"NAME":"amount","TYPE":"float","DEFAULT":0.25,"MIN":0,"MAX":1}
        ]}*/
        void main() { gl_FragColor = vec4(audioBass, audioTime, audioBeatPulse, 1.0); }
        )"), "test shader failed to compile");
        AudioFeatures loud;
        loud.bass = 0.8f; loud.levelTime = 0.6f; loud.beatPulse = 0.4f;
        shader.setAudioFeatures(loud);
        expectPixel(shader, 0.8f, 0.6f, 0.4f);
        shader.setAudioEnabled(false);
        expectPixel(shader, 0, 0, 0);
        shader.setAudioFeatures(loud);
        shader.setAudioState(1, 1, 1, 1, 0);
        expectPixel(shader, 0, 0, 0);
        auto& binding = shader.audioBindings()["amount"];
        binding.signal = AudioSignal::Bass;
        for (int i = 0; i < 120; ++i)
            shader.applyAudioBindings(1, 1, 1, 1, 1, 1.0f / 60);
        require(std::get<float>(shader.inputs()[0].value) == 0.25f,
                "Off allowed an audio parameter binding to move");
        require(binding.signal == AudioSignal::Bass, "Off destroyed binding recipe");
        shader.setAudioEnabled(true);
        shader.setAudioFeatures(loud);
        expectPixel(shader, 0.8f, 0.6f, 0.4f);
        for (int i = 0; i < 120; ++i)
            shader.applyAudioBindings(1, 1, 1, 1, 1, 1.0f / 60);
        require(std::get<float>(shader.inputs()[0].value) > 0.8f,
                "On failed to resume the saved binding");
        // A freshly loaded shader must respect the same disabled master gate.
        require(shader.reload(R"(/*{"ISFVSN":"2"}*/
          void main() { gl_FragColor = vec4(audioBass, audioTime, audioBeatPulse, 1.0); }
        )"), "replacement shader failed to compile");
        shader.setAudioEnabled(false);
        shader.setAudioFeatures(loud);
        expectPixel(shader, 0, 0, 0);
        std::cout << "PASS: audio bus neutralized, bindings held/restored, shader reload gated\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}
