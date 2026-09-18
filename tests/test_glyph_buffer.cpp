// SCTEXT/1 glyph-buffer encoder tests (Phase C).
#include <catch2/catch_test_macros.hpp>
#include "render/GlyphBuffer.h"
#include "speech/CaptionSegment.h"
#include <map>

using namespace GlyphBuffer;

namespace {
struct Slot { int cp, word, reveal, speaker; };
Slot slot(const GlyphEncoded& e, int i) {
    const uint8_t* g = &e.bytes[(size_t)(2 * i) * 4];
    const uint8_t* t = g + 4;
    return { g[0] | (g[1] << 8), g[2], t[0] | (t[1] << 8), t[2] };
}
// The pre-Phase-C ShaderSource::uploadUniforms mapping, verbatim.
int oldShaderSourceCode(char c) {
    int ch = 26;
    if (c >= 'A' && c <= 'Z') ch = c - 'A';
    else if (c >= 'a' && c <= 'z') ch = c - 'a';
    else if (c >= '0' && c <= '9') ch = 27 + (c - '0');
    else ch = 26;
    return ch;
}
GlyphEncodeOptions opts(int cap, int shimCap = -1) {
    GlyphEncodeOptions o;
    o.cap = cap;
    o.shimCap = shimCap < 0 ? cap : shimCap;
    return o;
}
}

TEST_CASE("decodeUtf8: ASCII, multibyte, emoji, incomplete tail", "[glyph]") {
    auto a = decodeUtf8("Hi 42!");
    REQUIRE(a == std::vector<uint32_t>{'H', 'i', ' ', '4', '2', '!'});
    auto e = decodeUtf8("caf\xC3\xA9");            // é = U+00E9
    REQUIRE(e.size() == 4);
    REQUIRE(e[3] == 0xE9);
    auto emoji = decodeUtf8("a\xF0\x9F\x98\x80z");  // U+1F600 -> FFFD
    REQUIRE(emoji == std::vector<uint32_t>{'a', 0xFFFD, 'z'});
    auto cut = decodeUtf8("ab\xC3");                 // incomplete trailing sequence dropped
    REQUIRE(cut == std::vector<uint32_t>{'a', 'b'});
    auto cut2 = decodeUtf8("ab\xE2\x82");
    REQUIRE(cut2 == std::vector<uint32_t>{'a', 'b'});
}

TEST_CASE("legacyCharCode matches the old ShaderSource mapping", "[glyph]") {
    for (int c = 0; c < 128; ++c) {
        REQUIRE(legacyCharCode((uint32_t)c) == oldShaderSourceCode((char)c));
    }
    REQUIRE(legacyCharCode(0xE9) == 26);
    REQUIRE(legacyCharCode(0xFFFD) == 26);
}

TEST_CASE("encodeTextRun: ASCII layout, alpha 255, padding, word indices", "[glyph]") {
    TextRun run; run.text = "Hi wall 42";
    auto e = encodeTextRun(run, opts(16));
    REQUIRE(e.len == 10);
    REQUIRE(e.bytes.size() == 16 * 2 * 4);
    for (size_t i = 3; i < e.bytes.size(); i += 4) REQUIRE(e.bytes[i] == 255);
    REQUIRE(slot(e, 0).cp == 'H');
    REQUIRE(slot(e, 1).cp == 'i');
    REQUIRE(slot(e, 0).word == 0);
    REQUIRE(slot(e, 1).word == 0);
    REQUIRE(slot(e, 2).word == 255);   // space
    REQUIRE(slot(e, 3).word == 1);     // w
    REQUIRE(slot(e, 7).word == 255);
    REQUIRE(slot(e, 8).word == 2);     // 4
    REQUIRE(slot(e, 9).word == 2);
    // revealMs unknown, speaker unknown
    REQUIRE(slot(e, 0).reveal == 65535);
    REQUIRE(slot(e, 0).speaker == 255);
    // padding slots: zero cp, A==255 (checked above)
    for (int i = 10; i < 16; ++i) {
        REQUIRE(slot(e, i).cp == 0);
        REQUIRE(slot(e, i).word == 0);
        REQUIRE(slot(e, i).reveal == 0);
    }
    // shim
    REQUIRE(e.shimLen == 10);
    REQUIRE((int)e.shimCodes.size() == 16);
    std::string s = "Hi wall 42";
    for (int i = 0; i < 16; ++i) {
        int expect = i < (int)s.size() ? oldShaderSourceCode(s[i]) : 26;
        REQUIRE(e.shimCodes[i] == expect);
    }
}

