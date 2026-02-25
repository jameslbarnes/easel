#version 330 core
layout(location = 0) in vec2 aPos;

void main() {
    // Convert from UV space (0-1) to clip space (-1 to 1)
    vec2 clipPos = aPos * 2.0 - 1.0;
    gl_Position = vec4(clipPos, 0.0, 1.0);
}
