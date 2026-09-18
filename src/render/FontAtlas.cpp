#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
// stb_image WITHOUT the IMPLEMENTATION define -- it lives in src/render/Texture.cpp
#include "stb_image.h"
#include "render/FontAtlas.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <string>

GLuint FontAtlas::s_texture = 0;
GLuint FontAtlas::s_glyphTexture = 0;

static void destroyMSDFFaces();

GLuint FontAtlas::texture() {
    if (!s_texture) generate();
    return s_texture;
}

GLuint FontAtlas::glyphTexture() {
    if (!s_glyphTexture) generateGlyphGrid();
    return s_glyphTexture;
}

void FontAtlas::destroy() {
    if (s_texture) {
        glDeleteTextures(1, &s_texture);
        s_texture = 0;
    }
    if (s_glyphTexture) {
        glDeleteTextures(1, &s_glyphTexture);
        s_glyphTexture = 0;
    }
    destroyMSDFFaces();
}

// --- shared helpers (legacy 37-cell strip + SCTEXT/1 glyph grid) ---

// Load the first available system font into `fontData`/`font`.
static bool loadSystemFont(std::vector<unsigned char>& fontData, stbtt_fontinfo& font) {
    const char* fontPaths[] = {
#ifdef __APPLE__
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
#elif defined(_WIN32)
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/calibri.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
#endif
        nullptr
    };

    for (int i = 0; fontPaths[i]; i++) {
        FILE* f = fopen(fontPaths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        fontData.resize(size);
        size_t bytesRead = fread(fontData.data(), 1, size, f);
        fclose(f);
        if (bytesRead != static_cast<size_t>(size)) continue;

        if (stbtt_InitFont(&font, fontData.data(), 0)) {
            return true;
        }
    }
    return false;
}

// Rasterize `codepoint` into a cellW x cellH cell of a single-channel atlas
// whose GL row 0 is the BOTTOM. (cellX0, cellRow0) = the cell's left pixel
// column and its bottom row in the atlas buffer. baselineY = baseline
// distance from the TOP of the cell (stb yoff is relative to the baseline,
// positive down). Rows are flipped so the glyph is upright in GL space.
static void rasterizeGlyphCell(const stbtt_fontinfo& font, float scale, int codepoint,
                               std::vector<unsigned char>& atlas, int atlasW, int atlasH,
                               int cellX0, int cellRow0, int cellW, int cellH,
                               int baselineY, bool verbose, int logIdx) {
    int glyphIdx = stbtt_FindGlyphIndex(&font, codepoint);
    if (!glyphIdx) {
        if (verbose) printf("FontAtlas: no glyph for '%c' (codepoint %d)\n", (char)codepoint, codepoint);
        return;
    }

    int gw, gh, xoff, yoff;
    unsigned char* bitmap = stbtt_GetGlyphBitmap(&font, scale, scale,
                                                  glyphIdx, &gw, &gh, &xoff, &yoff);
    if (!bitmap) {
        if (verbose) printf("FontAtlas: null bitmap for '%c'\n", (char)codepoint);
        return;
    }
    if (verbose) {
        printf("FontAtlas: [%d] '%c' glyph=%d size=%dx%d off=%d,%d\n",
               logIdx, (char)codepoint, glyphIdx, gw, gh, xoff, yoff);
    }

    // Center glyph horizontally in cell
    int dstX = cellX0 + (cellW - gw) / 2;

    for (int y = 0; y < gh; y++) {
        // Source row (top-down from stb_truetype); destination row flipped so
        // the baseline sits near the cell bottom in GL coords (row 0 = bottom).
        int rowInCell = cellH - 1 - (baselineY + yoff + y);
        if (rowInCell < 0 || rowInCell >= cellH) continue;
        int dstRow = cellRow0 + rowInCell;
        if (dstRow < 0 || dstRow >= atlasH) continue;
        for (int x = 0; x < gw; x++) {
            int dx = dstX + x;
            if (dx < cellX0 || dx >= cellX0 + cellW || dx < 0 || dx >= atlasW) continue;
            atlas[dstRow * atlasW + dx] = bitmap[y * gw + x];
        }
    }

    stbtt_FreeBitmap(bitmap, nullptr);
}

static GLuint uploadRedAtlas(const std::vector<unsigned char>& atlas, int atlasW, int atlasH) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlasW, atlasH, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Swizzle so .r reads work AND .rgb reads return white
    GLint swizzle[] = { GL_RED, GL_RED, GL_RED, GL_RED };
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    return tex;
}

