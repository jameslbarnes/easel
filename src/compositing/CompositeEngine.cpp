#include "compositing/CompositeEngine.h"
#include <glm/glm.hpp>
#include <iostream>

bool CompositeEngine::init(int width, int height) {
    m_width = width;
    m_height = height;

    if (!m_fbo[0].create(width, height) || !m_fbo[1].create(width, height)) {
        std::cerr << "Failed to create composite FBOs" << std::endl;
        return false;
    }

    if (!m_compositeShader.loadFromFiles("shaders/passthrough.vert", "shaders/composite.frag")) {
        std::cerr << "Failed to load composite shader" << std::endl;
        return false;
    }

    if (!m_passthroughShader.loadFromFiles("shaders/passthrough.vert", "shaders/passthrough.frag")) {
        std::cerr << "Failed to load passthrough shader" << std::endl;
        return false;
    }

    m_quad.createQuad();
    return true;
}

void CompositeEngine::resize(int width, int height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    m_fbo[0].resize(width, height);
    m_fbo[1].resize(width, height);
}

void CompositeEngine::clear() {
    for (int i = 0; i < 2; i++) {
        m_fbo[i].bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    m_current = 0;
}

void CompositeEngine::composite(const LayerStack& stack) {
    clear();

    bool firstLayer = true;
    for (int i = 0; i < stack.count(); i++) {
        const auto& layer = stack[i];
        if (!layer->visible || layer->opacity <= 0.0f || !layer->textureId()) continue;

        int next = 1 - m_current;
        m_fbo[next].bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        if (firstLayer && layer->blendMode == BlendMode::Normal) {
            // First layer: just draw it directly
            m_passthroughShader.use();
            m_passthroughShader.setMat3("uTransform", layer->getTransformMatrix());
            m_passthroughShader.setFloat("uOpacity", layer->opacity);
            m_passthroughShader.setInt("uTexture", 0);
            m_passthroughShader.setInt("uTileX", layer->tileX);
            m_passthroughShader.setInt("uTileY", layer->tileY);
            m_passthroughShader.setVec4("uCrop", glm::vec4(
                layer->cropLeft, layer->cropRight, layer->cropTop, layer->cropBottom));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, layer->textureId());

            // Mask support for first layer
            if (layer->mask && layer->mask->id()) {
                m_passthroughShader.setBool("uHasMask", true);
                m_passthroughShader.setInt("uMask", 1);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, layer->mask->id());
            } else {
                m_passthroughShader.setBool("uHasMask", false);
            }
        } else {
            m_compositeShader.use();
            // Draw a full-screen quad (identity transform) — the fragment
            // shader uses uInvTransform to sample the layer texture at the
            // correct position.
            m_compositeShader.setMat3("uTransform", glm::mat3(1.0f));
            glm::mat3 layerXform = layer->getTransformMatrix();
            m_compositeShader.setMat3("uInvTransform", glm::inverse(layerXform));
            m_compositeShader.setInt("uBase", 0);
            m_compositeShader.setInt("uLayer", 1);
            m_compositeShader.setFloat("uOpacity", layer->opacity);
            m_compositeShader.setInt("uBlendMode", (int)layer->blendMode);
            m_compositeShader.setInt("uTileX", layer->tileX);
            m_compositeShader.setInt("uTileY", layer->tileY);
            m_compositeShader.setVec4("uCrop", glm::vec4(
                layer->cropLeft, layer->cropRight, layer->cropTop, layer->cropBottom));

            // Base (accumulated)
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_fbo[m_current].textureId());

            // Current layer
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, layer->textureId());

            // Mask
            if (layer->mask && layer->mask->id()) {
                m_compositeShader.setBool("uHasMask", true);
                m_compositeShader.setInt("uMask", 2);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, layer->mask->id());
            } else {
                m_compositeShader.setBool("uHasMask", false);
            }
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_quad.draw();
        glDisable(GL_BLEND);

        m_current = next;
        firstLayer = false;
    }

    Framebuffer::unbind();
}

GLuint CompositeEngine::resultTexture() const {
    return m_fbo[m_current].textureId();
}
