#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uOpacity = 1.0;
uniform vec4 uBorderColor = vec4(0.0, 0.8, 1.0, 1.0);
uniform float uBorderWidth = 0.01; // UV units

void main() {
    vec2 uv = vTexCoord;
    // Thin border around the display edge
    float bw = uBorderWidth;
    if (uv.x < bw || uv.x > 1.0 - bw || uv.y < bw || uv.y > 1.0 - bw) {
        FragColor = uBorderColor;
        return;
    }
    vec4 color = texture(uTexture, uv);
    FragColor = vec4(color.rgb, color.a * uOpacity);
}
