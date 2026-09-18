#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "speech/CaptionSegment.h"

TEST_CASE("Cue unified transcript shape (camelCase, words, fullTranscript, live)", "[caption][cue]") {
    const std::string json = R"({
        "type":"transcript","sessionId":"demo",
        "text":"fast live words","transcript":"fast live words",
        "isFinal":true,"speechFinal":false,"live":true,"provider":"deepgram",
        "timestamp":1755800001.456,"speaker":"speaker_0",
        "utteranceId":"utt-1","revision":3,"lang":"en",
        "startMs":1755800000123,"endMs":1755800001456,
        "utteranceText":"earlier chunk fast live words",
        "words":[{"w":"fast","s":1755800000123,"e":1755800000400,"conf":0.98,"speaker":"speaker_0"},
                 {"w":"live","s":1755800000400,"e":1755800000700,"conf":0.9}],
        "fullTranscript":"earlier chunk fast live words"
    })";
    CaptionSegment seg;
    REQUIRE(parseCaptionSegmentJson(json, seg));
    CHECK(seg.text == "fast live words");
    CHECK(seg.isFinal);
    CHECK_FALSE(seg.speechFinal);
    CHECK(seg.live);
    CHECK(seg.speaker == "speaker_0");
    CHECK(seg.utteranceId == "utt-1");
    CHECK(seg.revision == 3);
    CHECK(seg.lang == "en");
    CHECK(seg.startMs == Catch::Approx(1755800000123.0));
    CHECK(seg.endMs == Catch::Approx(1755800001456.0));
    CHECK(seg.utteranceText == "earlier chunk fast live words");
    CHECK(seg.fullTranscript == "earlier chunk fast live words");
    REQUIRE(seg.words.size() == 2);
    CHECK(seg.words[0].w == "fast");
    CHECK(seg.words[0].s == Catch::Approx(1755800000123.0));
    CHECK(seg.words[0].e == Catch::Approx(1755800000400.0));
    CHECK(seg.words[0].conf == Catch::Approx(0.98f));
    CHECK(seg.words[0].speaker == "speaker_0");
    CHECK(seg.words[1].speaker.empty());
}

TEST_CASE("Legacy Cue transcript shape", "[caption][cue][legacy]") {
    CaptionSegment seg;
    REQUIRE(parseCaptionSegmentJson(R"({"type":"transcript","text":"hello there","isFinal":true})", seg));
    CHECK(seg.text == "hello there");
    CHECK(seg.isFinal);
    CHECK_FALSE(seg.speechFinal);
    CHECK_FALSE(seg.live);
    CHECK(seg.speaker.empty());
    CHECK(seg.utteranceId.empty());
    CHECK(seg.utteranceText.empty());
    CHECK(seg.fullTranscript.empty());
    CHECK(seg.revision == 0);
    CHECK(seg.words.empty());

    SECTION("transcript alias is accepted when text is absent") {
        CaptionSegment s2;
        REQUIRE(parseCaptionSegmentJson(R"({"type":"transcript","transcript":"alias","isFinal":false})", s2));
        CHECK(s2.text == "alias");
        CHECK_FALSE(s2.isFinal);
    }
}

TEST_CASE("Etherea snake_case transcript shape", "[caption][etherea]") {
    const std::string json = R"({
        "type":"transcript","text":"snake case words","is_final":true,"speech_final":true,
        "full":"all the words so far snake case words","unprocessed":"snake case words",
        "utterance_id":"u-7","revision":2,"utterance_text":"snake case words",
        "start_ms":100.5,"end_ms":900,"lang":"en","speaker":"Speaker",
        "words":[{"w":"snake","s":100.5,"e":300,"conf":0.7,"speaker":"Speaker"}]
    })";
    CaptionSegment seg;
    REQUIRE(parseCaptionSegmentJson(json, seg));
    CHECK(seg.text == "snake case words");
    CHECK(seg.isFinal);
    CHECK(seg.speechFinal);
    CHECK(seg.fullTranscript == "all the words so far snake case words");
    CHECK(seg.utteranceId == "u-7");
    CHECK(seg.revision == 2);
    CHECK(seg.utteranceText == "snake case words");
    CHECK(seg.startMs == Catch::Approx(100.5));
    CHECK(seg.endMs == Catch::Approx(900.0));
    CHECK(seg.speaker == "Speaker");
    REQUIRE(seg.words.size() == 1);
    CHECK(seg.words[0].w == "snake");
    CHECK(seg.words[0].speaker == "Speaker");
}

TEST_CASE("caption.segment shape", "[caption][cue]") {
    const std::string json = R"({
        "type":"caption.segment","sessionId":"demo","outputId":"captions",
        "utteranceId":"uuid-9","revision":1,"text":"first","utteranceText":"first",
        "isFinal":false,"speechFinal":false,"provider":"deepgram","timestamp":1.0
    })";
    CaptionSegment seg;
    REQUIRE(parseCaptionSegmentJson(json, seg));
    CHECK(seg.text == "first");
    CHECK(seg.utteranceId == "uuid-9");
    CHECK(seg.revision == 1);
    CHECK_FALSE(seg.isFinal);
    CHECK_FALSE(seg.speechFinal);
    CHECK_FALSE(seg.live);
}

TEST_CASE("Invalid JSON / missing text returns false and never throws", "[caption][error]") {
    CaptionSegment seg;
    seg.text = "stale";
    CHECK_FALSE(parseCaptionSegmentJson("{not json", seg));
    CHECK(seg.text.empty());
    CHECK_FALSE(parseCaptionSegmentJson("", seg));
    CHECK_FALSE(parseCaptionSegmentJson("[1,2,3]", seg));
    CHECK_FALSE(parseCaptionSegmentJson(R"({"type":"transcript","isFinal":true})", seg));
    CHECK_FALSE(parseCaptionSegmentJson(R"({"type":"transcript","text":""})", seg));
    CHECK_FALSE(parseCaptionSegmentJson(R"({"type":"transcript","text":42})", seg));
}

TEST_CASE("Numeric word speaker maps to speaker_N", "[caption][words]") {
    const std::string json = R"({
        "type":"transcript","text":"two speakers","isFinal":true,"speaker":1,
        "words":[{"word":"two","start":0.1,"end":0.2,"confidence":0.5,"speaker":0},
                 {"text":"speakers","start":0.2,"end":0.4,"speaker":1},
                 {"nope":"skipped"}]
    })";
    CaptionSegment seg;
    REQUIRE(parseCaptionSegmentJson(json, seg));
    CHECK(seg.speaker == "speaker_1");
    REQUIRE(seg.words.size() == 2);
    CHECK(seg.words[0].w == "two");
    CHECK(seg.words[0].speaker == "speaker_0");
    CHECK(seg.words[0].conf == Catch::Approx(0.5f));
    CHECK(seg.words[1].w == "speakers");
    CHECK(seg.words[1].speaker == "speaker_1");
    CHECK(seg.words[1].conf == Catch::Approx(-1.f));
}
