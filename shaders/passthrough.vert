#version 430 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

uniform mat3 uTransform = mat3(1.0);

void main() {
    vec3 pos = uTransform * vec3(aPos, 1.0);
    gl_Position = vec4(pos.xy, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
