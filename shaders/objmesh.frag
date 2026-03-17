#version 430 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uTextured = true;
uniform vec4 uSolidColor = vec4(0.0, 0.0, 0.0, 1.0);

void main() {
    if (uTextured) {
        FragColor = texture(uTexture, vTexCoord);
    } else {
        FragColor = uSolidColor;
    }
}
