#version 330 core
uniform sampler2D uState;
uniform vec2      uStateSize;  // e.g. (1024, 1024)
uniform float     uPointSize;  // base point size in px
out float vSpeed;

void main() {
    int id = gl_InstanceID;
    int iw = int(uStateSize.x);
    ivec2 tc = ivec2(id % iw, id / iw);
    vec4 state = texelFetch(uState, tc, 0);
    vec2 pos = state.xy;
    vec2 vel = state.zw;
    gl_Position = vec4(pos, 0.0, 1.0);
    float speed = length(vel);
    vSpeed = speed;
    gl_PointSize = clamp(uPointSize * (0.6 + speed * 2.5), 1.0, 48.0);
}
