#pragma once
#include <glad/glad.h>
#include <string>

// SCFONT/1 MSDF face (Phase D, phase-D-design.md 1.2-1.4): one MSDF atlas
// page + a 2048x256 RGBA8 per-codepoint metrics texture (8 texels/cp).
//   atlasInfo = (atlasWidthPx, atlasHeightPx, distanceRangePx, atlasEmPx)
//   faceInfo  = (ascender, descender, lineHeight, underlineY) in em
// real=false means the synthesized fallback face built from the 16x6
// coverage grid (glyphTexture()) because the assets were missing/bad.
struct MSDFFace {
    GLuint atlasTex = 0, metricsTex = 0;
    float atlasInfo[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float faceInfo[4]  = {0.0f, 0.0f, 0.0f, 0.0f};
    bool real = false;
    std::string id;
};

// Generates a 37-cell font atlas texture matching Shader-Claw 3 layout:
// A-Z (indices 0-25), space (26), 0-9 (indices 27-36)
// Each cell is 1/37 of the texture width. Shaders sample:
//   texture(fontAtlasTex, vec2((float(ch) + uv.x) / 37.0, uv.y)).r
class FontAtlas {
public:
    // Get the singleton atlas texture (created on first call)
    static GLuint texture();

    // SCTEXT/1 glyph atlas (Phase C): 16 columns x 6 rows of 128x180 cells
    // (2048x1080), cell idx = cp-32 for cp 32..126, cell 95 = '?' replacement.
    // Row 0 (cp 32..47) is the TOP band in GL texture space (v in [5/6,1]);
    // glyphs are upright. Shaders sample via the sc_text v1 helpers:
    //   vec2((col+uv.x)/16.0, 1.0-(row+1.0-uv.y)/6.0), uv.y = 1 at glyph top.
    // GL_RED + RRRR swizzle, LINEAR, CLAMP_TO_EDGE. Bound at unit 12 as
    // `glyphAtlasTex` when a shader references it.
    static GLuint glyphTexture();

    // SCFONT/1 (Phase D): where the shared font assets live
    // (<shader-claw3>/assets/fonts). EASEL_FONT_ASSETS env overrides.
    static void setAssetDir(const std::string& fontsDir);

    // Resolve + lazily load the MSDF face for an ISF fontFamily/fontWeight
    // (fonts.json families[index] -> nearest sctext.weight, ties lower).
    // Falls back to a face synthesized from the 16x6 glyph grid on any
    // failure (missing dir/file, parse error, bad PNG). Requires a current
    // GL context. Cached per face id.
    static const MSDFFace& msdfFace(int fontFamily, int fontWeight);

    // Destroy the textures (call at shutdown)
    static void destroy();

private:
    static GLuint s_texture;
    static GLuint s_glyphTexture;
    static void generate();
    static void generateGlyphGrid();
};
