/*{
    "DESCRIPTION": "Particle field bouncing off the edges of the canvas. Grid-seeded, audio-reactive, velocity-stretched streaks.",
    "CATEGORIES": ["Generator", "Particles", "Audio Reactive"],
    "CREDIT": "Easel / edges v1",
    "INPUTS": [
        { "NAME": "motionSpeed",    "TYPE": "float", "DEFAULT": 1.2, "MIN": 0.0, "MAX": 4.0, "LABEL": "Motion Speed" },
        { "NAME": "chaos",          "TYPE": "float", "DEFAULT": 0.6, "MIN": 0.0, "MAX": 2.0, "LABEL": "Chaos" },
        { "NAME": "particleSize",   "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.1, "MAX": 4.0, "LABEL": "Particle Size" },
        { "NAME": "stretch",        "TYPE": "float", "DEFAULT": 1.2, "MIN": 0.0, "MAX": 4.0, "LABEL": "Stretch" },
        { "NAME": "vortexStrength", "TYPE": "float", "DEFAULT": 0.8, "MIN": 0.0, "MAX": 3.0, "LABEL": "Vortex" },
        { "NAME": "audioReactivity","TYPE": "float", "DEFAULT": 0.7, "MIN": 0.0, "MAX": 2.0, "LABEL": "Audio" },
        { "NAME": "color1", "TYPE": "color", "DEFAULT": [1.0, 1.0, 1.0, 1.0], "LABEL": "Core" },
        { "NAME": "color2", "TYPE": "color", "DEFAULT": [0.1, 0.7, 1.0, 1.0], "LABEL": "Halo" },
        { "NAME": "bg",     "TYPE": "color", "DEFAULT": [0.02, 0.02, 0.03, 1.0], "LABEL": "Background" },
        { "NAME": "glow",   "TYPE": "float", "DEFAULT": 1.3, "MIN": 0.0, "MAX": 3.0, "LABEL": "Glow" }
    ]
}*/

float hash11(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

// Triangle-wave bounce: x (time-like) folded into [0,1] with reflection.
float bounce01(float x) { return abs(fract(x * 0.5) * 2.0 - 1.0); }

// 2D sinusoidal "vortex" — cheap analytic flow field, no noise tables.
vec2 vortex(vec2 p, float t) {
    float a = sin(p.x * 1.3 + t * 0.7) + cos(p.y * 1.7 - t * 0.5);
    float b = cos(p.x * 1.9 - t * 0.4) + sin(p.y * 1.1 + t * 0.9);
    return vec2(a, b) * 0.5;
}

void main() {
    vec2 uv = isf_FragNormCoord * 2.0 - 1.0;
    float aspect = RENDERSIZE.x / RENDERSIZE.y;
    uv.x *= aspect;

    float t = TIME;
    float audio = audioLevel + audioBass * 1.1 + audioHigh * 0.5;

    vec3 acc = vec3(0.0);
    const int N = 256;

    for (int i = 0; i < N; i++) {
        float fi = float(i);
        float s1 = hash11(fi * 1.37);
        float s2 = hash11(fi * 2.91 + 0.5);
        float s3 = hash11(fi * 4.17 + 0.3);
        float s4 = hash11(fi * 7.53 + 0.7);

        // Wider speed range + two stacked oscillators per axis → richer, less
        // periodic-feeling motion. Each particle has a dominant and secondary
        // frequency at 1.7× offset, mixed 70/30.
        float speedX1 = (0.2 + s1 * 2.8) * motionSpeed;
        float speedY1 = (0.2 + s2 * 2.8) * motionSpeed;
        float speedX2 = speedX1 * (1.0 + s3 * 0.8);
        float speedY2 = speedY1 * (1.0 + s4 * 0.8);
        float phaseX  = s3 * 6.2832;
        float phaseY  = s4 * 6.2832;
        float phaseX2 = s1 * 3.1416;
        float phaseY2 = s2 * 3.1416;

        float dt = 0.02;
        // Mix two bouncing oscillators so paths don't feel clockwork-regular.
        float bxA = bounce01(t      * speedX1 + phaseX) * 0.7
                  + bounce01(t      * speedX2 + phaseX2) * 0.3;
        float byA = bounce01(t      * speedY1 + phaseY) * 0.7
                  + bounce01(t      * speedY2 + phaseY2) * 0.3;
        float bxB = bounce01((t+dt) * speedX1 + phaseX) * 0.7
                  + bounce01((t+dt) * speedX2 + phaseX2) * 0.3;
        float byB = bounce01((t+dt) * speedY1 + phaseY) * 0.7
                  + bounce01((t+dt) * speedY2 + phaseY2) * 0.3;

        vec2 baseA = vec2(bxA, byA) * 2.0 - 1.0;
        vec2 baseB = vec2(bxB, byB) * 2.0 - 1.0;

        // Chaos: per-particle noise displacement that drifts in time. Adds
        // non-periodic jitter without ever escaping the canvas.
        vec2 chaosA = vec2(
            sin(t * (1.1 + s1 * 1.3) + s3 * 6.28),
            cos(t * (0.9 + s2 * 1.5) + s4 * 6.28)
        ) * chaos * 0.25;
        vec2 chaosB = vec2(
            sin((t+dt) * (1.1 + s1 * 1.3) + s3 * 6.28),
            cos((t+dt) * (0.9 + s2 * 1.5) + s4 * 6.28)
        ) * chaos * 0.25;
        baseA += chaosA;
        baseB += chaosB;
        baseA = clamp(baseA, vec2(-1.0), vec2(1.0));
        baseB = clamp(baseB, vec2(-1.0), vec2(1.0));

        // Aspect-stretched world-space positions.
        vec2 posA = vec2(baseA.x * aspect, baseA.y);
        vec2 posB = vec2(baseB.x * aspect, baseB.y);

        // Optional vortex perturbation.
        posA += vortex(posA, t)          * vortexStrength * 0.08;
        posB += vortex(posB, t + dt)     * vortexStrength * 0.08;

        vec2 vel = (posB - posA) / dt;
        float speed = length(vel);

        // Capsule endpoints for motion-stretched particle.
        float stretchLen = 0.006 * stretch * (0.5 + audio * audioReactivity);
        vec2 a = posA - vel * stretchLen;
        vec2 b = posA + vel * stretchLen;

        // Distance to capsule (line segment with rounded caps).
        vec2 pa = uv - a;
        vec2 ba = b - a;
        float denom = max(dot(ba, ba), 1e-6);
        float h = clamp(dot(pa, ba) / denom, 0.0, 1.0);
        float d = length(pa - ba * h);

        float r = 0.012 * particleSize * (0.6 + audio * audioReactivity * 0.6);
        float core = smoothstep(r, 0.0, d);
        float halo = exp(-d * 70.0);

        acc += mix(color2.rgb, color1.rgb, core) * (core + halo * 0.35);
    }

    vec3 rgb = bg.rgb + acc * glow;
    gl_FragColor = vec4(rgb, 1.0);
}
