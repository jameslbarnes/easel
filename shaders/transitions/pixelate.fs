/*{
    "DESCRIPTION": "Pixelate transition: both clips quantize into increasingly large blocks as progress approaches the midpoint, then resolve into clip B. Crossfade is driven by progress around the middle third so A and B blend where the pixelation peaks.",
    "CATEGORIES": ["Transition"],
    "CREDIT": "Easel transitions v1 — example template",
    "INPUTS": [
        { "NAME": "from",       "TYPE": "image" },
        { "NAME": "to",         "TYPE": "image" },
        { "NAME": "progress",   "TYPE": "float", "DEFAULT": 0.0,  "MIN": 0.0,  "MAX": 1.0,   "LABEL": "Progress" },
        { "NAME": "minBlocks",  "TYPE": "float", "DEFAULT": 8.0,  "MIN": 2.0,  "MAX": 64.0,  "LABEL": "Min Block Count (peak pixelation)" },
        { "NAME": "maxBlocks",  "TYPE": "float", "DEFAULT": 240.0,"MIN": 32.0, "MAX": 1024.0,"LABEL": "Max Block Count (near native)" },
        { "NAME": "crossStart", "TYPE": "float", "DEFAULT": 0.35, "MIN": 0.0,  "MAX": 0.5,   "LABEL": "A→B Crossfade Start" },
        { "NAME": "crossEnd",   "TYPE": "float", "DEFAULT": 0.65, "MIN": 0.5,  "MAX": 1.0,   "LABEL": "A→B Crossfade End" }
    ]
}*/

// Example transition shader. Interface contract for Easel timeline transitions:
//   - `from` (image):  the outgoing clip at the current timeline frame
//   - `to`   (image):  the incoming clip at the current timeline frame
//   - `progress` (float 0..1): where we are inside the transition window
// Any extra INPUTS become user-editable sliders in the transition inspector.
//
// Design:
//   - Pixel block count follows a smooth bump (1 at the edges, minBlocks at
//     progress=0.5) so the image dissolves into chunky pixels and back out.
//   - The A→B blend happens *inside* the pixelated region, so the pixels
//     themselves morph rather than one image appearing on top of the other.

void main() {
    vec2 uv = isf_FragNormCoord;
    float p = clamp(progress, 0.0, 1.0);

    // Bump profile: 0 at the endpoints, 1 at the midpoint. `sin(pi * p)` is
    // smooth and symmetric — any 0→1→0 curve works here.
    float peak = sin(p * 3.14159265);

    // Block count eases between "barely pixelated" at the ends and
    // "very pixelated" at the middle. Same for both images so they stay
    // spatially aligned while crossfading.
    float blocks = mix(maxBlocks, minBlocks, peak);
    blocks = max(blocks, 1.0);
    vec2 quant = (floor(uv * blocks) + 0.5) / blocks;

    vec4 ca = IMG_NORM_PIXEL(from, quant);
    vec4 cb = IMG_NORM_PIXEL(to,   quant);

    // Crossfade A→B within a user-controlled window so the swap lands where
    // the pixelation is thickest (default 0.35 → 0.65).
    float mixT = smoothstep(crossStart, crossEnd, p);
    gl_FragColor = mix(ca, cb, mixT);
}