TEST_CASE("encodeTextRun: UTF-8 e-acute and emoji", "[glyph]") {
    TextRun run; run.text = "caf\xC3\xA9 \xF0\x9F\x98\x80";
    auto e = encodeTextRun(run, opts(8));
    REQUIRE(e.len == 6);
    REQUIRE(slot(e, 3).cp == 0xE9);
    REQUIRE(slot(e, 5).cp == 0xFFFD);
    REQUIRE(slot(e, 5).word == 1);
    REQUIRE(e.shimCodes[3] == 26);
    REQUIRE(e.shimCodes[5] == 26);
}

TEST_CASE("encodeTextRun: CASE upper / lower / preserve (shim always folds)", "[glyph]") {
    TextRun run; run.text = "aB9";
    auto o = opts(4);
    o.caseMode = GlyphEncodeOptions::Upper;
    auto up = encodeTextRun(run, o);
    REQUIRE(slot(up, 0).cp == 'A'); REQUIRE(slot(up, 1).cp == 'B'); REQUIRE(slot(up, 2).cp == '9');
    o.caseMode = GlyphEncodeOptions::Lower;
    auto lo = encodeTextRun(run, o);
    REQUIRE(slot(lo, 0).cp == 'a'); REQUIRE(slot(lo, 1).cp == 'b');
    o.caseMode = GlyphEncodeOptions::Preserve;
    auto pr = encodeTextRun(run, o);
    REQUIRE(slot(pr, 0).cp == 'a'); REQUIRE(slot(pr, 1).cp == 'B');
    for (auto* e : { &up, &lo, &pr }) {
        REQUIRE(e->shimCodes[0] == 0);
        REQUIRE(e->shimCodes[1] == 1);
        REQUIRE(e->shimCodes[2] == 27 + 9);
    }
}

TEST_CASE("encodeTextRun: OVERFLOW head vs tail at cap, word indices restart", "[glyph]") {
    TextRun run; run.text = "one two three";   // 13 glyphs
    auto o = opts(5, 4);
    o.overflowHead = false;  // tail
    auto tail = encodeTextRun(run, o);
    REQUIRE(tail.len == 5);
    REQUIRE(slot(tail, 0).cp == 't');
    REQUIRE(slot(tail, 4).cp == 'e');
    REQUIRE(slot(tail, 0).word == 0);  // indices restart for the kept window
    REQUIRE(tail.shimLen == 4);
    REQUIRE(tail.shimCodes[0] == oldShaderSourceCode('h'));
    REQUIRE(tail.shimCodes[3] == oldShaderSourceCode('e'));
    o.overflowHead = true;
    auto head = encodeTextRun(run, o);
    REQUIRE(head.len == 5);
    REQUIRE(slot(head, 0).cp == 'o');
    REQUIRE(slot(head, 3).cp == ' ');
    REQUIRE(slot(head, 3).word == 255);
    REQUIRE(slot(head, 4).cp == 't');
    REQUIRE(slot(head, 4).word == 1);
    REQUIRE(head.shimCodes[0] == oldShaderSourceCode('o'));
    REQUIRE(head.shimCodes[3] == 26);
    // exact cap: no truncation
    auto o13 = opts(13);
    auto ex = encodeTextRun(run, o13);
    REQUIRE(ex.len == 13);
    REQUIRE(ex.shimLen == 13);
    // shimLen <= shimCap even when len > shimCap
    auto o2 = opts(64, 8);
    auto big = encodeTextRun(run, o2);
    REQUIRE(big.len == 13);
    REQUIRE(big.shimLen == 8);
    REQUIRE((int)big.shimCodes.size() == 8);
}

