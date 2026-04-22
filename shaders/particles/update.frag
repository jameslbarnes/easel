#version 330 core
in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

uniform sampler2D uState;
uniform float uTime;
uniform float uSpeed;
uniform float uVortex;
uniform float uChaos;
uniform bool  uSeed;

vec2 hash22(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453123);
}

void main() {
    vec2 uv = vTexCoord;

    if (uSeed) {
        vec2 r1 = hash22(uv + vec2(1.3, 7.7));
        vec2 r2 = hash22(uv + vec2(9.1, 2.3));
        vec2 pos = r1 * 2.0 - 1.0;
        vec2 dir = normalize(r2 * 2.0 - 1.0 + vec2(0.0001));
        vec2 vel = dir * (0.05 + r2.x * 0.15) * max(uSpeed, 0.01);
        fragColor = vec4(pos, vel);
        return;
    }

    vec4 state = texture(uState, uv);
    vec2 pos = state.xy;
    vec2 vel = state.zw;

    // Vortex from analytic curl-of-sin/cos flow field
    vel += 0.003 * uVortex * vec2(
        sin(pos.x * 3.0 + uTime * 0.5) + cos(pos.y * 2.7 - uTime * 0.3),
        cos(pos.x * 2.3 - uTime * 0.4) + sin(pos.y * 3.1 + uTime * 0.6)
    );

    // Chaos jitter
    vec2 rnd = hash22(uv + vec2(uTime * 0.137, uTime * 0.211)) * 2.0 - 1.0;
    vel += uChaos * 0.0015 * rnd;

    // Integrate
    pos += vel * uSpeed * 0.02;

    // Bounce at walls
    if (pos.x >  1.0) { pos.x =  1.0; vel.x = -vel.x; }
    if (pos.x < -1.0) { pos.x = -1.0; vel.x = -vel.x; }
    if (pos.y >  1.0) { pos.y =  1.0; vel.y = -vel.y; }
    if (pos.y < -1.0) { pos.y = -1.0; vel.y = -vel.y; }

    // Damping
    vel *= 0.995;

    // Speed clamp
    float sp = length(vel);
    if (sp > 2.0) vel = (vel / sp) * 2.0;

    fragColor = vec4(pos, vel);
}
