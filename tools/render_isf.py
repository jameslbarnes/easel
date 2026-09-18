#!/usr/bin/env python3
"""
render_isf.py — minimum-viable offline ISF renderer for Easel auto-improve loop.

Renders an ISF .fs shader to a 2x2 contact-sheet PNG at multiple TIME values.
Mirrors src/sources/ShaderSource.cpp::translateFragment preamble so what you
see here matches what Easel renders (modulo audio/cue live data and IMG_*
texture content — those are stubbed).

Deps (install into /Users/lu/easel/.venv-eval):
    moderngl>=5.12, Pillow>=11, numpy>=2

CLI:
    render_isf.py <shader.fs> --out <out.png>
        [--res 1280x720] [--times 0,1.5,3.5,7] [--msg "..."]
        [--font-atlas <path>] [--single]

Honest caveats:
- No real audio FFT — `audioLevel/Bass/Mid/High` driven by a synthetic
  envelope per-frame, `audioFFT` is a 256x1 ramp texture.
- IMG_* image inputs are bound to a 1x1 black texture; IMG_SIZE_<name> = vec2(0)
  so shaders that gate on it fall back gracefully.
- Font atlas: procedural 37-cell ramp atlas (A-Z, space, 0-9) so font shaders
  don't go blank. Visually approximate, not the same as Easel's stb_truetype
  atlas — letters won't look identical but composition reads correctly.
- player[i].*/cue.*/data.* binds: the orchestrator script drives these by
  setting per-frame default values (energyA/B/C, bassPull, etc.) through the
  same uniform that BIND would target in Easel. See `inject_default_values`.
"""

from __future__ import annotations
import argparse
import json
import math
import os
import re
import sys
from pathlib import Path
from typing import Any, Optional

import moderngl
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ─── ISF parsing ──────────────────────────────────────────────────────────

