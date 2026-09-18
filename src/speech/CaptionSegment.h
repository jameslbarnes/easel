#pragma once
#include <string>
#include <vector>

// Shared caption contract (Phase B "Words on the Wall"). One struct for every
// live-transcript producer Easel listens to:
//   - Cue  /sessions/:id/events  "transcript" (camelCase; optional utteranceId/
//     revision/speechFinal/words/startMs/endMs/lang/fullTranscript/live) and
//     "caption.segment" (same fields, flat).
//   - Etherea raw WS /ws/transcript "transcript" (snake_case: is_final,
//     speech_final, utterance_id, utterance_text, start_ms, end_ms, full).
// All new fields are optional on the wire, so legacy {type,text,isFinal}
// producers still parse (utteranceId empty, revision 0).

struct CaptionWord {
    std::string w;          // word text
    double s = 0, e = 0;    // wall-clock Unix ms (0 when unknown)
    float conf = -1.f;      // confidence 0..1, -1 when unknown
    std::string speaker;    // "speaker_N" or producer label; empty when unknown
};

struct CaptionSegment {
    std::string text;           // this result's transcript (chunk-level, NOT cumulative)
    std::string utteranceText;  // cumulative committed chunks + live tail (optional)
    std::string speaker;
    std::string utteranceId;    // spans chunks until speechFinal; empty for legacy producers
    std::string lang;
    std::string fullTranscript; // Cue: session transcript (present only after ingest)
    int revision = 0;           // >= 1 when producer supplies it; strictly increasing per utteranceId
    bool isFinal = false;       // Deepgram is_final (chunk committed)
    bool speechFinal = false;   // Deepgram speech_final (utterance ended)
    bool live = false;          // Cue: true for the Deepgram stream path
    double startMs = 0, endMs = 0;
    std::vector<CaptionWord> words;
};

// Parse a transcript / caption.segment JSON object into `out`. Accepts both
// camelCase (Cue) and snake_case (Etherea) keys. Returns false on JSON parse
// error or when no non-empty text|transcript field is present. Never throws.
// `out` is reset to defaults before filling.
bool parseCaptionSegmentJson(const std::string& json, CaptionSegment& out);
