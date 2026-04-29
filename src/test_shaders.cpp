#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "sources/ShaderSource.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

namespace fs = std::filesystem;

struct TestResult {
    std::string file;
    bool parsed = false;
    bool compiled = false;
    std::string error;
    int inputCount = 0;
};

int main(int argc, char* argv[]) {
    std::string shadersDir = "shaders";
    if (argc > 1) shadersDir = argv[1];

    if (!fs::exists(shadersDir)) {
        std::cerr << "Directory not found: " << shadersDir << std::endl;
        return 1;
    }

    // Hidden GL context for shader compilation
    glfwSetErrorCallback([](int, const char* msg) { std::cerr << "GLFW: " << msg << std::endl; });
    if (!glfwInit()) { std::cerr << "glfwInit failed" << std::endl; return 1; }

#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
#endif
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(64, 64, "test", nullptr, nullptr);
    if (!win) { std::cerr << "Window creation failed" << std::endl; return 1; }
    glfwMakeContextCurrent(win);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "Testing shaders in: " << shadersDir << "\n\n";

    // Collect all .fs files
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(shadersDir)) {
        if (entry.path().extension() == ".fs") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<TestResult> results;
    int pass = 0, fail = 0;

    for (const auto& path : files) {
        TestResult r;
        r.file = fs::path(path).filename().string();

        // Redirect stderr to capture GL errors
        auto source = std::make_shared<ShaderSource>();
        if (source->loadFromFile(path)) {
            r.parsed = true;
            r.compiled = true;
            r.inputCount = (int)source->inputs().size();
            pass++;
        } else {
            // Try to distinguish parse vs compile failure
            std::ifstream f(path);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string code = ss.str();

            // Check if ISF header parses
            auto testParse = std::make_shared<ShaderSource>();
            // We can't easily separate parse from compile, so just report failure
            r.parsed = (code.find("/*") != std::string::npos && code.find("*/") != std::string::npos);
            r.compiled = false;
            r.error = "compilation failed";
            fail++;
        }

        const char* status = r.compiled ? "PASS" : "FAIL";
        const char* color = r.compiled ? "\033[32m" : "\033[31m";
        std::cout << color << "[" << status << "]\033[0m " << r.file;
        if (r.compiled) {
            std::cout << " (" << r.inputCount << " inputs)";
        } else {
            std::cout << " - " << r.error;
        }
        std::cout << std::endl;

        results.push_back(r);
    }

    std::cout << "\n--- Results ---\n";
    std::cout << "Total: " << results.size() << "  Pass: " << pass << "  Fail: " << fail << std::endl;

    if (fail > 0) {
        std::cout << "\nFailing shaders:\n";
        for (const auto& r : results) {
            if (!r.compiled) {
                std::cout << "  " << r.file << std::endl;
            }
        }
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return fail > 0 ? 1 : 0;
}
