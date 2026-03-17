#version 430 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uOpacity = 1.0;
uniform sampler2D uMask;
uniform bool uHasMask = false;
uniform float uTileX = 1.0;
uniform float uTileY = 1.0;
uniform vec4 uCrop = vec4(0);          // left, right, top, bottom
uniform int uMosaicMode = 0;           // 0=Mirror, 1=Hex
uniform float uMosaicDensity = 4.0;
uniform float uMosaicSpin = 0.0;
uniform float uTime = 0.0;
uniform float uAudioRMS = 0.0;
uniform float uAudioStrength = 0.0;
uniform int uMosaicModeFrom = 0;
uniform float uMosaicTransition = 1.0; // 0=old mode, 1=new mode

float cellPop(vec2 cell) {
    float h1 = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5);
    float h2 = fract(sin(dot(cell, vec2(269.5, 183.3))) * 28461.3);

    // Three waves at prime-ratio speeds — interference pattern rarely repeats
    float w1 = smoothstep(0.12, 0.0, min(abs(h1 - fract(uTime * 0.7)), 1.0 - abs(h1 - fract(uTime * 0.7))));
    float w2 = smoothstep(0.15, 0.0, min(abs(h2 - fract(uTime * 1.1)), 1.0 - abs(h2 - fract(uTime * 1.1))));
    float w3 = smoothstep(0.18, 0.0, min(abs(h1 - fract(uTime * 0.3 + 0.5)), 1.0 - abs(h1 - fract(uTime * 0.3 + 0.5))));

    // Radial pulse: beats ripple outward from center
    float dist = length(cell) * 0.15;
    float ripple = smoothstep(0.2, 0.0, abs(fract(uTime * 0.8 - dist) - 0.5) * 2.0);

    float pop = max(max(w1, w2 * 0.7), w3 * 0.5) + ripple * 0.4;
    pop *= uAudioRMS * uAudioStrength * 4.0;
    return clamp(pop, 0.0, 1.0);
}

vec2 mosaicMirror(vec2 uv, out float pop) {
    uv *= vec2(uTileX, uTileY);
    vec2 cell = floor(uv);
    uv = fract(uv);
    if (int(cell.x) % 2 == 1) uv.x = 1.0 - uv.x;
    if (int(cell.y) % 2 == 1) uv.y = 1.0 - uv.y;

    pop = cellPop(cell);
    uv = mix(uv, vec2(0.5), pop * 0.2);
    return uv;
}

vec2 mosaicHex(vec2 uv, out float pop) {
    float spin = radians(uMosaicSpin);
    vec2 c = uv - 0.5;
    c = vec2(c.x * cos(spin) - c.y * sin(spin),
             c.x * sin(spin) + c.y * cos(spin));
    vec2 p = c * uMosaicDensity;

    const vec2 s = vec2(1.0, 1.7320508);
    vec2 halfS = s * 0.5;
    vec2 a = mod(p + halfS, s) - halfS;
    vec2 b = mod(p, s) - halfS;

    vec2 gv;
    vec2 cellCenter;
    if (dot(a, a) < dot(b, b)) {
        gv = a;
        cellCenter = p - a;
    } else {
        gv = b;
        cellCenter = p - b;
    }

    pop = (length(cellCenter) < 0.5) ? 0.0 : cellPop(cellCenter);
    vec2 result = gv / s + 0.5;
    result = mix(result, vec2(0.5), pop * 0.2);
    return clamp(result, 0.0, 1.0);
}

// Sample texture through a given mosaic mode, applying crop
vec4 sampleWithMode(vec2 uv, int mode, vec2 cropMin, vec2 cropMax) {
    float pop = 0.0;

    if (mode == 0) {
        // Mirror: crop first, then tile
        uv = (uv - cropMin) / (cropMax - cropMin);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            return vec4(0);
        }
        uv = mosaicMirror(uv, pop);
    } else {
        uv = mosaicHex(uv, pop);
    }

    // Map back to texture space (apply crop)
    uv = cropMin + uv * (cropMax - cropMin);

    vec4 color = texture(uTexture, uv);
    color.rgb *= 1.0 + pop * 0.6;
    return color;
}

void main() {
    vec2 uv = vTexCoord;
    vec2 maskUV = uv;

    vec2 cropMin = vec2(uCrop.x, uCrop.w);
    vec2 cropMax = vec2(1.0 - uCrop.y, 1.0 - uCrop.z);

    vec4 color;

    if (uMosaicTransition >= 1.0) {
        // Fast path: no transition, single sample
        color = sampleWithMode(uv, uMosaicMode, cropMin, cropMax);
    } else {
        vec4 colorFrom = sampleWithMode(uv, uMosaicModeFrom, cropMin, cropMax);
        vec4 colorTo   = sampleWithMode(uv, uMosaicMode, cropMin, cropMax);

        float t = smoothstep(0.0, 1.0, uMosaicTransition);

        // Radial reveal: new pattern blooms from center
        float dist = length(uv - 0.5) * 2.0;
        float radialT = smoothstep(0.0, 1.0, t * 2.5 - dist * 0.7);

        color = mix(colorFrom, colorTo, radialT);

        // Brightness pulse at midpoint
        color.rgb *= 1.0 + sin(t * 3.14159) * 0.15;
    }

    color.a *= uOpacity;
    if (uHasMask) {
        float maskVal = texture(uMask, maskUV).r;
        color.a *= maskVal;
    }
    FragColor = color;
}
