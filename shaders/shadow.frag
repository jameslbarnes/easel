#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec3 uShadowColor = vec3(0.0);
uniform float uShadowOpacity = 1.0;
uniform float uShadowSpread = 1.0;

void main() {
    // Sample alpha of source layer; ignore color entirely
    float a = texture(uTexture, vTexCoord).a;
    // Spread: multiply then clamp (>1 thickens shadow into solid interior regions)
    float sa = clamp(a * uShadowSpread, 0.0, 1.0);
    FragColor = vec4(uShadowColor, sa * uShadowOpacity);
}
