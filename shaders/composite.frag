#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uBase;      // accumulated result
uniform sampler2D uLayer;     // current layer
uniform float uOpacity = 1.0;
uniform int uBlendMode = 0;
uniform sampler2D uMask;
uniform bool uHasMask = false;
uniform mat3 uInvTransform = mat3(1.0);
uniform int uTileX = 1; // columns
uniform int uTileY = 1; // rows
uniform vec4 uCrop = vec4(0);      // left, right, top, bottom

vec3 blendNormal(vec3 base, vec3 blend)    { return blend; }
vec3 blendMultiply(vec3 base, vec3 blend)  { return base * blend; }
vec3 blendScreen(vec3 base, vec3 blend)    { return 1.0 - (1.0 - base) * (1.0 - blend); }

vec3 blendOverlay(vec3 base, vec3 blend) {
    return vec3(
        base.r < 0.5 ? 2.0 * base.r * blend.r : 1.0 - 2.0 * (1.0 - base.r) * (1.0 - blend.r),
        base.g < 0.5 ? 2.0 * base.g * blend.g : 1.0 - 2.0 * (1.0 - base.g) * (1.0 - blend.g),
        base.b < 0.5 ? 2.0 * base.b * blend.b : 1.0 - 2.0 * (1.0 - base.b) * (1.0 - blend.b)
    );
}

vec3 blendAdd(vec3 base, vec3 blend)       { return min(base + blend, 1.0); }
vec3 blendSubtract(vec3 base, vec3 blend)  { return max(base - blend, 0.0); }
vec3 blendDifference(vec3 base, vec3 blend){ return abs(base - blend); }

void main() {
    vec4 base = texture(uBase, vTexCoord);

    // Convert tex coords to NDC, apply inverse transform, back to UV
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec3 layerNDC = uInvTransform * vec3(ndc, 1.0);
    vec2 layerUV = layerNDC.xy * 0.5 + 0.5;

    // If layer UV is outside [0,1], this pixel is not covered by the layer
    if (layerUV.x < 0.0 || layerUV.x > 1.0 || layerUV.y < 0.0 || layerUV.y > 1.0) {
        FragColor = base;
        return;
    }

    // Keep un-mirrored UVs for the mask
    vec2 maskUV = layerUV;

    // Crop: remap UV into the content region
    vec2 cropMin = vec2(uCrop.x, uCrop.w);              // left, bottom
    vec2 cropMax = vec2(1.0 - uCrop.y, 1.0 - uCrop.z); // 1-right, 1-top
    vec2 uv = (layerUV - cropMin) / (cropMax - cropMin);

    // Outside cropped region = transparent
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = base;
        return;
    }

    // Tile with alternating mirror
    uv *= vec2(float(uTileX), float(uTileY));
    vec2 cell = floor(uv);
    uv = fract(uv);
    if (int(cell.x) % 2 == 1) uv.x = 1.0 - uv.x;
    if (int(cell.y) % 2 == 1) uv.y = 1.0 - uv.y;

    // Map back to texture space
    layerUV = cropMin + uv * (cropMax - cropMin);

    vec4 layer = texture(uLayer, layerUV);

    float alpha = layer.a * uOpacity;

    if (uHasMask) {
        float maskVal = texture(uMask, maskUV).r;
        alpha *= maskVal;
    }

    vec3 blended;
    switch (uBlendMode) {
        case 0: blended = blendNormal(base.rgb, layer.rgb); break;
        case 1: blended = blendMultiply(base.rgb, layer.rgb); break;
        case 2: blended = blendScreen(base.rgb, layer.rgb); break;
        case 3: blended = blendOverlay(base.rgb, layer.rgb); break;
        case 4: blended = blendAdd(base.rgb, layer.rgb); break;
        case 5: blended = blendSubtract(base.rgb, layer.rgb); break;
        case 6: blended = blendDifference(base.rgb, layer.rgb); break;
        default: blended = blendNormal(base.rgb, layer.rgb); break;
    }

    vec3 result = mix(base.rgb, blended, alpha);
    float resultAlpha = alpha + base.a * (1.0 - alpha);

    FragColor = vec4(result, resultAlpha);
}
