#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <nlohmann/json.hpp>
#include "stb_image.h"
#include "stb_image_write.h"
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#include "tiny_gltf.h"

#include "warp/ObjMeshWarp.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <map>
#include <limits>

// --- OrbitCamera ---

glm::mat4 OrbitCamera::viewMatrix() const {
    float cosEl = cosf(elevation);
    float sinEl = sinf(elevation);
    float cosAz = cosf(azimuth);
    float sinAz = sinf(azimuth);

    glm::vec3 eye = target + distance * glm::vec3(cosEl * sinAz, sinEl, cosEl * cosAz);
    return glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projMatrix(float aspect) const {
    return glm::perspective(glm::radians(fovDeg), aspect, 0.01f, 100.0f);
}

// --- ObjMeshWarp ---

bool ObjMeshWarp::init() {
    if (!m_shader.loadFromFiles("shaders/objmesh.vert", "shaders/objmesh.frag")) {
        std::cerr << "ObjMeshWarp: failed to load shaders" << std::endl;
        return false;
    }
    return true;
}

bool ObjMeshWarp::isLoaded() const {
    for (const auto& mg : m_materials) {
        if (mg.mesh.isLoaded()) return true;
    }
    return false;
}

bool ObjMeshWarp::loadModel(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool ok = false;
    if (ext == ".gltf" || ext == ".glb") {
        ok = loadGLTF(path);
    } else {
        ok = loadOBJ(path);
    }

    if (ok) {
        // Auto-center camera on the model's bounding box
        m_camera.target = m_bboxCenter;
        m_camera.distance = m_bboxExtent * 1.5f;
        if (m_camera.distance < 0.5f) m_camera.distance = 3.0f;
        m_modelPosition = {0.0f, 0.0f, 0.0f};
        m_modelScale = 1.0f;
    }

    return ok;
}

bool ObjMeshWarp::loadOBJ(const std::string& path) {
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;

    if (!reader.ParseFromFile(path, config)) {
        if (!reader.Error().empty()) {
            std::cerr << "OBJ load error: " << reader.Error() << std::endl;
        }
        return false;
    }
    if (!reader.Warning().empty()) {
        std::cerr << "OBJ warning: " << reader.Warning() << std::endl;
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& mats = reader.GetMaterials();

    // Collect geometry per material index
    // materialIdx -1 = no material assigned
    std::map<int, std::pair<std::vector<Vertex3D>, std::vector<unsigned int>>> matGeom;

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
            int fv = shape.mesh.num_face_vertices[f];
            int matId = -1;
            if (f < shape.mesh.material_ids.size()) {
                matId = shape.mesh.material_ids[f];
            }

            auto& [verts, inds] = matGeom[matId];
            for (int v = 0; v < fv; v++) {
                const auto& index = shape.mesh.indices[indexOffset + v];
                Vertex3D vert{};
                vert.x = attrib.vertices[3 * index.vertex_index + 0];
                vert.y = attrib.vertices[3 * index.vertex_index + 1];
                vert.z = attrib.vertices[3 * index.vertex_index + 2];
                if (index.texcoord_index >= 0) {
                    vert.u = attrib.texcoords[2 * index.texcoord_index + 0];
                    vert.v = attrib.texcoords[2 * index.texcoord_index + 1];
                }
                inds.push_back((unsigned int)verts.size());
                verts.push_back(vert);
            }
            indexOffset += fv;
        }
    }

    // Compute bounding box from all vertices
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());
    for (const auto& [matId, geom] : matGeom) {
        for (const auto& v : geom.first) {
            bmin.x = std::min(bmin.x, v.x); bmin.y = std::min(bmin.y, v.y); bmin.z = std::min(bmin.z, v.z);
            bmax.x = std::max(bmax.x, v.x); bmax.y = std::max(bmax.y, v.y); bmax.z = std::max(bmax.z, v.z);
        }
    }
    m_bboxCenter = (bmin + bmax) * 0.5f;
    m_bboxExtent = glm::length(bmax - bmin) * 0.5f;

    m_materials.clear();
    int totalVerts = 0;
    for (auto& [matId, geom] : matGeom) {
        MaterialGroup mg;
        if (matId >= 0 && matId < (int)mats.size()) {
            mg.name = mats[matId].name;
        } else {
            mg.name = "Default";
        }
        mg.textured = true; // default all textured for OBJ
        mg.mesh.upload(geom.first, geom.second);
        totalVerts += (int)geom.first.size();
        m_materials.push_back(std::move(mg));
    }

    m_meshPath = path;
    std::cout << "Mesh loaded: " << path << " (" << totalVerts << " verts, "
              << m_materials.size() << " materials)" << std::endl;
    return !m_materials.empty();
}

