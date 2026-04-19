/*{
    "DESCRIPTION": "Volumetric LED display simulator - audio-reactive 3D voxel grid, raymarched. Adapted from cytonomy/volumetric-led.",
    "CATEGORIES": ["Generator", "Audio Reactive"],
    "INPUTS": [
        { "NAME": "patternMode", "TYPE": "long", "DEFAULT": 0, "VALUES": [0, 1, 2], "LABELS": ["Splines", "Lightning", "Pulse"], "LABEL": "Pattern" },
        { "NAME": "palette",     "TYPE": "long", "DEFAULT": 0, "VALUES": [0, 1, 2, 3], "LABELS": ["Cyan/Magenta", "Fire", "Ocean", "Rainbow"], "LABEL": "Palette" },
        { "NAME": "gridN",       "TYPE": "float", "DEFAULT": 16.0, "MIN": 8.0,  "MAX": 32.0, "LABEL": "Grid Size" },
        { "NAME": "zoom",        "TYPE": "float", "DEFAULT": 1.0,  "MIN": 0.5,  "MAX": 2.5,  "LABEL": "Zoom" },
        { "NAME": "rotSpeed",    "TYPE": "float", "DEFAULT": 0.15, "MIN": 0.0,  "MAX": 1.0,  "LABEL": "Rotation Speed" },
        { "NAME": "tilt",        "TYPE": "float", "DEFAULT": 0.3,  "MIN": -0.8, "MAX": 0.8,  "LABEL": "Tilt" },
        { "NAME": "audioGain",   "TYPE": "float", "DEFAULT": 1.0,  "MIN": 0.0,  "MAX": 3.0,  "LABEL": "Audio Sensitivity" },
        { "NAME": "dotSharp",    "TYPE": "float", "DEFAULT": 28.0, "MIN": 8.0,  "MAX": 80.0, "LABEL": "LED Sharpness" },
        { "NAME": "bloom",       "TYPE": "float", "DEFAULT": 1.0,  "MIN": 0.0,  "MAX": 2.5,  "LABEL": "Bloom" },
        { "NAME": "exposure",    "TYPE": "float", "DEFAULT": 1.0,  "MIN": 0.2,  "MAX": 3.0,  "LABEL": "Exposure" }
    ]
}*/

uniform float audioLevel;
uniform float audioBass;
uniform float audioMid;
uniform float audioHigh;

