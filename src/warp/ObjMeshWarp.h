#pragma once
#include "render/Mesh3D.h"
#include "render/ShaderProgram.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct OrbitCamera {
    float azimuth = 0.0f;
    float elevation = 0.3f;
    float distance = 3.0f;
    glm::vec3 target = {0.0f, 0.0f, 0.0f};
    float fovDeg = 50.0f;

    glm::mat4 viewMatrix() const;
    glm::mat4 projMatrix(float aspect) const;
};

struct MaterialGroup {
    std::string name;
    Mesh3D mesh;
    bool textured = false; // true = receives projected texture, false = solid black
};

class ObjMeshWarp {
public:
    bool init();
    bool loadOBJ(const std::string& path);
    bool loadGLTF(const std::string& path);
    bool loadModel(const std::string& path); // auto-detect by extension
    void render(GLuint sourceTexture, float aspect);

    bool isLoaded() const;
    const std::string& meshPath() const { return m_meshPath; }
    const std::string& objPath() const { return m_meshPath; }

    OrbitCamera& camera() { return m_camera; }
    const OrbitCamera& camera() const { return m_camera; }

    float& modelScale() { return m_modelScale; }
    float modelScale() const { return m_modelScale; }
    glm::vec3& modelPosition() { return m_modelPosition; }
    const glm::vec3& modelPosition() const { return m_modelPosition; }

    std::vector<MaterialGroup>& materials() { return m_materials; }
    const std::vector<MaterialGroup>& materials() const { return m_materials; }

private:
    std::vector<MaterialGroup> m_materials;
    ShaderProgram m_shader;
    OrbitCamera m_camera;
    std::string m_meshPath;
    float m_modelScale = 1.0f;
    glm::vec3 m_modelPosition = {0.0f, 0.0f, 0.0f};
    glm::vec3 m_bboxCenter = {0.0f, 0.0f, 0.0f};
    float m_bboxExtent = 3.0f;
};
