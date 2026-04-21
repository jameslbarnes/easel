/*{
    "DESCRIPTION": "Noise-modulated dissolve transition: pixels fade from A to B at different rates based on a stable noise map.",
    "CATEGORIES": ["Transition"],
    "CREDIT": "Easel transitions v1",
    "INPUTS": [
        { "NAME": "from",     "TYPE": "image" },
        { "NAME": "to",       "TYPE": "image" },
        { "NAME": "progress", "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0, "LABEL": "Progress" },
        { "NAME": "softness", "TYPE": "float", "DEFAULT": 0.15, "MIN": 0.0, "MAX": 0.5, "LABEL": "Edge Softness" },
        { "NAME": "scale",    "TYPE": "float", "DEFAULT": 3.0, "MIN": 0.5, "MAX": 12.0, "LABEL": "Noise Scale" }
    ]
}*/

// Stable value-noise (no audio coupling — deterministic per pixel)
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main() {
    vec2 uv = isf_FragNormCoord;
    vec4 ca = IMG_NORM_PIXEL(from, uv);
    vec4 cb = IMG_NORM_PIXEL(to, uv);

    float n = vnoise(uv * scale) * 0.7 + vnoise(uv * scale * 2.3) * 0.3;
    // Dissolve threshold: progress must exceed the per-pixel noise for B to show.
    // Remap so progress=0 → all A, progress=1 → all B, with soft edge of width `softness`.
    float edge = progress * (1.0 + softness) - softness * 0.5;
    float t = smoothstep(n - softness * 0.5, n + softness * 0.5, edge);

    gl_FragColor = mix(ca, cb, t);
}
