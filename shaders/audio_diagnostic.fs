/*{
    "DESCRIPTION": "Audio diagnostic - displays audio uniform values as colored bars",
    "INPUTS": []
}*/

// Audio uniforms provided by Easel
uniform float audioLevel;
uniform float audioBass;
uniform float audioMid;
uniform float audioHigh;
uniform sampler2D audioFFT;

void main() {
    vec2 uv = isf_FragNormCoord;

    vec3 color = vec3(0.05);

    // Layout: 4 vertical bars + FFT spectrum at bottom
    float barWidth = 0.2;
    float gap = 0.04;
    float startX = 0.06;

    // Bar backgrounds
    float barBottom = 0.25;
    float barTop = 0.95;

    // Bar 1: audioLevel (white)
    float x1 = startX;
    if (uv.x > x1 && uv.x < x1 + barWidth && uv.y > barBottom && uv.y < barTop) {
        float fillHeight = barBottom + audioLevel * (barTop - barBottom);
        if (uv.y < fillHeight) {
            color = vec3(0.9, 0.9, 0.9);
        } else {
            color = vec3(0.12);
        }
    }

    // Bar 2: audioBass (red)
    float x2 = x1 + barWidth + gap;
    if (uv.x > x2 && uv.x < x2 + barWidth && uv.y > barBottom && uv.y < barTop) {
        float fillHeight = barBottom + audioBass * (barTop - barBottom);
        if (uv.y < fillHeight) {
            color = vec3(0.9, 0.2, 0.15);
        } else {
            color = vec3(0.12);
        }
    }

    // Bar 3: audioMid (green)
    float x3 = x2 + barWidth + gap;
    if (uv.x > x3 && uv.x < x3 + barWidth && uv.y > barBottom && uv.y < barTop) {
        float fillHeight = barBottom + audioMid * (barTop - barBottom);
        if (uv.y < fillHeight) {
            color = vec3(0.2, 0.8, 0.3);
        } else {
            color = vec3(0.12);
        }
    }

    // Bar 4: audioHigh (cyan)
    float x4 = x3 + barWidth + gap;
    if (uv.x > x4 && uv.x < x4 + barWidth && uv.y > barBottom && uv.y < barTop) {
        float fillHeight = barBottom + audioHigh * (barTop - barBottom);
        if (uv.y < fillHeight) {
            color = vec3(0.2, 0.7, 0.9);
        } else {
            color = vec3(0.12);
        }
    }

    // FFT spectrum at bottom (full width)
    if (uv.y < 0.2) {
        float fftVal = texture(audioFFT, vec2(uv.x, 0.5)).r;
        float fftHeight = fftVal * 0.2;
        if (uv.y < fftHeight) {
            // Color gradient by frequency
            color = mix(vec3(0.9, 0.2, 0.1), vec3(0.1, 0.5, 0.9), uv.x);
        } else {
            color = vec3(0.06);
        }
    }

    // Labels area (top strip)
    // Just use the bar colors as indicators

    gl_FragColor = vec4(color, 1.0);
}
