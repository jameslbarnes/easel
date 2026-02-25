#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uOpacity = 1.0;
uniform sampler2D uMask;
uniform bool uHasMask = false;
uniform int uTileX = 1; // columns
uniform int uTileY = 1; // rows
uniform vec4 uCrop = vec4(0);      // left, right, top, bottom

void main() {
    vec2 uv = vTexCoord;

    // Crop: remap UV into the content region
    vec2 cropMin = vec2(uCrop.x, uCrop.w);              // left, bottom
    vec2 cropMax = vec2(1.0 - uCrop.y, 1.0 - uCrop.z); // 1-right, 1-top
    uv = (uv - cropMin) / (cropMax - cropMin);

    // Outside cropped region = transparent
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = vec4(0);
        return;
    }

    // Tile with alternating mirror
    uv *= vec2(float(uTileX), float(uTileY));
    vec2 cell = floor(uv);
    uv = fract(uv);
    // Mirror on odd cells
    if (int(cell.x) % 2 == 1) uv.x = 1.0 - uv.x;
    if (int(cell.y) % 2 == 1) uv.y = 1.0 - uv.y;

    // Map back to texture space
    uv = cropMin + uv * (cropMax - cropMin);

    vec4 color = texture(uTexture, uv);
    color.a *= uOpacity;
    if (uHasMask) {
        float maskVal = texture(uMask, vTexCoord).r;
        color.a *= maskVal;
    }
    FragColor = color;
}