void FontAtlas::generate() {
    // Atlas layout: 37 cells horizontally, each cellW x cellH pixels
    // A-Z (0-25), space (26), 0-9 (27-36)
    const int ATLAS_CHARS = 37;
    const int cellW = 128;
    const int cellH = 180; // ~5:7 aspect like bitmap font grid
    const int atlasW = ATLAS_CHARS * cellW;
    const int atlasH = cellH;

    std::vector<unsigned char> fontData;
    stbtt_fontinfo font;
    bool fontLoaded = loadSystemFont(fontData, font);

    if (!fontLoaded) {
        fprintf(stderr, "FontAtlas: could not load any system font\n");
        // Create a blank texture so we don't crash
        glGenTextures(1, &s_texture);
        glBindTexture(GL_TEXTURE_2D, s_texture);
        std::vector<unsigned char> blank(atlasW * atlasH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlasW, atlasH, 0,
                     GL_RED, GL_UNSIGNED_BYTE, blank.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return;
    }

    // Scale font to fill most of cell height
    float pixelHeight = cellH * 0.78f;
    float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    float scaledAscent = ascent * scale;

    // Character map: indices 0-25 = A-Z, 26 = space, 27-36 = 0-9
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789";

    // Allocate atlas buffer (single channel)
    std::vector<unsigned char> atlas(atlasW * atlasH, 0);

    // Position vertically: baseline at scaledAscent, yoff is relative to top of cell
    int baselineY = (int)(cellH * 0.15f + scaledAscent);

    for (int i = 0; i < ATLAS_CHARS; i++) {
        int codepoint = chars[i];
        if (codepoint == ' ') continue; // leave space cell blank
        rasterizeGlyphCell(font, scale, codepoint, atlas, atlasW, atlasH,
                           i * cellW, 0, cellW, cellH, baselineY, true, i);
    }

    s_texture = uploadRedAtlas(atlas, atlasW, atlasH);

    printf("FontAtlas: generated %dx%d atlas (%d chars)\n", atlasW, atlasH, ATLAS_CHARS);
}

void FontAtlas::generateGlyphGrid() {
    // SCTEXT/1 glyph atlas: 16 columns x 6 rows, cell idx = cp - 32 for
    // printable ASCII 32..126, cell 95 = replacement glyph ('?').
    const int COLS = 16, ROWS = 6;
    const int cellW = 128;
    const int cellH = 180;
    const int atlasW = COLS * cellW;   // 2048
    const int atlasH = ROWS * cellH;   // 1080

    std::vector<unsigned char> atlas((size_t)atlasW * atlasH, 0);

    std::vector<unsigned char> fontData;
    stbtt_fontinfo font;
    if (!loadSystemFont(fontData, font)) {
        fprintf(stderr, "FontAtlas: could not load any system font for the glyph grid\n");
        s_glyphTexture = uploadRedAtlas(atlas, atlasW, atlasH); // blank, never crash
        return;
    }

    // ~0.72 of the cell height so descenders fit above the cell bottom;
    // baseline ~0.25*cellH above the bottom (= 0.75*cellH from the top).
    float pixelHeight = cellH * 0.72f;
    float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);
    int baselineY = (int)(cellH * 0.75f);

    for (int idx = 0; idx < COLS * ROWS; idx++) {
        int cp = (idx == 95) ? '?' : 32 + idx;
        if (cp == ' ') continue; // blank cell
        int col = idx % COLS;
        int row = idx / COLS;
        // Row 0 is the TOP band in GL texture space (v in [5/6, 1]) — GL row
        // 0 is the bottom of the buffer, so row r starts at (ROWS-1-r)*cellH.
        int cellRow0 = (ROWS - 1 - row) * cellH;
        rasterizeGlyphCell(font, scale, cp, atlas, atlasW, atlasH,
                           col * cellW, cellRow0, cellW, cellH, baselineY, false, idx);
    }

    s_glyphTexture = uploadRedAtlas(atlas, atlasW, atlasH);
    printf("FontAtlas: generated %dx%d glyph grid (%d cells)\n", atlasW, atlasH, COLS * ROWS);
}

// --- SCFONT/1 MSDF faces (Phase D, phase-D-design.md 1.2-1.4, 1.7) ---------

