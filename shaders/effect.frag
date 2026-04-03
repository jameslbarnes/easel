#version 430 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uEffectType = 0; // 0=blur, 1=coloradjust, 2=invert, 3=pixelate
uniform vec2 uResolution = vec2(1920, 1080);

// Blur
uniform float uBlurRadius = 4.0;
uniform int uBlurPass = 0; // 0=horizontal, 1=vertical

// Color adjust
uniform float uBrightness = 0.0;
uniform float uContrast = 0.0;
uniform float uSaturation = 0.0;
uniform float uHueShift = 0.0;

// Pixelate
uniform float uPixelSize = 8.0;

// Feedback
uniform sampler2D uFeedback;
uniform float uFeedbackMix = 0.8;
uniform float uFeedbackZoom = 1.01;

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    if (uEffectType == 0) {
        // Gaussian blur (9-tap)
        vec2 dir = (uBlurPass == 0) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
        vec2 texel = dir / uResolution;
        float r = uBlurRadius;

        vec4 sum = vec4(0.0);
        float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
        sum += texture(uTexture, vTexCoord) * weights[0];
        for (int i = 1; i < 5; i++) {
            sum += texture(uTexture, vTexCoord + texel * r * float(i)) * weights[i];
            sum += texture(uTexture, vTexCoord - texel * r * float(i)) * weights[i];
        }
        FragColor = sum;

    } else if (uEffectType == 1) {
        // Color adjust
        vec4 c = texture(uTexture, vTexCoord);
        // Brightness
        c.rgb += uBrightness;
        // Contrast
        c.rgb = (c.rgb - 0.5) * (1.0 + uContrast) + 0.5;
        // Saturation
        float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        c.rgb = mix(vec3(gray), c.rgb, 1.0 + uSaturation);
        // Hue shift
        if (uHueShift != 0.0) {
            vec3 hsv = rgb2hsv(c.rgb);
            hsv.x = fract(hsv.x + uHueShift / 360.0);
            c.rgb = hsv2rgb(hsv);
        }
        FragColor = clamp(c, 0.0, 1.0);

    } else if (uEffectType == 2) {
        // Invert
        vec4 c = texture(uTexture, vTexCoord);
        FragColor = vec4(1.0 - c.rgb, c.a);

    } else if (uEffectType == 3) {
        // Pixelate
        vec2 ps = vec2(uPixelSize) / uResolution;
        vec2 uv = floor(vTexCoord / ps) * ps + ps * 0.5;
        FragColor = texture(uTexture, uv);

    } else if (uEffectType == 4) {
        // Feedback (mix with previous frame)
        vec4 current = texture(uTexture, vTexCoord);
        vec2 fbUV = (vTexCoord - 0.5) / uFeedbackZoom + 0.5;
        vec4 prev = texture(uFeedback, fbUV);
        FragColor = mix(current, prev, uFeedbackMix);
    }
}