TEST_CASE("encodeTextRun: revealMs + speaker sentinels and clamping", "[glyph]") {
    TextRun run; run.text = "ab cd";
    run.speakerId = 3;
    run.words.push_back({0, 2, 120, -1});
    run.words.push_back({3, 5, 70000, 7});
    auto e = encodeTextRun(run, opts(8));
    REQUIRE(slot(e, 0).reveal == 120);
    REQUIRE(slot(e, 1).reveal == 120);
    REQUIRE(slot(e, 0).speaker == 3);       // segment speaker
    REQUIRE(slot(e, 2).reveal == 65535);    // space: not covered
    REQUIRE(slot(e, 3).reveal == 65534);    // clamped
    REQUIRE(slot(e, 3).speaker == 7);       // word speaker overrides
    TextRun none; none.text = "x";
    auto n = encodeTextRun(none, opts(2));
    REQUIRE(slot(n, 0).reveal == 65535);
    REQUIRE(slot(n, 0).speaker == 255);
}

TEST_CASE("buildTextRun: alignment with punctuation, case, partial coverage", "[glyph]") {
    CaptionSegment seg;
    seg.text = "Hello, wall 42!";
    seg.startMs = 1000;
    seg.speaker = "speaker_1";
    seg.words = {
        { "hello", 1000, 1300, 0.9f, "" },
        { "WALL",  1500, 1800, 0.9f, "speaker_2" },
    };
    std::map<std::string, int> ids;
    auto toId = [&](const std::string& s) {
        auto it = ids.find(s);
        if (it != ids.end()) return it->second;
        int id = (int)ids.size();
        ids[s] = id;
        return id;
    };
    TextRun run = buildTextRun("Hello, wall 42!", seg, toId);
    REQUIRE(run.text == "Hello, wall 42!");
    REQUIRE(run.speakerId == 0);
    REQUIRE(run.words.size() == 2);
    REQUIRE(run.words[0].start == 0);
    REQUIRE(run.words[0].end == 5);       // "Hello" (comma excluded)
    REQUIRE(run.words[0].revealMs == 0);
    REQUIRE(run.words[0].speakerId == 0);
    REQUIRE(run.words[1].start == 7);
    REQUIRE(run.words[1].end == 11);      // "wall"
    REQUIRE(run.words[1].revealMs == 500);
    REQUIRE(run.words[1].speakerId == 1);

    auto e = encodeTextRun(run, opts(16));
    REQUIRE(slot(e, 0).reveal == 0);
    REQUIRE(slot(e, 5).reveal == 65535);  // ','
    REQUIRE(slot(e, 7).reveal == 500);
    REQUIRE(slot(e, 12).reveal == 65535); // "42" has no word entry
    REQUIRE(slot(e, 12).speaker == 0);    // segment speaker still applies
    REQUIRE(slot(e, 7).speaker == 1);

    // Typewriter prefix: shown text ends mid-word -> partial word still timed.
    TextRun part = buildTextRun("Hello, wa", seg, toId);
    REQUIRE(part.words.size() == 2);
    REQUIRE(part.words[1].start == 7);
    REQUIRE(part.words[1].end == 9);

    // Mismatch stops alignment; earlier words keep their timing.
    TextRun mis = buildTextRun("Hello, world", seg, toId);
    REQUIRE(mis.words.size() == 1);

    // No startMs: anchor = first word's s.
    CaptionSegment seg2 = seg;
    seg2.startMs = 0;
    TextRun r2 = buildTextRun("hello wall", seg2, toId);
    REQUIRE(r2.words.size() == 2);
    REQUIRE(r2.words[0].revealMs == 0);
    REQUIRE(r2.words[1].revealMs == 500);

    // No timings at all -> -1.
    CaptionSegment seg3;
    seg3.words = { { "hello", 0, 0, -1.f, "" } };
    TextRun r3 = buildTextRun("hello", seg3, nullptr);
    REQUIRE(r3.words.size() == 1);
    REQUIRE(r3.words[0].revealMs == -1);
    REQUIRE(r3.speakerId == -1);
}