// --- glTF helpers ---

struct PrimitiveData {
    std::vector<Vertex3D> vertices;
    std::vector<unsigned int> indices;
};

static void extractPrimitive(const tinygltf::Model& model,
                             const tinygltf::Primitive& primitive,
                             const glm::mat4& worldTransform,
                             PrimitiveData& out) {
    if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1)
        return;

    const float* positions = nullptr;
    size_t vertexCount = 0;
    if (primitive.attributes.count("POSITION")) {
        const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];
        const auto& bufView = model.bufferViews[accessor.bufferView];
        const auto& buf = model.buffers[bufView.buffer];
        positions = reinterpret_cast<const float*>(
            &buf.data[bufView.byteOffset + accessor.byteOffset]);
        vertexCount = accessor.count;
    }
    if (!positions || vertexCount == 0) return;

    const float* uvs = nullptr;
    if (primitive.attributes.count("TEXCOORD_0")) {
        const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
        const auto& bufView = model.bufferViews[accessor.bufferView];
        const auto& buf = model.buffers[bufView.buffer];
        uvs = reinterpret_cast<const float*>(
            &buf.data[bufView.byteOffset + accessor.byteOffset]);
    }

    unsigned int baseVertex = (unsigned int)out.vertices.size();
    for (size_t i = 0; i < vertexCount; i++) {
        glm::vec4 pos(positions[i*3+0], positions[i*3+1], positions[i*3+2], 1.0f);
        glm::vec4 transformed = worldTransform * pos;

        Vertex3D v{};
        v.x = transformed.x;
        v.y = transformed.y;
        v.z = transformed.z;
        if (uvs) {
            v.u = uvs[i * 2 + 0];
            v.v = uvs[i * 2 + 1];
        }
        out.vertices.push_back(v);
    }

    if (primitive.indices >= 0) {
        const auto& accessor = model.accessors[primitive.indices];
        const auto& bufView = model.bufferViews[accessor.bufferView];
        const auto& buf = model.buffers[bufView.buffer];
        const void* dataPtr = &buf.data[bufView.byteOffset + accessor.byteOffset];

        for (size_t i = 0; i < accessor.count; i++) {
            uint32_t idx = 0;
            switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                idx = ((const uint16_t*)dataPtr)[i]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                idx = ((const uint32_t*)dataPtr)[i]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                idx = ((const uint8_t*)dataPtr)[i]; break;
            }
            out.indices.push_back(baseVertex + idx);
        }
    } else {
        for (unsigned int i = 0; i < (unsigned int)vertexCount; i++) {
            out.indices.push_back(baseVertex + i);
        }
    }
}

static glm::mat4 nodeTransform(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 m;
        for (int i = 0; i < 16; i++)
            (&m[0][0])[i] = (float)node.matrix[i];
        return m;
    }

    glm::mat4 T(1.0f), R(1.0f), S(1.0f);
    if (node.translation.size() == 3) {
        T = glm::translate(glm::mat4(1.0f), glm::vec3(
            (float)node.translation[0], (float)node.translation[1], (float)node.translation[2]));
    }
    if (node.rotation.size() == 4) {
        glm::quat q((float)node.rotation[3], (float)node.rotation[0],
                     (float)node.rotation[1], (float)node.rotation[2]);
        R = glm::mat4_cast(q);
    }
    if (node.scale.size() == 3) {
        S = glm::scale(glm::mat4(1.0f), glm::vec3(
            (float)node.scale[0], (float)node.scale[1], (float)node.scale[2]));
    }
    return T * R * S;
}

// Walk nodes, collecting geometry grouped by node name
static void processNodeByName(const tinygltf::Model& model, int nodeIdx,
                              const glm::mat4& parentTransform,
                              std::map<std::string, PrimitiveData>& groups) {
    const auto& node = model.nodes[nodeIdx];
    glm::mat4 world = parentTransform * nodeTransform(node);

    if (node.mesh >= 0 && node.mesh < (int)model.meshes.size()) {
        std::string name = node.name.empty() ? ("Mesh " + std::to_string(node.mesh)) : node.name;
        const auto& mesh = model.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives) {
            extractPrimitive(model, primitive, world, groups[name]);
        }
    }

    for (int child : node.children) {
        processNodeByName(model, child, world, groups);
    }
}

// Walk nodes, collecting geometry grouped by material index
static void processNodeByMaterial(const tinygltf::Model& model, int nodeIdx,
                                  const glm::mat4& parentTransform,
                                  std::map<int, PrimitiveData>& matGeom) {
    const auto& node = model.nodes[nodeIdx];
    glm::mat4 world = parentTransform * nodeTransform(node);

    if (node.mesh >= 0 && node.mesh < (int)model.meshes.size()) {
        const auto& mesh = model.meshes[node.mesh];
        for (const auto& primitive : mesh.primitives) {
            int matIdx = primitive.material;
            extractPrimitive(model, primitive, world, matGeom[matIdx]);
        }
    }

    for (int child : node.children) {
        processNodeByMaterial(model, child, world, matGeom);
    }
}

