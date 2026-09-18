#pragma once
// SCTEXT/1 glyph-buffer encoder (Phase C "Words on the Wall").
//
// Pure, GL-free: turns a TextRun (UTF-8 text + optional per-word timing /
// speaker annotations) into
//   * the RGBA8 texel stream uploaded as `textBuf_<input>` (2*cap texels,
//     height 1; glyph texel = cp lo/hi + wordIndex, timing texel = revealMs
//     lo/hi + speakerId, alpha ALWAYS 255),
//   * the legacy Shader-Claw shim codes (`<input>_0..` A-Z 0..25, space 26,
//     0-9 27..36, everything else 26, a-z folded) so unconverted shaders keep
//     working unchanged.
// Contract: .claude/caption-pipeline/phase-C-design.md section 1.
// OVERFLOW (head/tail) is applied to the decoded code-point list BEFORE word
// indices are assigned, so wordIndex restarts at 0 for the kept window.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CaptionSegment;

// Word annotation in CODE-POINT indices into TextRun::text ([start, end)).
struct TextRunWord {
    int start = 0, end = 0;
    int revealMs = -1;   // ms since utterance start; -1 = unknown
    int speakerId = -1;  // small host-side id; -1 = unknown
};

struct TextRun {
    std::string text;                // UTF-8
    std::vector<TextRunWord> words;  // optional; empty => indices from whitespace only
    int speakerId = -1;              // segment-level speaker; -1 = unknown
};

struct GlyphEncodeOptions {
    int cap = 12;       // glyph slots in the buffer (MAX_LENGTH clamped 1..1024)
    int shimCap = 12;   // legacy per-char uniforms (min(MAX_LENGTH, 64))
    enum Case { Preserve, Upper, Lower } caseMode = Preserve;  // ASCII-only fold
    bool overflowHead = false;  // false = keep the END of the text (legacy easel)
};

struct GlyphEncoded {
    int len = 0;                    // filled glyph slots (<= cap)
    std::vector<uint8_t> bytes;     // 2*cap*4 RGBA8 bytes, A == 255 everywhere
    std::vector<int> shimCodes;     // shimCap legacy codes (26 = space/pad)
    int shimLen = 0;                // min(total glyphs, shimCap)
};

namespace GlyphBuffer {

// UTF-8 -> code points. Invalid lead/continuation bytes decode to U+FFFD;
// an incomplete trailing sequence is dropped (typewriter substr safety);
// cp > 0xFFFF -> U+FFFD (the buffer carries 16 bits).
std::vector<uint32_t> decodeUtf8(const std::string& utf8);

// Legacy Shader-Claw alphabet: A-Z/a-z -> 0..25, '0'-'9' -> 27..36, else 26.
int legacyCharCode(uint32_t cp);

GlyphEncoded encodeTextRun(const TextRun& run, const GlyphEncodeOptions& opts);

// Build a TextRun for `shownText` (possibly a typewriter prefix of the
// segment text) aligning seg.words[] sequentially from the start: case-
// insensitive, punctuation-tolerant (alphanumerics only are compared).
// revealMs = round(word.s - anchor), anchor = seg.startMs > 0 ? seg.startMs
// : first word's s; clamped 0..65534; -1 when unknown. speakerToId maps
// "speaker_N"/labels to small ints (may be empty -> -1).
TextRun buildTextRun(const std::string& shownText, const CaptionSegment& seg,
                     const std::function<int(const std::string&)>& speakerToId);

} // namespace GlyphBuffer