namespace {

std::string s_assetDir;                          // set by setAssetDir
std::map<std::string, MSDFFace> s_msdfFaces;     // loaded real faces, by id
MSDFFace s_msdfFallback;                         // synthesized grid face
bool s_msdfFallbackBuilt = false;
bool s_msdfFallbackLogged = false;

// fonts.json index cache (per asset dir)
struct MSDFIndex {
    std::string dir;      // dir it was loaded from
    bool ok = false;
    nlohmann::json families;                 // fonts.json "families" array
    std::map<std::string, int> faceWeights;  // face id -> sctext.weight
};
MSDFIndex s_msdfIndex;

std::string resolveAssetDir() {
    const char* env = getenv("EASEL_FONT_ASSETS");
    if (env && env[0]) return env;
    return s_assetDir;
}

bool readJsonFile(const std::string& path, nlohmann::json& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    try {
        f >> out;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// SCFONT/1 metrics codec (mirrors shader-claw3 tools/fonts/metrics-codec.mjs):
// 8 RGBA8 texels per code point, texel x = (cp&255)*8+k, page cp/256 at GL
// row `page` counted from row 0 = bottom (buf is uploaded unflipped).
void encodeMetricTexels(std::vector<unsigned char>& buf, int cp,
                        float atlasL, float atlasB, float atlasR, float atlasT,
                        float planeL, float planeB, float planeR, float planeT,
                        float advanceEm, bool present) {
    if (cp < 0 || cp > 0xFFFF) return;
    auto u16 = [](float v) -> int {
        long r = std::lround(v);
        return (int)(r < 0 ? 0 : (r > 65535 ? 65535 : r));
    };
    auto s16em = [](float em) -> int {
        long r = std::lround(em * 4096.0f) + 32768;
        return (int)(r < 0 ? 0 : (r > 65535 ? 65535 : r));
    };
    const int page = cp >> 8, lo = cp & 255;
    size_t base = ((size_t)page * 2048 + (size_t)lo * 8) * 4;
    auto put = [&](int k, int r, int g, int b, int a) {
        buf[base + k * 4 + 0] = (unsigned char)r;
        buf[base + k * 4 + 1] = (unsigned char)g;
        buf[base + k * 4 + 2] = (unsigned char)b;
        buf[base + k * 4 + 3] = (unsigned char)a;
    };
    int aL = u16(atlasL), aB = u16(atlasB), aR = u16(atlasR), aT = u16(atlasT);
    put(0, aL & 255, aL >> 8, aB & 255, aB >> 8);
    put(1, aR & 255, aR >> 8, aT & 255, aT >> 8);
    int pL = s16em(planeL), pB = s16em(planeB), pR = s16em(planeR), pT = s16em(planeT);
    put(2, pL & 255, pL >> 8, pB & 255, pB >> 8);
    put(3, pR & 255, pR >> 8, pT & 255, pT >> 8);
    int adv = s16em(advanceEm);
    put(4, adv & 255, adv >> 8, present ? 255 : 0, 255);
    // k = 5..7 stay zero (reserved)
}

GLuint uploadMetricsTexture(const std::vector<unsigned char>& buf) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint prevAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2048, 256, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

// Synthesized fallback face from the 16x6 coverage grid (phase-D-design.md
// 1.7): glyphMSDFTex = the grid texture (RRRR swizzle -> .rgb = coverage),
// px range 2 = "treat as coverage"; metrics texels for cp 32..126 + 65533
// (cell 95); advance from stbtt hmtx (0.6 em when no system font loads).
const MSDFFace& msdfFallbackFace() {
    if (s_msdfFallbackBuilt) return s_msdfFallback;

    const int cellW = 128, cellH = 180, COLS = 16;
    const float atlasEmPx = 0.72f * (float)cellH;   // 129.6

    s_msdfFallback.atlasTex = FontAtlas::glyphTexture();
    s_msdfFallback.metricsTex = 0;
    s_msdfFallback.atlasInfo[0] = 2048.0f;
    s_msdfFallback.atlasInfo[1] = 1080.0f;
    s_msdfFallback.atlasInfo[2] = 2.0f;
    s_msdfFallback.atlasInfo[3] = atlasEmPx;
    s_msdfFallback.faceInfo[0] = 0.93f;
    s_msdfFallback.faceInfo[1] = -0.24f;
    s_msdfFallback.faceInfo[2] = 1.2f;
    s_msdfFallback.faceInfo[3] = -0.1f;
    s_msdfFallback.real = false;
    s_msdfFallback.id = "grid-fallback";

    std::vector<unsigned char> fontData;
    stbtt_fontinfo font;
    bool fontLoaded = loadSystemFont(fontData, font);
    float scale = fontLoaded ? stbtt_ScaleForPixelHeight(&font, atlasEmPx) : 0.0f;

    std::vector<unsigned char> buf((size_t)2048 * 256 * 4, 0);
    for (int i = 0; i <= 95; i++) {
        const int cp = (i == 95) ? 65533 : 32 + i;
        const int cell = (cp == 65533) ? 95 : cp - 32;
        const int col = cell % COLS, row = cell / COLS;
        // Cell box in upright atlas px (grid row 0 = the TOP band => the GL
        // bottom row of cell row r is (5-r)*cellH)
        const float left = (float)(col * cellW);
        const float right = left + (float)cellW;
        const float bottom = (float)((5 - row) * cellH);
        const float top = bottom + (float)cellH;
        float advEm = 0.6f;
        if (fontLoaded) {
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font, (cp == 65533) ? '?' : cp, &adv, &lsb);
            if (adv > 0) advEm = (float)adv * scale / atlasEmPx;
        }
        // plane box in em: cell geometry (em = 0.72*cellH, baseline
        // 0.75*cellH from the cell top)
        encodeMetricTexels(buf, cp, left, bottom, right, top,
                           0.0f, -0.347f, 0.987f, 1.042f, advEm, true);
    }
    s_msdfFallback.metricsTex = uploadMetricsTexture(buf);
    s_msdfFallbackBuilt = true;
    printf("FontAtlas: synthesized SCFONT/1 grid fallback face ready\n");
    return s_msdfFallback;
}

// Load fonts.json (+ each face's sctext.weight) for `dir`, cached.
bool ensureIndex(const std::string& dir) {
    if (s_msdfIndex.dir == dir) return s_msdfIndex.ok;
    s_msdfIndex = MSDFIndex{};
    s_msdfIndex.dir = dir;
    nlohmann::json idx;
    if (!readJsonFile(dir + "/fonts.json", idx)) return false;
    if (!idx.contains("families") || !idx["families"].is_array() ||
        idx["families"].empty()) return false;
    s_msdfIndex.families = idx["families"];
    for (const auto& fam : s_msdfIndex.families) {
        if (!fam.contains("faces") || !fam["faces"].is_array()) continue;
        for (const auto& fid : fam["faces"]) {
            if (!fid.is_string()) continue;
            std::string id = fid.get<std::string>();
            if (s_msdfIndex.faceWeights.count(id)) continue;
            int w = 400;
            nlohmann::json fj;
            if (readJsonFile(dir + "/" + id + ".json", fj) &&
                fj.contains("sctext") && fj["sctext"].contains("weight") &&
                fj["sctext"]["weight"].is_number()) {
                w = fj["sctext"]["weight"].get<int>();
            } else {
                // trailing "-NNN" of the id as a last resort
                auto dash = id.find_last_of('-');
                if (dash != std::string::npos) {
                    int v = atoi(id.c_str() + dash + 1);
                    if (v > 0) w = v;
                }
            }
            s_msdfIndex.faceWeights[id] = w;
        }
    }
    s_msdfIndex.ok = true;
    return true;
}

// Face resolution (phase-D-design.md 1.2): families[index == fontFamily] ||
// families[0]; among its faces the weight nearest fontWeight (ties -> lower).
std::string resolveFaceId(int fontFamily, int fontWeight) {
    const nlohmann::json* fam = nullptr;
    for (const auto& f : s_msdfIndex.families) {
        if (f.contains("index") && f["index"].is_number_integer() &&
            f["index"].get<int>() == fontFamily) { fam = &f; break; }
    }
    if (!fam) fam = &s_msdfIndex.families[0];
    std::string best;
    int bestD = 1 << 30, bestW = 1 << 30;
    if (fam->contains("faces") && (*fam)["faces"].is_array()) {
        for (const auto& fid : (*fam)["faces"]) {
            if (!fid.is_string()) continue;
            std::string id = fid.get<std::string>();
            auto it = s_msdfIndex.faceWeights.find(id);
            int w = (it != s_msdfIndex.faceWeights.end()) ? it->second : 400;
            int d = std::abs(w - fontWeight);
            if (d < bestD || (d == bestD && w < bestW)) {
                best = id; bestD = d; bestW = w;
            }
        }
    }
    return best;
}

GLuint uploadFacePng(const std::string& path, bool nearest) {
    // stbi vertical flip is process-global state -- set it again immediately
    // before EACH load so the PNG bottom row lands at v = 0 (upright upload,
    // phase-D-design.md 1.5).
    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4); // force RGBA
    if (!data) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint prevAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    stbi_image_free(data);
    GLint filt = nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

bool loadRealFace(const std::string& dir, const std::string& id, MSDFFace& out) {
    nlohmann::json fj;
    const std::string jsonPath = dir + "/" + id + ".json";
    if (!readJsonFile(jsonPath, fj)) return false;
    if (!fj.contains("atlas") || !fj.contains("metrics")) return false;
    const auto& a = fj["atlas"];
    const auto& m = fj["metrics"];
    std::string atlasFile = id + ".msdf.png";
    std::string metricsFile = id + ".metrics.png";
    if (fj.contains("sctext")) {
        const auto& sc = fj["sctext"];
        if (sc.contains("atlas") && sc["atlas"].is_string())
            atlasFile = sc["atlas"].get<std::string>();
        if (sc.contains("metricsTexture") && sc["metricsTexture"].is_string())
            metricsFile = sc["metricsTexture"].get<std::string>();
    }
    GLuint atlasTex = uploadFacePng(dir + "/" + atlasFile, /*nearest=*/false);
    if (!atlasTex) return false;
    GLuint metricsTex = uploadFacePng(dir + "/" + metricsFile, /*nearest=*/true);
    if (!metricsTex) {
        glDeleteTextures(1, &atlasTex);
        return false;
    }
    out.atlasTex = atlasTex;
    out.metricsTex = metricsTex;
    out.atlasInfo[0] = a.value("width", 0.0f);
    out.atlasInfo[1] = a.value("height", 0.0f);
    out.atlasInfo[2] = a.value("distanceRange", 4.0f);
    out.atlasInfo[3] = a.value("size", 48.0f);
    out.faceInfo[0] = m.value("ascender", 0.93f);
    out.faceInfo[1] = m.value("descender", -0.24f);
    out.faceInfo[2] = m.value("lineHeight", 1.2f);
    out.faceInfo[3] = m.value("underlineY", -0.1f);
    out.real = true;
    out.id = id;
    printf("FontAtlas: loaded SCFONT/1 face '%s' from %s\n", id.c_str(), jsonPath.c_str());
    return true;
}

} // namespace

static void destroyMSDFFaces() {
    for (auto& kv : s_msdfFaces) {
        if (kv.second.atlasTex) glDeleteTextures(1, &kv.second.atlasTex);
        if (kv.second.metricsTex) glDeleteTextures(1, &kv.second.metricsTex);
    }
    s_msdfFaces.clear();
    // fallback atlasTex is s_glyphTexture (freed by destroy() itself)
    if (s_msdfFallback.metricsTex) glDeleteTextures(1, &s_msdfFallback.metricsTex);
    s_msdfFallback = MSDFFace{};
    s_msdfFallbackBuilt = false;
    s_msdfFallbackLogged = false;
    s_msdfIndex = MSDFIndex{};
}

void FontAtlas::setAssetDir(const std::string& fontsDir) {
    s_assetDir = fontsDir;
}

const MSDFFace& FontAtlas::msdfFace(int fontFamily, int fontWeight) {
    const std::string dir = resolveAssetDir();
    if (dir.empty() || !ensureIndex(dir)) {
        if (!s_msdfFallbackLogged) {
            fprintf(stderr, "FontAtlas: SCFONT/1 assets not found at '%s'; "
                    "using synthesized grid face\n",
                    dir.empty() ? "<unset>" : dir.c_str());
            s_msdfFallbackLogged = true;
        }
        return msdfFallbackFace();
    }
    const std::string id = resolveFaceId(fontFamily, fontWeight);
    if (id.empty()) {
        if (!s_msdfFallbackLogged) {
            fprintf(stderr, "FontAtlas: SCFONT/1 face resolution failed at '%s'; "
                    "using synthesized grid face\n", dir.c_str());
            s_msdfFallbackLogged = true;
        }
        return msdfFallbackFace();
    }
    auto it = s_msdfFaces.find(id);
    if (it != s_msdfFaces.end()) return it->second;

    MSDFFace face;
    if (!loadRealFace(dir, id, face)) {
        if (!s_msdfFallbackLogged) {
            fprintf(stderr, "FontAtlas: SCFONT/1 face '%s' failed to load from "
                    "'%s'; using synthesized grid face\n", id.c_str(), dir.c_str());
            s_msdfFallbackLogged = true;
        }
        return msdfFallbackFace();
    }
    return s_msdfFaces.emplace(id, face).first->second;
}
