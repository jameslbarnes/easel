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

void CompositeEngine::setAudioUniforms(ShaderProgram& shader, float audioStrength) {
    shader.setFloat("uAudioRMS", m_audio.rms);
    shader.setFloat("uAudioStrength", audioStrength);
    shader.setFloat("uAudioBass", audioStrength > 0 ? m_audio.bass : 0.0f);
    shader.setFloat("uAudioLowMid", audioStrength > 0 ? m_audio.lowMid : 0.0f);
    shader.setFloat("uAudioHighMid", audioStrength > 0 ? m_audio.highMid : 0.0f);
    shader.setFloat("uAudioTreble", audioStrength > 0 ? m_audio.treble : 0.0f);
    shader.setFloat("uAudioBeatDecay", audioStrength > 0 ? m_audio.beatDecay : 0.0f);
    // BPM sync (always available, independent of audio reactivity toggle)
    shader.setFloat("uBeatPhase", m_audio.beatPhase);
    shader.setFloat("uBeatPulse", m_audio.beatPulse);
    shader.setFloat("uBarPhase", m_audio.barPhase);
    shader.setFloat("uBPM", m_audio.bpm);
}

void CompositeEngine::composite(const std::vector<std::shared_ptr<Layer>>& layers) {
    clear();

    bool firstLayer = true;
    for (int i = 0; i < (int)layers.size(); i++) {
        const auto& layer = layers[i];

        // Update transition state
        if (layer->transitionActive && layer->transitionDuration > 0.0f) {
            float step = (1.0f / layer->transitionDuration) * (1.0f / 60.0f); // approx per frame
            if (layer->transitionDirection) {
                layer->transitionProgress = std::min(1.0f, layer->transitionProgress + step);
                if (layer->transitionProgress >= 1.0f) layer->transitionActive = false;
            } else {
                layer->transitionProgress = std::max(0.0f, layer->transitionProgress - step);
                if (layer->transitionProgress <= 0.0f) layer->transitionActive = false;
            }
        }

        // Compute effective opacity including transition
        float effectiveOpacity = layer->opacity;
        if (layer->transitionDuration > 0.0f) {
            effectiveOpacity *= layer->transitionProgress;
        }

        // Skip fully transparent or invisible (but allow transitioning-out layers to render)
        if (effectiveOpacity <= 0.0f || !layer->textureId()) continue;
        if (!layer->visible && !layer->transitionActive) continue;

        int next = 1 - m_current;
        m_fbo[next].bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        float audioStrength = layer->audioReactive ? layer->audioStrength : 0.0f;

        // Build native-size transform: normalized by canvas height so aspect ratio
        // is always preserved.  scale (1,1) = source fills canvas height.
        // Layers look consistent regardless of canvas resolution/aspect.
        // When mosaic tiling is active, fill the canvas so the pattern covers everything.
        bool mosaicFill = (layer->tileX > 1.0f || layer->tileY > 1.0f ||
                           layer->mosaicMode != MosaicMode::Mirror);
        glm::mat3 nativeScale(1.0f);
        if (!mosaicFill && !layer->source->isShader()) {
            int lw = layer->width(), lh = layer->height();
            if (lw > 0 && lh > 0 && m_width > 0 && m_height > 0) {
                // Height-normalized: Y fills canvas, X preserves aspect
                float srcAspect = (float)lw / lh;
                float canvasAspect = (float)m_width / m_height;
                nativeScale[0][0] = srcAspect / canvasAspect;
                nativeScale[1][1] = 1.0f;
            }
        }
        glm::mat3 layerXform = layer->getTransformMatrix() * nativeScale;

        if (firstLayer && layer->blendMode == BlendMode::Normal) {
            // First layer: just draw it directly
            m_passthroughShader.use();
            m_passthroughShader.setMat3("uTransform", layerXform);
            m_passthroughShader.setBool("uFlipV", layer->source && layer->source->isFlippedV());
            m_passthroughShader.setFloat("uOpacity", effectiveOpacity);
            m_passthroughShader.setInt("uTexture", 0);
            m_passthroughShader.setFloat("uTileX", layer->tileX);
            m_passthroughShader.setFloat("uTileY", layer->tileY);
            m_passthroughShader.setVec4("uCrop", glm::vec4(
                layer->cropLeft, layer->cropRight, layer->cropTop, layer->cropBottom));
            m_passthroughShader.setInt("uMosaicMode", (int)layer->mosaicMode);
            m_passthroughShader.setFloat("uMosaicDensity", layer->mosaicDensity);
            m_passthroughShader.setFloat("uMosaicSpin", layer->mosaicSpin);
            m_passthroughShader.setFloat("uTime", m_audio.time);
            m_passthroughShader.setFloat("uFeather", layer->feather);
            setAudioUniforms(m_passthroughShader, audioStrength);
            {
                float mt = glm::clamp((m_audio.time - layer->mosaicTransitionStart) / layer->mosaicTransitionDuration, 0.0f, 1.0f);
                m_passthroughShader.setInt("uMosaicModeFrom", (int)layer->mosaicModeFrom);
                m_passthroughShader.setFloat("uMosaicTransition", mt);
            }
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
            m_compositeShader.setMat3("uInvTransform", glm::inverse(layerXform));
            m_compositeShader.setBool("uFlipV", layer->source && layer->source->isFlippedV());
            m_compositeShader.setInt("uBase", 0);
            m_compositeShader.setInt("uLayer", 1);
            m_compositeShader.setFloat("uOpacity", effectiveOpacity);
            m_compositeShader.setInt("uBlendMode", (int)layer->blendMode);
            m_compositeShader.setFloat("uTileX", layer->tileX);
            m_compositeShader.setFloat("uTileY", layer->tileY);
            m_compositeShader.setVec4("uCrop", glm::vec4(
                layer->cropLeft, layer->cropRight, layer->cropTop, layer->cropBottom));
            m_compositeShader.setInt("uMosaicMode", (int)layer->mosaicMode);
            m_compositeShader.setFloat("uMosaicDensity", layer->mosaicDensity);
            m_compositeShader.setFloat("uMosaicSpin", layer->mosaicSpin);
            m_compositeShader.setFloat("uTime", m_audio.time);
            m_compositeShader.setFloat("uFeather", layer->feather);
            setAudioUniforms(m_compositeShader, audioStrength);
            {
                float mt = glm::clamp((m_audio.time - layer->mosaicTransitionStart) / layer->mosaicTransitionDuration, 0.0f, 1.0f);
                m_compositeShader.setInt("uMosaicModeFrom", (int)layer->mosaicModeFrom);
                m_compositeShader.setFloat("uMosaicTransition", mt);
            }

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