// ---- palettes (match the source's four schemes) ----
vec3 pal(int idx, float t, float b) {
    if (idx == 0) {
        return b * vec3(
            sin(t * 2.0) * 0.5 + 0.5,
            sin(t * 3.0 + 1.0) * 0.3 + 0.2,
            sin(t * 1.5 + 2.5) * 0.5 + 0.5
        );
    } else if (idx == 1) {
        return b * vec3(1.0, clamp(t, 0.0, 1.0) * 0.75, clamp(t * t, 0.0, 1.0) * 0.28);
    } else if (idx == 2) {
        return b * vec3(clamp(t, 0.0, 1.0) * 0.18, 0.55, 1.0);
    } else {
        vec3 h = fract(vec3(t) + vec3(0.0, 0.3333, 0.6667));
        return b * clamp(abs(h * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    }
}

mat3 rotY(float a) { float c = cos(a), s = sin(a); return mat3(c,0.0,s, 0.0,1.0,0.0, -s,0.0,c); }
mat3 rotX(float a) { float c = cos(a), s = sin(a); return mat3(1.0,0.0,0.0, 0.0,c,-s, 0.0,s,c); }

// ---- pattern fields (p in [-1..1]^3) ----

// Mode 0: Splines — three helical loci with traveling gaussian waves
float pat_splines(vec3 p, float t, float bass, float mid, float energy, float impulse) {
    // Distance to each helical curve (two perpendicular sinusoids per axis)
    vec2 s1 = vec2(cos(p.y * 3.0 + t * 0.8), sin(p.y * 3.0 + t * 0.8)) - vec2(p.x, p.z);
    vec2 s2 = vec2(cos(p.x * 2.5 + t * 0.6), sin(p.x * 2.5 + t * 0.6)) - vec2(p.y, p.z * 0.8);
    vec2 s3 = vec2(cos(p.z * 2.0 - t * 0.5), sin(p.z * 2.0 - t * 0.5)) - vec2(p.x * 0.7, p.y * 0.7);
    float d1 = dot(s1, s1), d2 = dot(s2, s2), d3 = dot(s3, s3);

    float w = 0.09 + impulse * 0.15;
    float peak = 0.25 + impulse * 1.2 + energy * 0.4;
    float b = peak * (exp(-d1 / w) + exp(-d2 / w) + exp(-d3 / w));

    // Traveling wave modulation along each curve — gives the pulse-along-spline look
    float tw1 = sin(p.y * 6.0 - t * (2.0 + bass * 4.0)) * 0.5 + 0.5;
    float tw2 = sin(p.x * 5.0 - t * (1.7 + mid  * 3.0)) * 0.5 + 0.5;
    float tw3 = sin(p.z * 4.0 + t * (1.3 + impulse * 2.0)) * 0.5 + 0.5;
    b *= mix(0.35, 1.0, (tw1 + tw2 + tw3) * 0.3333);
    return b;
}

// Mode 1: Lightning — dendrite-like filaments from 3D sinusoid interference
float pat_lightning(vec3 p, float t, float bass, float mid, float energy, float impulse) {
    // Warp domain for organic branching
    vec3 q = p + 0.25 * vec3(
        sin(p.y * 3.0 + t * 0.9),
        cos(p.z * 2.5 + t * 0.7),
        sin(p.x * 2.0 + t * 0.5)
    );
    float s = 0.0;
    for (int i = 0; i < 3; i++) {
        float fi = float(i);
        s += sin(q.x * (4.0 + fi)        + t * 0.5)
           * sin(q.y * (3.5 + fi * 1.2)  + t * 0.4)
           * sin(q.z * (4.2 - fi * 0.5)  + t * 0.3);
    }
    s *= 0.3333;
    float thresh = 0.32 - impulse * 0.18 - bass * 0.08;
    float filament = 1.0 - smoothstep(0.0, max(thresh, 0.04), abs(s));
    float b = filament * (0.25 + energy * 0.8 + impulse * 1.0);
    // Radial fade — keeps the dendrites inside the cube
    b *= 1.0 - smoothstep(0.85, 1.25, length(p));
    return b;
}

// Mode 2: Pulse — expanding shell from origin + treble sparkle
float pat_pulse(vec3 p, float t, float bass, float mid, float energy, float impulse) {
    float r = length(p);
    float phase = t * (0.6 + bass * 1.8);
    // Two overlapping radial shells for continuity
    float s1 = exp(-pow((r - fract(phase)       * 1.3) * 4.0, 2.0));
    float s2 = exp(-pow((r - fract(phase + 0.5) * 1.3) * 4.0, 2.0));
    float b = (s1 + s2) * (0.4 + impulse * 1.6);
    // Sparkle field modulated by treble
    float sp = sin(p.x * 14.0 + t * 2.0) * sin(p.y * 14.0 + t * 1.7) * sin(p.z * 14.0 + t * 1.3);
    b += clamp(sp, 0.0, 1.0) * audioHigh * 0.6;
    return b;
}

float patternAt(vec3 p, float t, float bass, float mid, float energy, float impulse) {
    if (patternMode == 0) return pat_splines  (p, t, bass, mid, energy, impulse);
    if (patternMode == 1) return pat_lightning(p, t, bass, mid, energy, impulse);
    return pat_pulse(p, t, bass, mid, energy, impulse);
}

void main() {
    vec2 uv = isf_FragNormCoord;
    vec2 p2 = (uv - 0.5) * 2.0;
    float aspect = RENDERSIZE.x / max(RENDERSIZE.y, 1.0);
    p2.x *= aspect;

    // Camera: orbit around cube, look toward origin
    float camDist = 3.0 / max(zoom, 0.001);
    vec3 ro = vec3(0.0, 0.0, -camDist);
    vec3 rd = normalize(vec3(p2, 1.6));
    mat3 rot = rotY(TIME * rotSpeed) * rotX(tilt);
    ro = rot * ro;
    rd = rot * rd;

    // Ray vs unit cube [-1..1]^3
    vec3 invRd = 1.0 / (rd + vec3(1e-6));
    vec3 t1 = (vec3(-1.0) - ro) * invRd;
    vec3 t2 = (vec3( 1.0) - ro) * invRd;
    vec3 tmn = min(t1, t2);
    vec3 tmx = max(t1, t2);
    float tEnter = max(max(tmn.x, tmn.y), tmn.z);
    float tExit  = min(min(tmx.x, tmx.y), tmx.z);

    vec3 col = vec3(0.0);

    if (tEnter < tExit && tExit > 0.0) {
        tEnter = max(tEnter, 0.0);

        float N = clamp(gridN, 8.0, 32.0);
        float voxSize = 2.0 / N;

        // Audio signals (approximate the source's impulse/energy)
        float bass   = audioBass * audioGain;
        float mid    = audioMid  * audioGain;
        float high   = audioHigh * audioGain;
        float energy = (bass * 3.0 + mid * 2.0 + high) / 6.0;
        float impulse = clamp(energy * energy * 2.0, 0.0, 1.0);

        // Raymarch — one step per voxel-slab across the traversal
        // 96 steps keeps cost bounded; diagonal is ~55 voxels at N=32.
        const int STEPS = 96;
        float segLen = tExit - tEnter;
        float stepSize = segLen / float(STEPS);
        // Dither start to break up banding
        float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        float tt = tEnter + stepSize * (0.5 + jitter * 0.5);

        for (int i = 0; i < STEPS; i++) {
            vec3 pos = ro + rd * tt;

            // Snap to nearest voxel center (gives the LED-grid look)
            vec3 voxIdx    = floor((pos + 1.0) / voxSize);
            vec3 voxCenter = (voxIdx + 0.5) * voxSize - 1.0;

            float brightness = patternAt(voxCenter, TIME, bass, mid, energy, impulse);

            if (brightness > 0.03) {
                // Gaussian point-sprite falloff from voxel center
                vec3 local = (pos - voxCenter) / voxSize;
                float d2  = dot(local, local);
                float dot_ = exp(-d2 * dotSharp);
                float halo = exp(-d2 * dotSharp * 0.18) * 0.22 * bloom;

                // Color by voxel position + slow time drift
                float colorT = TIME * 0.12
                             + voxCenter.x * 0.35
                             + voxCenter.y * 0.2
                             + voxCenter.z * 0.35;
                vec3 c = pal(palette, colorT, brightness);

                col += c * (dot_ + halo) * stepSize * 4.5;
            }

            tt += stepSize;
            if (tt > tExit) break;
        }
    }

    // Exposure + soft compression — prevents hot-white clipping on beats
    col *= exposure;
    col = col / (1.0 + col);
    col = pow(col, vec3(0.9));

    gl_FragColor = vec4(col, 1.0);
}
