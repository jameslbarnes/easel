#version 330 core
in float vSpeed;
uniform vec3  uColor1;
uniform vec3  uColor2;
uniform float uGlow;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;
    if (r > 1.0) discard;
    float core = smoothstep(1.0, 0.0, r);
    float halo = exp(-r * 3.0);
    vec3 col = mix(uColor2, uColor1, clamp(vSpeed * 2.0, 0.0, 1.0));
    fragColor = vec4(col * (core + halo * 0.4) * uGlow, core);
}
