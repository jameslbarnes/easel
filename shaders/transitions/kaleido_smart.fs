/*{
  "DESCRIPTION": "Kaleidoscopic vortex transition: source A implodes through a mirrored swirl and reassembles as source B. 2001 stargate via Pioneer DJ.",
  "CATEGORIES": ["Transition"],
  "CREDIT": "Easel transitions",
  "INPUTS": [
    { "NAME": "from",        "TYPE": "image" },
    { "NAME": "to",          "TYPE": "image" },
    { "NAME": "progress",    "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0, "LABEL": "Progress" },
    { "NAME": "slices",      "TYPE": "float", "DEFAULT": 8.0, "MIN": 3.0, "MAX": 16.0, "LABEL": "Kaleido Slices" },
    { "NAME": "zoomAmount",  "TYPE": "float", "DEFAULT": 0.5, "MIN": 0.0, "MAX": 0.9, "LABEL": "Zoom" },
    { "NAME": "swirl",       "TYPE": "float", "DEFAULT": 1.5, "MIN": 0.0, "MAX": 6.2832, "LABEL": "Swirl" },
    { "NAME": "audioKick",   "TYPE": "float", "DEFAULT": 0.0, "MIN": 0.0, "MAX": 1.0, "LABEL": "Audio Kick" }
  ]
}*/

void main() {
    vec2 uv = isf_FragNormCoord;

    // Envelope: peaks at progress=0.5, returns to 0 at endpoints. Guarantees
    // identity sampling at progress=0 (full from) and progress=1 (full to).
    float s = sin(progress * 3.14159265);

    // Polar from screen center.
    vec2 p = uv - 0.5;
    float r = length(p);
    float th = atan(p.y, p.x) + swirl * s + audioBass * audioKick;

    // Mirror-fold theta into one wedge of size 2π/slices.
    // The fold is what makes the kaleido — each wedge mirrors its neighbours.
    float seg = 6.2832 / max(slices, 3.0);
    th = abs(mod(th, seg) - seg * 0.5);

    // Zoom: at peak progress, suck the radius inward.
    float r2 = r * (1.0 - s * zoomAmount);

    // Cartesian back from folded polar.
    vec2 uv2 = vec2(cos(th), sin(th)) * r2 + 0.5;

    // Mix between identity (uv) and kaleidoscoped (uv2) by envelope amount.
    vec2 sampleUV = mix(uv, uv2, s);

    vec4 a = IMG_NORM_PIXEL(from, clamp(sampleUV, 0.0, 1.0));
    vec4 b = IMG_NORM_PIXEL(to,   clamp(sampleUV, 0.0, 1.0));

    // Crossfade the two sources around progress=0.5 with a sharp ramp so the
    // handoff happens at peak distortion.
    float crossT = smoothstep(0.4, 0.6, progress);
    gl_FragColor = mix(a, b, crossT);
}
