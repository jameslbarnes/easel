#include "render/GlyphBuffer.h"
#include "speech/CaptionSegment.h"
#include <algorithm>
#include <cmath>

namespace GlyphBuffer {

std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80)            { cp = c;         extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; } // stray continuation / invalid lead
        if (i + extra >= n) break; // incomplete trailing sequence -> drop it
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += extra + 1;
        if (cp > 0xFFFF) cp = 0xFFFD;
        out.push_back(cp);
    }
    return out;
}

int legacyCharCode(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return (int)(cp - 'A');
    if (cp >= 'a' && cp <= 'z') return (int)(cp - 'a');
    if (cp >= '0' && cp <= '9') return 27 + (int)(cp - '0');
    return 26;
}

static bool isSpaceCp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0xA0;
}

GlyphEncoded encodeTextRun(const TextRun& run, const GlyphEncodeOptions& opts) {
    GlyphEncoded enc;
    int cap = std::max(1, std::min(opts.cap, 1024));
    int shimCap = std::max(0, opts.shimCap);

    std::vector<uint32_t> cps = decodeUtf8(run.text);
    const int total = (int)cps.size();

    // Per-glyph annotations (code-point space) from the run's words.
    std::vector<int> reveal(total, -1);
    std::vector<int> speaker(total, run.speakerId);
    for (const auto& w : run.words) {
        int a = std::max(0, w.start), b = std::min(total, w.end);
        for (int i = a; i < b; ++i) {
            reveal[i] = w.revealMs;
            if (w.speakerId >= 0) speaker[i] = w.speakerId;
        }
    }

    // ---- shim (legacy alphabet, own overflow window, case-folded) ----
    enc.shimCodes.assign(shimCap, 26);
    int shimStart = 0, shimLen = total;
    if (total > shimCap) {
        shimLen = shimCap;
        shimStart = opts.overflowHead ? 0 : total - shimCap;
    }
    for (int i = 0; i < shimLen; ++i) enc.shimCodes[i] = legacyCharCode(cps[shimStart + i]);
    enc.shimLen = shimLen;

    // ---- buffer: case fold, then OVERFLOW window, then word indices ----
    if (opts.caseMode == GlyphEncodeOptions::Upper) {
        for (auto& cp : cps) if (cp >= 'a' && cp <= 'z') cp -= 32;
    } else if (opts.caseMode == GlyphEncodeOptions::Lower) {
        for (auto& cp : cps) if (cp >= 'A' && cp <= 'Z') cp += 32;
    }
    int start = 0, len = total;
    if (total > cap) {
        len = cap;
        start = opts.overflowHead ? 0 : total - cap;
    }
    enc.len = len;

    enc.bytes.assign((size_t)cap * 2 * 4, 0);
    for (int i = 0; i < cap; ++i) {
        uint8_t* g = &enc.bytes[(size_t)(2 * i) * 4];
        uint8_t* t = g + 4;
        g[3] = 255; t[3] = 255;
    }
    int wordIndex = -1;
    bool inWord = false;
    for (int i = 0; i < len; ++i) {
        uint32_t cp = cps[start + i];
        uint8_t* g = &enc.bytes[(size_t)(2 * i) * 4];
        uint8_t* t = g + 4;
        int wi;
        if (isSpaceCp(cp)) { inWord = false; wi = 255; }
        else {
            if (!inWord) { ++wordIndex; inWord = true; }
            wi = std::min(wordIndex, 254);
        }
        g[0] = (uint8_t)(cp & 255);
        g[1] = (uint8_t)((cp >> 8) & 255);
        g[2] = (uint8_t)wi;
        int ms = reveal[start + i];
        int msEnc = (ms < 0) ? 65535 : std::min(ms, 65534);
        t[0] = (uint8_t)(msEnc & 255);
        t[1] = (uint8_t)((msEnc >> 8) & 255);
        int sp = speaker[start + i];
        t[2] = (uint8_t)((sp < 0 || sp > 254) ? 255 : sp);
    }
    return enc;
}

// ---- word alignment ----

static bool isAlnumCp(uint32_t cp) {
    if (cp < 128) return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    return cp != 0xA0 && cp != 0xFFFD; // treat non-ASCII letters as word material
}
static uint32_t foldCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    return cp;
}

TextRun buildTextRun(const std::string& shownText, const CaptionSegment& seg,
                     const std::function<int(const std::string&)>& speakerToId) {
    TextRun run;
    run.text = shownText;
    auto toId = [&](const std::string& s) -> int {
        if (s.empty() || !speakerToId) return -1;
        int id = speakerToId(s);
        return (id < 0 || id > 254) ? -1 : id;
    };
    run.speakerId = toId(seg.speaker);
    if (seg.words.empty()) return run;

    std::vector<uint32_t> cps = decodeUtf8(shownText);
    const int total = (int)cps.size();

    double anchor = seg.startMs > 0 ? seg.startMs : 0.0;
    if (anchor <= 0) {
        for (const auto& w : seg.words) if (w.s > 0) { anchor = w.s; break; }
    }

    int pos = 0;
    for (const auto& w : seg.words) {
        std::vector<uint32_t> wcps;
        for (uint32_t c : decodeUtf8(w.w)) if (isAlnumCp(c)) wcps.push_back(foldCp(c));
        if (wcps.empty()) continue;
        // skip leading non-alnum glyphs in the shown text
        while (pos < total && !isAlnumCp(cps[pos])) ++pos;
        if (pos >= total) break;
        int wstart = pos;
        size_t k = 0;
        bool mismatch = false;
        int last = pos;
        while (pos < total && k < wcps.size()) {
            uint32_t c = cps[pos];
            if (!isAlnumCp(c)) { ++pos; continue; }  // punctuation inside the word ("don't")
            if (foldCp(c) != wcps[k]) { mismatch = true; break; }
            ++k; last = pos; ++pos;
        }
        if (mismatch) break;
        if (k == 0) break;
        TextRunWord tw;
        tw.start = wstart;
        tw.end = last + 1;
        if (w.s > 0 && anchor > 0) {
            double ms = std::round(w.s - anchor);
            tw.revealMs = (int)std::max(0.0, std::min(65534.0, ms));
        } else {
            tw.revealMs = -1;
        }
        tw.speakerId = w.speaker.empty() ? run.speakerId : toId(w.speaker);
        run.words.push_back(tw);
        if (k < wcps.size()) break; // shown text ended mid-word (typewriter)
    }
    return run;
}

} // namespace GlyphBuffer
