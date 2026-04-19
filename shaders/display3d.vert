#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

uniform mat4 uMVP = mat4(1.0);

void main() {
    // aPos is a unit quad (-1 to 1), transformed by uMVP (model * view * proj)
    gl_Position = uMVP * vec4(aPos.x, aPos.y, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
