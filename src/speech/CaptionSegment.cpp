#include "speech/CaptionSegment.h"

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

// First present key among `keys` (in order) or nullptr.
const json* pick(const json& obj, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = obj.find(k);
        if (it != obj.end() && !it->is_null()) return &*it;
    }
    return nullptr;
}

std::string asString(const json* v, const std::string& def = "") {
    if (!v) return def;
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<long long>());
    if (v->is_number_float()) return std::to_string(v->get<double>());
    if (v->is_boolean()) return v->get<bool>() ? "true" : "false";
    return def;
}

bool asBool(const json* v, bool def = false) {
    if (!v) return def;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number()) return v->get<double>() != 0.0;
    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        return s == "true" || s == "1";
    }
    return def;
}

double asDouble(const json* v, double def = 0.0) {
    if (!v) return def;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        try { return std::stod(v->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

int asInt(const json* v, int def = 0) {
    if (!v) return def;
    if (v->is_number_integer()) return (int)v->get<long long>();
    if (v->is_number_float()) return (int)v->get<double>();
    if (v->is_string()) {
        try { return std::stoi(v->get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

// Speaker may arrive as a string ("speaker_0", "Speaker") or as a number
// (Deepgram word-level diarization) => "speaker_N".
std::string asSpeaker(const json* v) {
    if (!v) return "";
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return "speaker_" + std::to_string(v->get<long long>());
    if (v->is_number_float()) return "speaker_" + std::to_string((long long)v->get<double>());
    return "";
}

} // namespace

bool parseCaptionSegmentJson(const std::string& jsonText, CaptionSegment& out) {
    out = CaptionSegment{};
    json root;
    try {
        root = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    } catch (...) {
        return false;
    }
    if (root.is_discarded() || !root.is_object()) return false;

    try {
        const json* textV = pick(root, {"text", "transcript"});
        if (!textV || !textV->is_string()) return false;
        out.text = textV->get<std::string>();
        if (out.text.empty()) return false;

        out.utteranceText  = asString(pick(root, {"utteranceText", "utterance_text"}));
        out.speaker        = asSpeaker(pick(root, {"speaker"}));
        out.utteranceId    = asString(pick(root, {"utteranceId", "utterance_id"}));
        out.lang           = asString(pick(root, {"lang", "language"}));
        out.fullTranscript = asString(pick(root, {"fullTranscript", "full_transcript", "full"}));
        out.revision       = asInt(pick(root, {"revision"}));
        out.isFinal        = asBool(pick(root, {"isFinal", "is_final"}));
        out.speechFinal    = asBool(pick(root, {"speechFinal", "speech_final"}));
        out.live           = asBool(pick(root, {"live"}));
        out.startMs        = asDouble(pick(root, {"startMs", "start_ms"}));
        out.endMs          = asDouble(pick(root, {"endMs", "end_ms"}));

        const json* wordsV = pick(root, {"words"});
        if (wordsV && wordsV->is_array()) {
            out.words.reserve(wordsV->size());
            for (const auto& wv : *wordsV) {
                if (!wv.is_object()) continue;
                CaptionWord cw;
                cw.w = asString(pick(wv, {"w", "text", "word", "punctuated_word"}));
                if (cw.w.empty()) continue;
                cw.s = asDouble(pick(wv, {"s", "start"}));
                cw.e = asDouble(pick(wv, {"e", "end"}));
                const json* confV = pick(wv, {"conf", "confidence"});
                cw.conf = confV ? (float)asDouble(confV, -1.0) : -1.f;
                cw.speaker = asSpeaker(pick(wv, {"speaker"}));
                out.words.push_back(std::move(cw));
            }
        }
    } catch (...) {
        // Defensive: any unexpected type conversion error => treat as unparsable.
        out = CaptionSegment{};
        return false;
    }
    return true;
}
