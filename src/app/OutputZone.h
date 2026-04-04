#pragma once
#include "compositing/CompositeEngine.h"
#include "render/Framebuffer.h"
#ifdef HAS_NDI
#include "app/NDIOutput.h"
#endif
#include <string>
#include <set>
#include <cstdint>

enum class OutputDest { None, Fullscreen, NDI };

struct OutputZone {
    std::string name = "Main";
    int width = 1920;
    int height = 1080;
    int compPreset = 0; // resolution preset index

    CompositeEngine compositor;
    Framebuffer warpFBO;
    GLuint canvasTexture = 0; // post-mask, pre-warp texture for canvas preview

    // Mapping profile index (-1 = none/passthrough, >=0 = index into Application::m_mappings)
    int mappingIndex = 0;

    // Layer visibility
    bool showAllLayers = true;
    std::set<uint32_t> visibleLayerIds;

    // Output routing
    OutputDest outputDest = OutputDest::None;
    int outputMonitor = -1;         // for Fullscreen: which monitor index
    std::string ndiStreamName;      // for NDI: stream name (defaults to zone name)

#ifdef HAS_NDI
    NDIOutput ndiOutput;            // per-zone NDI sender
#endif

    bool init();
    void resize(int w, int h);
};
