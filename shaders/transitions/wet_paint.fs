/*{
    "DESCRIPTION": "Wet-paint A→B transition: B bleeds through A with swirling flow and displacement, like ink spreading in water.",
    "CATEGORIES": ["Transition"],
    "CREDIT": "Easel transitions v1",
    "INPUTS": [
        { "NAME": "from",       "TYPE": "image" },
        { "NAME": "to",         "TYPE": "image" },
        { "NAME": "progress",   "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0, "LABEL": "Progress" },
        { "NAME": "swirl",      "TYPE": "float", "DEFAULT": 2.5, "MIN": 0.0, "MAX": 6.0,  "LABEL": "Swirl Amount" },
        { "NAME": "bleed",      "TYPE": "float", "DEFAULT": 0.08, "MIN": 0.0, "MAX": 0.25, "LABEL": "Bleed Distance" },
        { "NAME": "sharpness",  "TYPE": "float", "DEFAULT": 8.0, "MIN": 1.0, "MAX": 20.0, "LABEL": "Edge Sharpness" }
    ]
}*/

// Flow-noise: low-frequency 2D field that distorts UVs for the "wet" look
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i),             hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0,1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
vec2 flow(vec2 p) {
    float a = vnoise(p)         * 6.2832;
    float b = vnoise(p * 1.9 + 3.7) * 6.2832;
    return vec2(cos(a) + cos(b) * 0.5, sin(a) + sin(b) * 0.5);
}

void main() {
    vec2 uv = isf_FragNormCoord;
    // Swirl warp ramps in and out so UVs come back for a clean landing.
    float warpAmt = sin(progress * 3.14159) * bleed;
    vec2 warp = flow(uv * 2.0 + progress * swirl) * warpAmt;

    vec4 ca = IMG_NORM_PIXEL(from, uv + warp * 0.5);
    vec4 cb = IMG_NORM_PIXEL(to,   uv - warp * 0.5);

    // Wet mask: low-frequency noise grows with progress; B bleeds through where mask is high.
    float n = vnoise(uv * 3.0 + progress * 1.5) * 0.65
            + vnoise(uv * 7.0 + progress * 2.0) * 0.35;
    float mask = clamp((progress * 1.4 - 0.2 + (n - 0.5) * 0.8) * sharpness, 0.0, 1.0);

    gl_FragColor = mix(ca, cb, mask);
}