def parse_isf(source: str) -> tuple[str, dict, str]:
    """Returns (body_without_header, isf_dict, original_header_str)."""
    start = source.find("/*")
    if start < 0:
        return source, {}, ""
    end = source.find("*/", start)
    if end < 0:
        return source, {}, ""
    json_start = source.find("{", start, end)
    if json_start < 0:
        return source[end+2:], {}, source[start:end+2]
    # Walk brace depth to find matching close brace within comment
    depth = 0
    json_end = -1
    for i in range(json_start, end):
        c = source[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                json_end = i
                break
    if json_end < 0:
        return source[end+2:], {}, source[start:end+2]
    try:
        isf = json.loads(source[json_start:json_end+1])
    except Exception as e:
        print(f"[warn] ISF JSON parse failed: {e}", file=sys.stderr)
        isf = {}
    return source[end+2:], isf, source[start:end+2]


# ─── Preamble (mirrors ShaderSource::translateFragment) ───────────────────

# Audio Feature Bus float names (parity with claw3 js/isf.js AUDIO_BUS_FLOATS /
# easel ShaderSource.cpp, which always declares them natively).
AUDIO_BUS_FLOATS = [
    "audioSub", "audioLowMid", "audioHighMid", "audioTreble", "audioPunch",
    "audioBeat", "audioBeatPhase", "audioBeatPulse", "audioBarPhase", "audioBPM", "audioTempo01",
    "audioBrightness", "audioSpread", "audioRolloff", "audioFlatness", "audioTexture",
    "audioFlux", "audioOnset", "audioOnsetRate", "audioTilt", "audioZCR",
    "audioValence", "audioArousal", "audioTension", "audioWarmth", "audioSoftness", "audioRoughness", "audioCharm",
    "audioEnergy", "audioEnergyVel", "audioEnergyAcc", "audioBuildup", "audioBuildupRate", "audioDrop",
    "audioNovelty", "audioSectionPhase", "audioSectionAge", "audioLayers", "audioDensity",
    "audioPalTemp", "audioPalSat", "audioDominantPitch", "audioMajorMinor", "audioHCDF",
    "audioBassHit", "audioMidHit", "audioHighHit", "audioBassTime", "audioMidTime", "audioHighTime",
    "audioBassPresence", "audioMidPresence", "audioHighPresence", "audioTime",
    "audioBPMConfidence", "audioPhase2", "audioPhase4", "audioPhase8", "audioPhase16",
    "audioOnBeat", "audioToggleOnBeat",
    "stemBass", "stemDrums", "stemMelody", "stemAir", "stemVocal",
    "stemBassHit", "stemDrumsHit", "stemMelodyHit", "stemAirHit", "stemVocalHit",
    "stemBassPresence", "stemDrumsPresence", "stemMelodyPresence", "stemAirPresence", "stemVocalPresence",
]
AUDIO_BUS_VECS = [
    ("vec2", "audioMood"), ("vec4", "audioPresence"), ("vec2", "audioFlow"),
    ("vec3", "audioPalShadow"), ("vec3", "audioPalMid"), ("vec3", "audioPalHigh"), ("vec3", "audioPalAccent"),
]

# SCFONT/1 uniform names — any of them in the body loads/binds the MSDF face
MSDF_UNIFORM_NAMES = ("glyphMSDFTex", "glyphMetricsTex", "glyphAtlasInfo", "glyphFaceInfo")


def build_preamble(isf_inputs: list, pass_buffers: list, body: str) -> str:
    """Generate the Easel-compatible GL3.3 preamble. Mirrors ShaderSource.cpp."""
    lines = []
    lines.append("#version 330 core")
    lines.append("out vec4 FragColor;")
    lines.append("in vec2 isf_FragNormCoord;")
    lines.append("")
    # ISF builtins
    lines.append("uniform float TIME;")
    lines.append("uniform float TIMEDELTA;")
    lines.append("uniform vec2  RENDERSIZE;")
    lines.append("uniform int   PASSINDEX;")
    lines.append("uniform int   FRAMEINDEX;")
    lines.append("uniform vec2  mousePos;")
    lines.append("uniform vec2  mouseDelta;")
    lines.append("uniform float pinchHold;")
    lines.append("uniform float msgAge;")
    lines.append("uniform float mouseDown;")
    lines.append("")
    # MediaPipe stubs
    lines.append("uniform sampler2D mpPoseLandmarks;")
    lines.append("uniform sampler2D mpFaceLandmarks;")
    lines.append("uniform sampler2D mpHandLandmarks;")
    lines.append("uniform sampler2D mpSegmentation;")
    lines.append("")
    # Image inputs
    image_inputs = [i for i in isf_inputs if i.get("TYPE") == "image"]
    for inp in image_inputs:
        n = inp["NAME"]
        lines.append(f"uniform sampler2D {n};")
        lines.append(f"uniform vec2 IMG_SIZE_{n};")
        lines.append(f"uniform bool _flip_{n};")
    for buf in pass_buffers:
        lines.append(f"uniform sampler2D {buf};")
    # font atlas (if referenced)
    if "fontAtlasTex" in body:
        lines.append("uniform sampler2D fontAtlasTex;")
    # SCTEXT/1 glyph atlas (16x6 printable-ASCII grid, unit 12) if referenced
    if "glyphAtlasTex" in body:
        lines.append("uniform sampler2D glyphAtlasTex;")
    # SCFONT/1 MSDF glyph atlas + metrics (Phase D, phase-D-design.md 1.4):
    # units 10 (atlas, LINEAR) / 11 (metrics, NEAREST), declared when referenced
    if "glyphMSDFTex" in body:
        lines.append("uniform sampler2D glyphMSDFTex;")
    if "glyphMetricsTex" in body:
        lines.append("uniform sampler2D glyphMetricsTex;")
    if "glyphAtlasInfo" in body:
        lines.append("uniform vec4 glyphAtlasInfo;")
    if "glyphFaceInfo" in body:
        lines.append("uniform vec4 glyphFaceInfo;")
    # audio FFT + reactivity
    lines.append("uniform sampler2D audioFFT;")
    lines.append("uniform float audioLevel;")
    lines.append("uniform float audioBass;")
    lines.append("uniform float audioMid;")
    lines.append("uniform float audioHigh;")
    # Audio Feature Bus (parity with ShaderSource.cpp, which always declares
    # these; here conditional like claw3 isf.js AUDIO_BUS_FLOATS so uniform
    # counts stay small). Values default to 0 unless render_frame sets them.
    bus_needed = [n for n in AUDIO_BUS_FLOATS if n in body]
    if re.search(r"\baudioKick\s*\(", body) and "audioBeatPulse" not in bus_needed:
        bus_needed.append("audioBeatPulse")
    if re.search(r"\baudioHit\s*\(", body) and "audioOnset" not in bus_needed:
        bus_needed.append("audioOnset")
    if re.search(r"\baudioPalette\s*\(", body) and "audioBeat" not in bus_needed:
        bus_needed.append("audioBeat")
    for n in bus_needed:
        lines.append(f"uniform float {n};")
    for t, n in AUDIO_BUS_VECS:
        if n in body or (n.startswith("audioPal") and re.search(r"\baudioPalette\s*\(", body)):
            lines.append(f"uniform {t} {n};")
    if "_voiceGlitch" in body:
        lines.append("uniform float _voiceGlitch;")
    lines.append("uniform float _voiceLevel;")
    lines.append("")
    # IMG_* macros
    lines.append("#define IMG_NORM_PIXEL(img, coord) texture(img, coord)")
    lines.append("#define IMG_THIS_NORM_PIXEL(img) texture(img, isf_FragNormCoord)")
    lines.append("#define IMG_THIS_PIXEL(img) texture(img, gl_FragCoord.xy / RENDERSIZE)")
    for inp in image_inputs:
        n = inp["NAME"]
        lines.append(f"vec2 _isf_img_size_{n}() {{ return IMG_SIZE_{n}; }}")
        lines.append(f"vec4 _isf_img_pixel_{n}(vec2 coord) {{ return texture({n}, coord / max(IMG_SIZE_{n}, vec2(1.0))); }}")
        lines.append(f"vec4 _flippedTex_{n}(vec2 uv) {{ if (_flip_{n}) uv.y = 1.0 - uv.y; return texture({n}, uv); }}")
        lines.append(f"vec4 _flippedTexPixel_{n}(vec2 px) {{ return _flippedTex_{n}(px / max(IMG_SIZE_{n}, vec2(1.0))); }}")
        lines.append(f"vec4 _flippedTexThis_{n}() {{ return _flippedTex_{n}(gl_FragCoord.xy / RENDERSIZE); }}")
        lines.append(f"vec4 _flippedTexThisNorm_{n}() {{ return _flippedTex_{n}(isf_FragNormCoord); }}")
    lines.append("#define IMG_SIZE(img) RENDERSIZE")
    lines.append("#define IMG_PIXEL(img, coord) texture(img, (coord) / RENDERSIZE)")
    lines.append("")
    # User INPUTS
    for inp in isf_inputs:
        t = inp.get("TYPE", "float")
        n = inp["NAME"]
        if t == "float":
            lines.append(f"uniform float {n};")
        elif t == "color":
            lines.append(f"uniform vec4 {n};")
        elif t in ("bool", "event"):
            lines.append(f"uniform bool {n};")
        elif t == "point2D":
            lines.append(f"uniform vec2 {n};")
        elif t == "long":
            lines.append(f"uniform int {n};")
        elif t == "text":
            # SCTEXT/1 (phase-C-design.md 1.2/1.4): `<n>_len` always; legacy
            # per-char ints only while the body references `<n>_<digits>`
            # (shim, cap min(MAX_LENGTH,64)); glyph-buffer sampler + `<n>_cap`
            # only when the body samples `textBuf_<n>`.
            max_len = inp.get("MAX_LENGTH", 12)
            if max_len <= 0:
                max_len = 12
            lines.append(f"uniform int {n}_len;")
            if text_input_uses_shim(n, body):
                for i in range(min(max_len, SHIM_CAP_MAX)):
                    lines.append(f"uniform int {n}_{i};")
            if f"textBuf_{n}" in body:
                lines.append(f"uniform sampler2D textBuf_{n};")
                lines.append(f"uniform float {n}_cap;")
    lines.append("")
    lines.append("#define gl_FragColor FragColor")
    lines.append("#define texture2D texture")
    lines.append("")
    # Audio helper functions (native parity) — injected only when referenced
    if re.search(r"\baudioSpectrum\s*\(", body):
        lines.append("float audioSpectrum(float f){ return texture(audioFFT, vec2(clamp(f,0.0,1.0),0.5)).r; }")
    if re.search(r"\baudioKick\s*\(", body):
        lines.append("float audioKick(){ return audioBeatPulse; }")
    if re.search(r"\baudioHit\s*\(", body):
        lines.append("float audioHit(){ return audioOnset; }")
    if re.search(r"\baudioBreath\s*\(", body):
        lines.append("float audioBreath(){ return pow(max(audioLevel,0.0),0.6); }")
    if re.search(r"\baudioAlive\s*\(", body):
        lines.append("float audioAlive(float rest,float drive,float amount){ return rest+drive*amount; }")
    if re.search(r"\baudioPalette\s*\(", body):
        lines.append("vec3 audioPalette(float t){ t=clamp(t,0.0,1.0); vec3 c=(t<0.5)?mix(audioPalShadow,audioPalMid,t*2.0):mix(audioPalMid,audioPalHigh,t*2.0-1.0); return mix(c,audioPalAccent,audioBeat*smoothstep(0.6,1.0,t)*0.6); }")
    if ("scText" in body or "scGlyph" in body or "scMSDF" in body
            or "sampleGlyphMSDF" in body):
        lines.append(SC_TEXT_V2)
        lines.append("")
    if "sampleGlyphMSDF" in body:
        lines.append(SC_TEXT_V2D)
        lines.append("")
    return "\n".join(lines) + "\n"


SHIM_CAP_MAX = 64
CAP_MAX = 1024


def text_input_uses_shim(name: str, body: str) -> bool:
    """True when the GLSL body still reads the legacy per-char uniforms."""
    return re.search(rf"\b{re.escape(name)}_[0-9]+\b", body) is not None


# Canonical helper block "sc_text v2" (phase-C-design.md 1.6 + phase-D-design.md
# 1.6). Easel's preamble uses `texture(` (the body's texture2D is #defined away
# above). Keep the function names/text identical across easel / claw3 / etherea.
SC_TEXT_V2 = """\
// ---- sc_text v2 : SCTEXT/1 glyph buffer + SCFONT/1 MSDF glyph atlas + metrics helpers (keep identical across easel / claw3 / etherea) ----
vec4 scTextTexel(sampler2D buf, float cap, int i, float sub) {
    return texture(buf, vec2((float(i) * 2.0 + sub + 0.5) / (cap * 2.0), 0.5));
}
int scTextByte(float v) { return int(floor(v * 255.0 + 0.5)); }
int scTextCode(sampler2D buf, float cap, int i) {         // Unicode code point; 0 = empty slot
    vec4 t = scTextTexel(buf, cap, i, 0.0);
    return scTextByte(t.r) + 256 * scTextByte(t.g);
}
int scTextWord(sampler2D buf, float cap, int i) {         // word index; -1 = none (whitespace)
    int w = scTextByte(scTextTexel(buf, cap, i, 0.0).b);
    return (w == 255) ? -1 : w;
}
float scTextRevealMs(sampler2D buf, float cap, int i) {   // ms since utterance start; -1.0 = unknown
    vec4 t = scTextTexel(buf, cap, i, 1.0);
    int ms = scTextByte(t.r) + 256 * scTextByte(t.g);
    return (ms == 65535) ? -1.0 : float(ms);
}
int scTextSpeaker(sampler2D buf, float cap, int i) {      // speaker id; -1 = unknown
    int s = scTextByte(scTextTexel(buf, cap, i, 1.0).b);
    return (s == 255) ? -1 : s;
}
int scGlyphCell(int cp) { return (cp >= 32 && cp <= 126) ? cp - 32 : 95; }
vec2 scGlyphUV(int cp, vec2 uv) {                         // uv glyph-local, y = 1 at glyph top
    float idx = float(scGlyphCell(cp));
    float col = mod(idx, 16.0);
    float row = floor(idx / 16.0);
    return vec2((col + uv.x) / 16.0, 1.0 - (row + 1.0 - uv.y) / 6.0);
}
float scGlyphSample(sampler2D atlas, int cp, vec2 uv) {   // coverage 0..1; 0 outside the cell / for space
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
    if (cp <= 32) return 0.0;
    return texture(atlas, scGlyphUV(cp, uv)).r;
}
float scU16(vec2 v) { return floor(v.x * 255.0 + 0.5) + 256.0 * floor(v.y * 255.0 + 0.5); }
float scS16em(vec2 v) { return (scU16(v) - 32768.0) / 4096.0; }          // em, fixed-point 1/4096
vec4 scGlyphMetricTexel(sampler2D mtx, int cp, int k) {                  // 8 texels per code point, page = cp/256
    int page = cp / 256; int lo = cp - page * 256;
    return texture(mtx, vec2((float(lo * 8 + k) + 0.5) / 2048.0, (float(page) + 0.5) / 256.0));
}
bool  scGlyphPresent(sampler2D mtx, int cp) { return scGlyphMetricTexel(mtx, cp, 4).b > 0.5; }
int   scGlyphResolve(sampler2D mtx, int cp) { return scGlyphPresent(mtx, cp) ? cp : 65533; }   // U+FFFD fallback
vec4  scGlyphAtlasRect(sampler2D mtx, int cp) {                           // atlas px (left, bottom, right, top), y up
    vec4 a = scGlyphMetricTexel(mtx, cp, 0), b = scGlyphMetricTexel(mtx, cp, 1);
    return vec4(scU16(a.rg), scU16(a.ba), scU16(b.rg), scU16(b.ba));
}
vec4  scGlyphPlane(sampler2D mtx, int cp) {                               // em box rel. to pen origin (left, bottom, right, top)
    vec4 a = scGlyphMetricTexel(mtx, cp, 2), b = scGlyphMetricTexel(mtx, cp, 3);
    return vec4(scS16em(a.rg), scS16em(a.ba), scS16em(b.rg), scS16em(b.ba));
}
float scGlyphAdvance(sampler2D mtx, int cp) { return scS16em(scGlyphMetricTexel(mtx, cp, 4).rg); }   // em
float scMSDFMedian(vec3 c) { return max(min(c.r, c.g), min(max(c.r, c.g), c.b)); }
// Analytic (no derivatives): screenPxRange = distanceRangePx * (glyph px on screen / glyph px in the atlas).
float scMSDFCoverage(sampler2D atlas, vec2 atlasUV, float screenPxRange) {
    float sd = scMSDFMedian(texture(atlas, atlasUV).rgb) - 0.5;
    return clamp(sd * max(screenPxRange, 1.0) + 0.5, 0.0, 1.0);
}
float scMSDFScreenPxRange(vec4 atlasInfo, float glyphScreenPx, float glyphAtlasPx) {
    return atlasInfo.z * glyphScreenPx / max(glyphAtlasPx, 1.0);
}
// Glyph-local uv over the plane box (x 0..1 left->right, y 0..1 bottom->top) -> coverage; 0 outside / absent / space.
float scGlyphMSDF(sampler2D atlas, sampler2D mtx, vec4 atlasInfo, int cp, vec2 uv, float screenPxRange) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
    vec4 r = scGlyphAtlasRect(mtx, cp);
    if (r.z <= r.x || r.w <= r.y) return 0.0;
    vec2 auv = (r.xy + uv * (r.zw - r.xy)) / atlasInfo.xy;
    return scMSDFCoverage(atlas, auv, screenPxRange);
}
// ---- end sc_text v2 ----"""

# Derivative-based variant, injected only when the body references
# sampleGlyphMSDF (GLSL 330 core has fwidth without any extension).
SC_TEXT_V2D = """\
// ---- sc_text v2d : derivative-based variant ----
float sampleGlyphMSDF(sampler2D atlas, vec2 atlasUV, float distanceRange, vec2 atlasSizePx) {
    vec2 unitRange = vec2(distanceRange) / atlasSizePx;
    vec2 screenTexSize = vec2(1.0) / fwidth(atlasUV);
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
    float sd = scMSDFMedian(texture(atlas, atlasUV).rgb) - 0.5;
    return clamp(sd * screenPxRange + 0.5, 0.0, 1.0);
}
// ---- end sc_text v2d ----"""


def translate_body(body: str, image_inputs: list) -> str:
    """Apply the same body-rewrites ShaderSource does."""
    # Strip #version
    body = re.sub(r"#version\s+\d+[^\n]*", "// (version stripped)", body)
    # Strip precision qualifiers
    body = re.sub(r"precision\s+(highp|mediump|lowp)\s+\w+\s*;", "// (precision stripped)", body)
    # varying -> in (fragment side)
    body = re.sub(r"\bvarying\b", "in", body)
    # texture2D handled by #define
    # Per-input flipped texture wrappers
    for inp in image_inputs:
        n = inp["NAME"]
        body = re.sub(rf"\btexture\s*\(\s*{n}\s*,", f"_flippedTex_{n}(", body)
        body = re.sub(rf"\bIMG_NORM_PIXEL\s*\(\s*{n}\s*,", f"_flippedTex_{n}(", body)
        body = re.sub(rf"\bIMG_PIXEL\s*\(\s*{n}\s*,", f"_flippedTexPixel_{n}(", body)
        body = re.sub(rf"\bIMG_THIS_PIXEL\s*\(\s*{n}\s*\)", f"_flippedTexThis_{n}()", body)
        body = re.sub(rf"\bIMG_THIS_NORM_PIXEL\s*\(\s*{n}\s*\)", f"_flippedTexThisNorm_{n}()", body)
    return body


# ─── Font atlas ───────────────────────────────────────────────────────────

ATLAS_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789"  # 37 cells


def build_font_atlas(cell_w: int = 64, cell_h: int = 96) -> np.ndarray:
    """Procedural 37-cell font atlas. Returns HxW uint8, R channel.
    Layout matches Easel's FontAtlas.cpp: 37 horizontal cells, single row."""
    atlas_w = cell_w * len(ATLAS_CHARS)
    atlas_h = cell_h
    img = Image.new("L", (atlas_w, atlas_h), 0)
    draw = ImageDraw.Draw(img)
    # Pick a font
    font_path = None
    for p in [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    ]:
        if os.path.exists(p):
            font_path = p
            break
    pixel_h = int(cell_h * 0.78)
    try:
        font = ImageFont.truetype(font_path, pixel_h) if font_path else ImageFont.load_default()
    except Exception:
        font = ImageFont.load_default()
    for i, ch in enumerate(ATLAS_CHARS):
        if ch == " ":
            continue
        # Center horizontally
        try:
            bbox = draw.textbbox((0, 0), ch, font=font)
            w = bbox[2] - bbox[0]
            x = i * cell_w + (cell_w - w) // 2 - bbox[0]
            y = int(cell_h * 0.10) - bbox[1]
        except Exception:
            x = i * cell_w + cell_w // 4
            y = cell_h // 8
        draw.text((x, y), ch, fill=255, font=font)
    # Easel's FontAtlas writes pixels with dstRow = cellH-1-baselineY-yoff-y, then
    # uploads directly. Effect: in the uploaded texture, baseline area sits at HIGH
    # row indices, which (since moderngl/glTexImage2D treats row 0 as v=0 bottom)
    # places baseline at HIGH v. Shaders sample with v∈[0,1] expecting top of letter
    # at v=1. To match: flipud our PIL render so PIL's "top of letter" ends up at
    # the high-row end of the byte buffer.
    arr = np.array(img, dtype=np.uint8)
    arr = np.flipud(arr).copy()
    return arr


def _pick_font(pixel_h: int):
    font_path = None
    for p in [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]:
        if os.path.exists(p):
            font_path = p
            break
    try:
        return ImageFont.truetype(font_path, pixel_h) if font_path else ImageFont.load_default()
    except Exception:
        return ImageFont.load_default()


def build_glyph_atlas(cell_w: int = 64, cell_h: int = 90) -> np.ndarray:
    """SCTEXT/1 glyph atlas (phase-C-design.md 1.5): 16 columns x 6 rows,
    cell idx = cp-32 for cp 32..126, cell 95 = '?' replacement. Returns HxW
    uint8 (R channel). SAME orientation rule as src/render/FontAtlas.cpp
    generateGlyphGrid(): the grid is drawn top-down in PIL (row 0 = cp 32..47
    at the top) and then flipud like build_font_atlas, so after glTexImage2D
    (row 0 = v 0 = bottom) row 0 occupies the TOP band v in [5/6, 1] and the
    glyphs are upright; shaders sample vec2((col+uv.x)/16, 1-(row+1-uv.y)/6)."""
    cols, rows = 16, 6
    atlas_w, atlas_h = cell_w * cols, cell_h * rows
    img = Image.new("L", (atlas_w, atlas_h), 0)
    draw = ImageDraw.Draw(img)
    font = _pick_font(int(cell_h * 0.72))
    baseline_from_top = int(cell_h * 0.75)  # baseline ~0.25*cellH above the cell bottom
    for idx in range(cols * rows):
        cp = ord("?") if idx == 95 else 32 + idx
        if cp == 32:
            continue
        ch = chr(cp)
        col, row = idx % cols, idx // cols
        x0, y0 = col * cell_w, row * cell_h
        try:
            # anchor "ls" = left/baseline so the baseline lands at y0+baseline_from_top
            bbox = draw.textbbox((0, 0), ch, font=font, anchor="ls")
            w = bbox[2] - bbox[0]
            x = x0 + (cell_w - w) // 2 - bbox[0]
            draw.text((x, y0 + baseline_from_top), ch, fill=255, font=font, anchor="ls")
        except Exception:
            bbox = draw.textbbox((0, 0), ch, font=font)
            w = bbox[2] - bbox[0]
            x = x0 + (cell_w - w) // 2 - bbox[0]
            draw.text((x, y0 + int(cell_h * 0.1) - bbox[1]), ch, fill=255, font=font)
    arr = np.array(img, dtype=np.uint8)
    arr = np.flipud(arr).copy()
    return arr


# ─── SCFONT/1 MSDF face (Phase D, phase-D-design.md 1.2-1.4, 1.7) ────────

def encode_glyph_texels(glyphs: list, present_cps=None) -> np.ndarray:
    """Port of shader-claw3 tools/fonts/metrics-codec.mjs encodeGlyphTexels.
    Returns uint8 (256, 2048, 4) TOP-DOWN: page p at array row 255-p, so after
    the caller's np.flipud (the upload convention shared with the atlases) GL
    row p counted from the bottom holds page p.
    k=0/1: atlas rect u16 px (L,B / R,T); k=2/3: plane box s16 fixed
    round(em*4096)+32768; k=4: (advance lo, hi, present, 255); k=5..7 zero."""
    out = np.zeros((256, 2048, 4), dtype=np.uint8)

    def u16(v):
        return max(0, min(65535, round(v)))

    def s16em(v):
        return max(0, min(65535, round(v * 4096) + 32768))

    for g in glyphs:
        cp = g.get("unicode")
        if cp is None or cp < 0 or cp > 0xFFFF:
            continue
        page, lo = cp >> 8, cp & 255
        row = 255 - page
        x0 = lo * 8
        ab = g.get("atlasBounds") or {}
        pb = g.get("planeBounds") or {}
        aL, aB = u16(ab.get("left", 0)), u16(ab.get("bottom", 0))
        aR, aT = u16(ab.get("right", 0)), u16(ab.get("top", 0))
        out[row, x0 + 0] = (aL & 255, aL >> 8, aB & 255, aB >> 8)
        out[row, x0 + 1] = (aR & 255, aR >> 8, aT & 255, aT >> 8)
        pL, pB = s16em(pb.get("left", 0)), s16em(pb.get("bottom", 0))
        pR, pT = s16em(pb.get("right", 0)), s16em(pb.get("top", 0))
        out[row, x0 + 2] = (pL & 255, pL >> 8, pB & 255, pB >> 8)
        out[row, x0 + 3] = (pR & 255, pR >> 8, pT & 255, pT >> 8)
        adv = s16em(g.get("advance", 0) or 0)
        present = 255 if (present_cps is None or cp in present_cps) else 0
        out[row, x0 + 4] = (adv & 255, adv >> 8, present, 255)
    return out


def load_msdf_face(assets_dir, font_family: float = 0, font_weight: float = 400):
    """Load an SCFONT/1 face from fonts.json + <id>.json + PNGs. Resolution
    rule (phase-D-design.md 1.2): families[index == round(fontFamily)] ||
    families[0]; nearest sctext.weight (ties -> lower). Returns a dict
    (atlas HxWx4/HxW uint8 flipud'd for upload, atlas_components, metrics
    (256,2048,4) flipud'd, atlas_info, face_info, id, real) or None."""
    try:
        base = Path(assets_dir)
        idx = json.loads((base / "fonts.json").read_text())
        fams = idx.get("families") or []
        fam = next((f for f in fams if f.get("index") == round(font_family)), None)
        if fam is None and fams:
            fam = fams[0]
        faces = (fam or {}).get("faces") or idx.get("faces") or []
        best = best_d = best_w = best_json = None
        for fid in faces:
            fj = json.loads((base / f"{fid}.json").read_text())
            w = fj.get("sctext", {}).get("weight", 400)
            d = abs(w - font_weight)
            if best is None or d < best_d or (d == best_d and w < best_w):
                best, best_d, best_w, best_json = fid, d, w, fj
        if best is None:
            return None
        fj = best_json
        sct = fj.get("sctext", {})
        atlas_img = Image.open(base / sct.get("atlas", best + ".msdf.png")).convert("RGBA")
        metrics_img = Image.open(base / sct.get("metricsTexture", best + ".metrics.png")).convert("RGBA")
        atlas_arr = np.flipud(np.array(atlas_img, dtype=np.uint8)).copy()
        metrics_arr = np.flipud(np.array(metrics_img, dtype=np.uint8)).copy()
        a, m = fj.get("atlas", {}), fj.get("metrics", {})
        atlas_info = (float(a.get("width", atlas_img.width)), float(a.get("height", atlas_img.height)),
                      float(a.get("distanceRange", 4)), float(a.get("size", 48)))
        face_info = (float(m.get("ascender", 0.93)), float(m.get("descender", -0.24)),
                     float(m.get("lineHeight", 1.2)), float(m.get("underlineY", -0.1)))
        print(f"[msdf] loaded SCFONT/1 face '{best}' from {base}")
        return dict(atlas=atlas_arr, atlas_components=4, metrics=metrics_arr,
                    atlas_info=atlas_info, face_info=face_info, id=best, real=True)
    except Exception as e:
        print(f"[msdf] SCFONT/1 assets not usable at {assets_dir}: {e}", file=sys.stderr)
        return None


def build_msdf_fallback(cell_w: int = 64, cell_h: int = 90):
    """Synthesized SCFONT/1 face from the 16x6 coverage grid (phase-D-design.md
    1.7): atlas = the grid (RRRR swizzle -> .rgb = coverage, px range 2 =
    'treat as coverage'); metrics texels for cp 32..126 + 65533 (cell 95)."""
    atlas_arr = build_glyph_atlas(cell_w, cell_h)  # HxW uint8, already flipud'd
    font_px = int(cell_h * 0.72)
    font = _pick_font(font_px)
    glyphs = []
    for cp in list(range(32, 127)) + [0xFFFD]:
        cell = 95 if cp == 0xFFFD else cp - 32
        col, row = cell % 16, cell // 16
        left = col * cell_w
        bottom = (5 - row) * cell_h
        ch = "?" if cp == 0xFFFD else chr(cp)
        adv = 0.6
        try:
            adv = font.getlength(ch) / font_px
        except Exception:
            pass
        if not adv or adv <= 0:
            adv = 0.6
        glyphs.append({
            "unicode": cp, "advance": adv,
            # plane box in em: cell geometry (em = 0.72*cellH, baseline
            # 0.75*cellH from the cell top; shared 32:45 cell aspect)
            "planeBounds": {"left": 0.0, "bottom": -0.347, "right": 0.987, "top": 1.042},
            "atlasBounds": {"left": left, "bottom": bottom,
                            "right": left + cell_w, "top": bottom + cell_h},
        })
    metrics = np.flipud(encode_glyph_texels(glyphs)).copy()
    print("[msdf] SCFONT/1 assets not found; using synthesized grid face", file=sys.stderr)
    return dict(atlas=atlas_arr, atlas_components=1, metrics=metrics,
                atlas_info=(float(cell_w * 16), float(cell_h * 6), 2.0, 0.72 * cell_h),
                face_info=(0.93, -0.24, 1.2, -0.1), id="grid-fallback", real=False)


def msdf_asset_dir_candidates(cli_dir: str, shader_path) -> list:
    """--font-assets DIR wins; else <shader dir>/../assets/fonts, then
    %USERPROFILE%/src/shader-claw3/assets/fonts."""
    if cli_dir:
        return [cli_dir]
    cands = [str(Path(shader_path).resolve().parent.parent / "assets" / "fonts")]
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME")
    if home:
        cands.append(str(Path(home) / "src" / "shader-claw3" / "assets" / "fonts"))
    return cands


# ─── Default-value bake (handles BIND defaults) ──────────────────────────

# Sentinel BIND prefixes we want to "wake up" so the shader doesn't
# look completely dead. The orchestrator can override per-frame; for
# v1 we drive them with a synthetic envelope.
PLAYER_BIND_RE = re.compile(r"player\[(\d+)\]\.(\w+)")


def default_for_input(inp: dict, time_value: float) -> Any:
    """Pick a value for an ISF input.
    - For player[i].energy/active/etc binds: drive a synthetic per-channel envelope so
      shaders that gate on energy actually show something.
    - For audio.* binds: synthetic audio envelope.
    - Otherwise: use DEFAULT.
    """
    bind = inp.get("BIND", "")
    t = inp.get("TYPE", "float")
    name = inp["NAME"]
    # audio.* drives
    if bind == "audio.bass":
        return 0.45 + 0.4 * (0.5 + 0.5 * math.sin(time_value * 1.7))
    if bind == "audio.mid":
        return 0.35 + 0.3 * (0.5 + 0.5 * math.sin(time_value * 2.3 + 1.0))
    if bind == "audio.high":
        return 0.25 + 0.3 * (0.5 + 0.5 * math.sin(time_value * 3.1 + 2.0))
    if bind == "audio.level":
        return 0.35 + 0.4 * (0.5 + 0.5 * math.sin(time_value * 1.1))
    if bind == "transport.beat":
        return (time_value * 2.0) % 1.0
    m = PLAYER_BIND_RE.match(bind)
    if m:
        idx = int(m.group(1))
        prop = m.group(2)
        if prop in ("energy", "level", "amp"):
            # Stagger players so different ones light up at different TIMEs.
            phase = idx * 1.3
            return 0.15 + 0.7 * max(0.0, math.sin(time_value * 0.9 + phase))
        if prop in ("active", "on", "voicing"):
            return 1.0 if math.sin(time_value * 0.6 + idx * 1.1) > -0.3 else 0.0
        return 0.5
    if bind.startswith("cue.") or bind.startswith("data."):
        # text bindings already handled via msg uniforms
        return None
    # Use ISF DEFAULT.
    if t == "float":
        d = inp.get("DEFAULT", 0.5)
        return float(d) if isinstance(d, (int, float, str)) else 0.5
    if t == "long":
        d = inp.get("DEFAULT", 0)
        return int(d) if isinstance(d, (int, float, str)) else 0
    if t in ("bool", "event"):
        d = inp.get("DEFAULT", False)
        if isinstance(d, bool):
            return d
        if isinstance(d, (int, float)):
            return bool(d)
        return False
    if t == "color":
        d = inp.get("DEFAULT", [1.0, 1.0, 1.0, 1.0])
        if not isinstance(d, list):
            d = [1.0, 1.0, 1.0, 1.0]
        while len(d) < 4:
            d.append(1.0)
        return tuple(float(x) for x in d[:4])
    if t == "point2D":
        d = inp.get("DEFAULT", [0.5, 0.5])
        if not isinstance(d, list):
            d = [0.5, 0.5]
        return (float(d[0]), float(d[1]))
    if t == "text":
        d = inp.get("DEFAULT", "")
        return str(d)
    return None


# ─── Text-input encoding (mirrors ShaderSource / GlyphBuffer) ─────────────

def _text_codepoints(text: str) -> list[int]:
    """One code-point list shared by the shim and the glyph buffer
    (cp > 0xFFFF -> 0xFFFD, like GlyphBuffer::decodeUtf8)."""
    return [ord(c) if ord(c) <= 0xFFFF else 0xFFFD for c in text]


def _overflow_window(cps: list[int], cap: int, overflow: str) -> list[int]:
    if len(cps) <= cap:
        return cps
    return cps[:cap] if overflow == "head" else cps[-cap:]


def legacy_char_code(cp: int) -> int:
    """Legacy Shader-Claw alphabet (mirrors GlyphBuffer::legacyCharCode)."""
    if ord("A") <= cp <= ord("Z"):
        return cp - ord("A")
    if ord("a") <= cp <= ord("z"):
        return cp - ord("a")
    if ord("0") <= cp <= ord("9"):
        return 27 + (cp - ord("0"))
    return 26


def encode_text(text: str, max_len: int, overflow: str = "tail") -> tuple[int, list[int]]:
    """Legacy shim codes `<n>_0..<n>_{shimCap-1}` + shim length. shimCap =
    min(MAX_LENGTH, 64); a-z folded; OVERFLOW default tail (legacy easel)."""
    if max_len <= 0:
        max_len = 12
    shim_cap = min(max_len, SHIM_CAP_MAX)
    cps = _overflow_window(_text_codepoints(text), shim_cap, overflow)
    codes = [legacy_char_code(cps[i]) if i < len(cps) else 26 for i in range(shim_cap)]
    return len(cps), codes


def encode_text_buffer(text: str, cap: int, overflow: str = "tail",
                       case: str = "preserve") -> tuple[int, bytes]:
    """SCTEXT/1 glyph buffer (phase-C-design.md 1.1): returns (len, bytes) with
    bytes = 2*cap RGBA8 texels. Glyph texel: R=cp&255, G=cp>>8, B=wordIndex
    (255 on whitespace), A=255. Timing texel: revealMs 65535 (unknown, no word
    timings in this offline tool), B=speaker 255 (unknown), A=255. Padding
    slots are zero with A=255. OVERFLOW is applied BEFORE word indices are
    assigned (they restart at 0 for the kept window)."""
    cap = max(1, min(int(cap), CAP_MAX))
    cps = _text_codepoints(text)
    if case == "upper":
        cps = [cp - 32 if ord("a") <= cp <= ord("z") else cp for cp in cps]
    elif case == "lower":
        cps = [cp + 32 if ord("A") <= cp <= ord("Z") else cp for cp in cps]
    cps = _overflow_window(cps, cap, overflow)
    out = bytearray(2 * cap * 4)
    for i in range(cap):
        out[(2 * i) * 4 + 3] = 255
        out[(2 * i + 1) * 4 + 3] = 255
    word_index, in_word = -1, False
    for i, cp in enumerate(cps):
        if cp in (0x20, 0x09, 0x0A, 0x0D, 0x0B, 0x0C, 0xA0):
            in_word, wi = False, 255
        else:
            if not in_word:
                word_index += 1
                in_word = True
            wi = min(word_index, 254)
        g = (2 * i) * 4
        out[g] = cp & 255
        out[g + 1] = (cp >> 8) & 255
        out[g + 2] = wi
        t = g + 4
        out[t] = 255      # revealMs 65535 = unknown
        out[t + 1] = 255
        out[t + 2] = 255  # speaker unknown
    return len(cps), bytes(out)


# ─── Main render ──────────────────────────────────────────────────────────

VERTEX_SRC = """
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aTexCoord;
out vec2 isf_FragNormCoord;
void main() {
    isf_FragNormCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
"""


def safe_set(prog, name, value):
    """Set a uniform if present in the linked program. moderngl raises on
    unknown uniforms via prog[name]; this guards."""
    try:
        prog[name].value = value
    except KeyError:
        pass
    except Exception as e:
        # type mismatches etc — non-fatal
        pass


def render_frame(ctx, prog, fbo, time_value: float, frame_index: int,
                 res: tuple[int, int], inputs: list, msg: str,
                 fft_tex, font_tex, img_tex, vao, glyph_tex=None, body: str = "",
                 msdf_face=None, msdf_atlas_tex=None, msdf_metrics_tex=None):
    fbo.use()
    ctx.clear(0.0, 0.0, 0.0, 1.0)
    # Builtins
    safe_set(prog, "TIME", float(time_value))
    safe_set(prog, "TIMEDELTA", 1.0 / 60.0)
    safe_set(prog, "RENDERSIZE", (float(res[0]), float(res[1])))
    safe_set(prog, "PASSINDEX", 0)
    safe_set(prog, "FRAMEINDEX", frame_index)
    safe_set(prog, "mousePos", (res[0] * 0.5, res[1] * 0.5))
    safe_set(prog, "mouseDelta", (0.0, 0.0))
    safe_set(prog, "pinchHold", 0.0)
    safe_set(prog, "mouseDown", 0.0)
    # msgAge: simulate "live, just spoken" — text already typed at time=0
    safe_set(prog, "msgAge", max(0.0, time_value))
    # Synthetic audio
    safe_set(prog, "audioLevel", 0.4 + 0.3 * math.sin(time_value * 1.2))
    safe_set(prog, "audioBass", 0.5 + 0.35 * math.sin(time_value * 0.9))
    safe_set(prog, "audioMid", 0.35 + 0.3 * math.sin(time_value * 2.0 + 1.0))
    safe_set(prog, "audioHigh", 0.25 + 0.25 * math.sin(time_value * 3.3 + 2.0))
    safe_set(prog, "_voiceLevel", 0.3)
    safe_set(prog, "_voiceGlitch", 0.0)
    # Synthetic Audio Feature Bus subset (declared conditionally in the
    # preamble; anything not set here reads 0.0)
    beat = max(0.0, math.sin(time_value * 4.0))
    safe_set(prog, "audioBeat", beat)
    safe_set(prog, "audioBeatPulse", beat ** 4)
    safe_set(prog, "audioOnset", max(0.0, math.sin(time_value * 5.3)) ** 8)
    safe_set(prog, "audioEnergy", 0.4 + 0.3 * math.sin(time_value * 0.7))
    # Bind textures
    fft_tex.use(location=2)
    safe_set(prog, "audioFFT", 2)
    if font_tex is not None:
        font_tex.use(location=3)
        safe_set(prog, "fontAtlasTex", 3)
    if glyph_tex is not None:
        glyph_tex.use(location=12)
        safe_set(prog, "glyphAtlasTex", 12)
    # SCFONT/1 MSDF face: atlas unit 10, metrics unit 11 (mirrors ShaderSource)
    if msdf_atlas_tex is not None:
        msdf_atlas_tex.use(location=10)
        safe_set(prog, "glyphMSDFTex", 10)
    if msdf_metrics_tex is not None:
        msdf_metrics_tex.use(location=11)
        safe_set(prog, "glyphMetricsTex", 11)
    if msdf_face is not None:
        safe_set(prog, "glyphAtlasInfo", tuple(msdf_face["atlas_info"]))
        safe_set(prog, "glyphFaceInfo", tuple(msdf_face["face_info"]))
    # MediaPipe stubs → unit 7 zero texture
    img_tex.use(location=7)
    for name in ("mpPoseLandmarks", "mpFaceLandmarks", "mpHandLandmarks", "mpSegmentation"):
        safe_set(prog, name, 7)

    # ISF user inputs
    text_inputs = []
    img_unit = 8
    for inp in inputs:
        t = inp.get("TYPE", "float")
        n = inp["NAME"]
        if t == "text":
            text_inputs.append(inp)
            continue
        if t == "image":
            img_tex.use(location=img_unit)
            safe_set(prog, n, img_unit)
            safe_set(prog, f"IMG_SIZE_{n}", (0.0, 0.0))
            safe_set(prog, f"_flip_{n}", False)
            img_unit += 1
            continue
        val = default_for_input(inp, time_value)
        if val is None:
            continue
        if t in ("float",):
            safe_set(prog, n, float(val))
        elif t == "long":
            safe_set(prog, n, int(val))
        elif t in ("bool", "event"):
            safe_set(prog, n, bool(val))
        elif t == "color":
            safe_set(prog, n, tuple(val))
        elif t == "point2D":
            safe_set(prog, n, tuple(val))

    # Text inputs — use msg for any text input (SCTEXT/1: the buffer keeps
    # case per the input's CASE key; the shim always folds a-z -> A-Z).
    text_bufs = []
    for k, inp in enumerate(text_inputs):
        n = inp["NAME"]
        max_len = inp.get("MAX_LENGTH", 12)
        if max_len <= 0:
            max_len = 12
        overflow = str(inp.get("OVERFLOW", "tail")).lower()
        case = str(inp.get("CASE", "preserve")).lower()
        cap = max(1, min(int(max_len), CAP_MAX))
        uses_shim = text_input_uses_shim(n, body) if body else True
        uses_buf = f"textBuf_{n}" in body
        length, buf_bytes = encode_text_buffer(msg, cap, overflow, case)
        shim_len, codes = encode_text(msg, max_len, overflow)
        safe_set(prog, f"{n}_len", shim_len if (uses_shim and not uses_buf) else length)
        if uses_shim:
            for i, c in enumerate(codes):
                safe_set(prog, f"{n}_{i}", int(c))
        if uses_buf and k < 3:
            tb = ctx.texture((2 * cap, 1), 4, buf_bytes)
            tb.filter = (moderngl.NEAREST, moderngl.NEAREST)
            tb.repeat_x = False
            tb.repeat_y = False
            tb.use(location=13 + k)
            safe_set(prog, f"textBuf_{n}", 13 + k)
            safe_set(prog, f"{n}_cap", float(cap))
            text_bufs.append(tb)

    # Draw
    vao.render(moderngl.TRIANGLE_STRIP, vertices=4)
    for tb in text_bufs:
        tb.release()

    # Read back
    data = fbo.read(components=3, alignment=1)
    arr = np.frombuffer(data, dtype=np.uint8).reshape(res[1], res[0], 3)
    # FBO is bottom-up
    arr = np.flipud(arr).copy()
    return arr


def assemble_contact_sheet(frames: list[tuple[float, np.ndarray]],
                           single: bool = False) -> Image.Image:
    if single or len(frames) == 1:
        t, a = frames[0]
        img = Image.fromarray(a)
        draw = ImageDraw.Draw(img)
        _label(draw, f"t={t:.2f}s", (12, 12))
        return img
    h, w = frames[0][1].shape[:2]
    n = len(frames)
    cols = 2
    rows = (n + cols - 1) // cols
    sheet = Image.new("RGB", (w * cols, h * rows), (0, 0, 0))
    for i, (t, a) in enumerate(frames):
        sub = Image.fromarray(a)
        x = (i % cols) * w
        y = (i // cols) * h
        sheet.paste(sub, (x, y))
        draw = ImageDraw.Draw(sheet)
        _label(draw, f"t={t:.2f}s", (x + 12, y + 12))
    return sheet


def _label(draw, text, xy):
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 20)
    except Exception:
        font = ImageFont.load_default()
    # Outline
    x, y = xy
    for dx, dy in [(-1,0),(1,0),(0,-1),(0,1)]:
        draw.text((x+dx, y+dy), text, fill=(0,0,0), font=font)
    draw.text(xy, text, fill=(255,255,255), font=font)


def parse_res(s: str) -> tuple[int, int]:
    a, b = s.lower().split("x")
    return int(a), int(b)


def parse_times(s: str) -> list[float]:
    return [float(x) for x in s.split(",")]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("shader")
    ap.add_argument("--out", required=True)
    ap.add_argument("--res", default="1280x720")
    ap.add_argument("--times", default="0,1.5,3.5,7")
    ap.add_argument("--msg", default="HELLO WORLD CONVERSATION VISUAL TEST")
    ap.add_argument("--font-atlas", default="")
    ap.add_argument("--font-assets", default="",
                    help="SCFONT/1 assets dir (fonts.json + faces); default "
                         "<shader dir>/../assets/fonts then "
                         "%%USERPROFILE%%/src/shader-claw3/assets/fonts")
    ap.add_argument("--single", action="store_true",
                    help="Single-frame mode (only first TIME).")
    args = ap.parse_args(argv)

    shader_path = Path(args.shader)
    if not shader_path.exists():
        print(f"error: shader not found: {shader_path}", file=sys.stderr)
        return 2
    src = shader_path.read_text()
    body, isf, _header = parse_isf(src)
    inputs = isf.get("INPUTS", []) if isinstance(isf, dict) else []
    pass_buffers = []
    if isinstance(isf, dict) and isinstance(isf.get("PASSES"), list):
        for p in isf["PASSES"]:
            if isinstance(p, dict) and p.get("TARGET"):
                pass_buffers.append(p["TARGET"])

    preamble = build_preamble(inputs, pass_buffers, body)
    body = translate_body(body, [i for i in inputs if i.get("TYPE") == "image"])
    frag_src = preamble + body

    res = parse_res(args.res)
    times = parse_times(args.times)
    if args.single:
        times = times[:1]

    ctx = moderngl.create_standalone_context(require=330)

    # Build program
    log_path = Path(args.out).with_suffix(".log")
    try:
        prog = ctx.program(vertex_shader=VERTEX_SRC, fragment_shader=frag_src)
    except moderngl.Error as e:
        # Write the full source + error log
        log_path.write_text(f"=== compile/link error ===\n{e}\n\n=== fragment source ===\n{frag_src}\n")
        print(f"error: GLSL compile/link failed; see {log_path}", file=sys.stderr)
        return 3

    # Fullscreen triangle strip (-1..1 quad)
    verts = np.array([
        # x,    y,    u,    v
        -1.0, -1.0, 0.0, 0.0,
         1.0, -1.0, 1.0, 0.0,
        -1.0,  1.0, 0.0, 1.0,
         1.0,  1.0, 1.0, 1.0,
    ], dtype="f4")
    vbo = ctx.buffer(verts.tobytes())
    try:
        vao = ctx.vertex_array(prog, [(vbo, "2f 2f", "aPos", "aTexCoord")])
    except KeyError:
        # isf_FragNormCoord unused by the fragment -> the driver strips
        # aTexCoord; bind positions only (pre-existing gap for shaders that
        # read gl_FragCoord exclusively, e.g. text_typewriter).
        vao = ctx.vertex_array(prog, [(vbo, "2f 2x4", "aPos")])

    # FBO
    tex = ctx.texture(res, 3)
    fbo = ctx.framebuffer(color_attachments=[tex])

    # Stub textures
    # audio FFT: 256x1 ramp
    fft_data = (np.linspace(0.2, 0.8, 256, dtype="f4") *
                (0.5 + 0.5*np.sin(np.linspace(0, 6*np.pi, 256)))).astype("f4")
    fft_tex = ctx.texture((256, 1), 1, fft_data.tobytes(), dtype="f4")
    fft_tex.repeat_x = False
    fft_tex.repeat_y = False
    # font atlas
    if args.font_atlas and os.path.exists(args.font_atlas):
        atlas_img = Image.open(args.font_atlas).convert("L")
        atlas_arr = np.array(atlas_img, dtype=np.uint8)
        atlas_arr = np.flipud(atlas_arr).copy()
    else:
        atlas_arr = build_font_atlas()
    font_tex = ctx.texture((atlas_arr.shape[1], atlas_arr.shape[0]), 1,
                           atlas_arr.tobytes())
    font_tex.repeat_x = False
    font_tex.repeat_y = False
    font_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
    # Make R-only texture appear as luminance in .r .g .b
    font_tex.swizzle = "RRRR"
    # 1x1 black image stub for IMG inputs and MediaPipe
    img_tex = ctx.texture((1, 1), 4, b"\x00\x00\x00\x00")
    # SCTEXT/1 glyph atlas (unit 12) when referenced
    glyph_tex = None
    if "glyphAtlasTex" in body:
        glyph_arr = build_glyph_atlas()
        glyph_tex = ctx.texture((glyph_arr.shape[1], glyph_arr.shape[0]), 1,
                                glyph_arr.tobytes())
        glyph_tex.repeat_x = False
        glyph_tex.repeat_y = False
        glyph_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
        glyph_tex.swizzle = "RRRR"
    # SCFONT/1 MSDF face (units 10/11) when any of the four names is referenced
    msdf_face = None
    msdf_atlas_tex = None
    msdf_metrics_tex = None
    if any(n in body for n in MSDF_UNIFORM_NAMES):
        fam, wgt = 0, 400
        for inp in inputs:
            if inp.get("NAME") == "fontFamily":
                try:
                    fam = int(inp.get("DEFAULT", 0) or 0)
                except (TypeError, ValueError):
                    fam = 0
            elif inp.get("NAME") == "fontWeight":
                try:
                    wgt = float(inp.get("DEFAULT", 400) or 400)
                except (TypeError, ValueError):
                    wgt = 400
        for d in msdf_asset_dir_candidates(args.font_assets, shader_path):
            msdf_face = load_msdf_face(d, fam, wgt)
            if msdf_face:
                break
        if msdf_face is None:
            msdf_face = build_msdf_fallback()
        arr = msdf_face["atlas"]
        comp = msdf_face["atlas_components"]
        msdf_atlas_tex = ctx.texture((arr.shape[1], arr.shape[0]), comp, arr.tobytes())
        msdf_atlas_tex.repeat_x = False
        msdf_atlas_tex.repeat_y = False
        msdf_atlas_tex.filter = (moderngl.LINEAR, moderngl.LINEAR)
        if comp == 1:
            msdf_atlas_tex.swizzle = "RRRR"
        marr = msdf_face["metrics"]
        msdf_metrics_tex = ctx.texture((marr.shape[1], marr.shape[0]), 4, marr.tobytes())
        msdf_metrics_tex.repeat_x = False
        msdf_metrics_tex.repeat_y = False
        msdf_metrics_tex.filter = (moderngl.NEAREST, moderngl.NEAREST)

    frames = []
    for i, t in enumerate(times):
        arr = render_frame(ctx, prog, fbo, t, i, res, inputs, args.msg,
                           fft_tex, font_tex, img_tex, vao, glyph_tex, body,
                           msdf_face, msdf_atlas_tex, msdf_metrics_tex)
        frames.append((t, arr))

    sheet = assemble_contact_sheet(frames, single=args.single)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    # Also save the assembled fragment source for debugging
    log_path.write_text(f"=== render ok ===\nshader={shader_path}\nout={out_path}\n"
                        f"res={res} times={times}\n"
                        f"=== fragment source ===\n{frag_src}\n")
    print(f"ok: wrote {out_path} (frames: {len(frames)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