bool ObjMeshWarp::loadGLTF(const std::string& path) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool ok = false;
    if (ext == ".glb") {
        ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    } else {
        ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    }

    if (!warn.empty()) std::cerr << "glTF warning: " << warn << std::endl;
    if (!err.empty()) std::cerr << "glTF error: " << err << std::endl;
    if (!ok) return false;

    int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (sceneIdx >= (int)model.scenes.size()) return false;
    const auto& scene = model.scenes[sceneIdx];

    bool hasMaterials = !model.materials.empty();

    m_materials.clear();
    int totalVerts = 0;

    // Helper to expand bounding box from a set of vertices
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(std::numeric_limits<float>::lowest());
    auto expandBBox = [&](const std::vector<Vertex3D>& verts) {
        for (const auto& v : verts) {
            bmin.x = std::min(bmin.x, v.x); bmin.y = std::min(bmin.y, v.y); bmin.z = std::min(bmin.z, v.z);
            bmax.x = std::max(bmax.x, v.x); bmax.y = std::max(bmax.y, v.y); bmax.z = std::max(bmax.z, v.z);
        }
    };

    if (hasMaterials) {
        // Group by material
        std::map<int, PrimitiveData> matGeom;
        for (int rootNode : scene.nodes) {
            processNodeByMaterial(model, rootNode, glm::mat4(1.0f), matGeom);
        }
        for (auto& [matIdx, data] : matGeom) {
            if (data.vertices.empty()) continue;
            expandBBox(data.vertices);
            MaterialGroup mg;
            if (matIdx >= 0 && matIdx < (int)model.materials.size()) {
                mg.name = model.materials[matIdx].name;
                if (mg.name.empty()) mg.name = "Material " + std::to_string(matIdx);
            } else {
                mg.name = "Default";
            }
            mg.textured = true;
            mg.mesh.upload(data.vertices, data.indices);
            totalVerts += (int)data.vertices.size();
            m_materials.push_back(std::move(mg));
        }
    } else {
        // No materials — group by node name
        std::map<std::string, PrimitiveData> groups;
        for (int rootNode : scene.nodes) {
            processNodeByName(model, rootNode, glm::mat4(1.0f), groups);
        }
        for (auto& [name, data] : groups) {
            if (data.vertices.empty()) continue;
            expandBBox(data.vertices);
            MaterialGroup mg;
            mg.name = name;
            mg.textured = true;
            mg.mesh.upload(data.vertices, data.indices);
            totalVerts += (int)data.vertices.size();
            m_materials.push_back(std::move(mg));
        }
    }

    m_bboxCenter = (bmin + bmax) * 0.5f;
    m_bboxExtent = glm::length(bmax - bmin) * 0.5f;

    m_meshPath = path;
    std::cout << "Mesh loaded: " << path << " (" << totalVerts << " verts, "
              << m_materials.size() << " groups)" << std::endl;
    return !m_materials.empty();
}

void ObjMeshWarp::render(GLuint sourceTexture, float aspect) {
    if (!isLoaded()) return;

    glm::mat4 mdl = glm::translate(glm::mat4(1.0f), m_modelPosition);
    mdl = glm::scale(mdl, glm::vec3(m_modelScale));

    glm::mat4 view = m_camera.viewMatrix();
    glm::mat4 proj = m_camera.projMatrix(aspect);
    glm::mat4 mvp = proj * view * mdl;

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    m_shader.use();
    m_shader.setMat4("uMVP", mvp);
    m_shader.setInt("uTexture", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);

    // First pass: solid fill for all groups (textured or dark solid)
    for (const auto& mg : m_materials) {
        if (!mg.mesh.isLoaded()) continue;
        m_shader.setBool("uTextured", mg.textured);
        if (!mg.textured) {
            m_shader.setVec4("uSolidColor", glm::vec4(0.03f, 0.03f, 0.05f, 1.0f));
        }
        mg.mesh.draw();
    }

    // Second pass: wireframe overlay for untextured groups so structure is visible
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    m_shader.setBool("uTextured", false);
    m_shader.setVec4("uSolidColor", glm::vec4(0.15f, 0.18f, 0.22f, 1.0f));
    for (const auto& mg : m_materials) {
        if (!mg.mesh.isLoaded() || mg.textured) continue;
        mg.mesh.draw();
    }
    glDisable(GL_POLYGON_OFFSET_LINE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glDisable(GL_DEPTH_TEST);
}
