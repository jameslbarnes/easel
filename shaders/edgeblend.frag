#version 430 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;

// Blend widths as fraction of output (0.0 = no blend, 0.15 = 15% overlap)
uniform float uBlendLeft = 0.0;
uniform float uBlendRight = 0.0;
uniform float uBlendTop = 0.0;
uniform float uBlendBottom = 0.0;
uniform float uGamma = 2.2;

void main() {
    vec4 color = texture(uTexture, vTexCoord);

    // Compute blend factor for each edge (raised cosine for smooth falloff)
    float blend = 1.0;

    if (uBlendLeft > 0.0 && vTexCoord.x < uBlendLeft) {
        float t = vTexCoord.x / uBlendLeft;
        blend *= 0.5 - 0.5 * cos(t * 3.14159);
    }
    if (uBlendRight > 0.0 && vTexCoord.x > (1.0 - uBlendRight)) {
        float t = (1.0 - vTexCoord.x) / uBlendRight;
        blend *= 0.5 - 0.5 * cos(t * 3.14159);
    }
    if (uBlendBottom > 0.0 && vTexCoord.y < uBlendBottom) {
        float t = vTexCoord.y / uBlendBottom;
        blend *= 0.5 - 0.5 * cos(t * 3.14159);
    }
    if (uBlendTop > 0.0 && vTexCoord.y > (1.0 - uBlendTop)) {
        float t = (1.0 - vTexCoord.y) / uBlendTop;
        blend *= 0.5 - 0.5 * cos(t * 3.14159);
    }

    // Gamma-correct the blend for perceptual linearity
    blend = pow(blend, 1.0 / uGamma);

    FragColor = vec4(color.rgb * blend, color.a);
}
