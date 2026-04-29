/*{
  "DESCRIPTION": "Soft 3D gradient orb with drifting color stops, rim glow, optional video texture (masked to orb) with fisheye lens, blend modes, and optional wireframe ground grid",
  "CREDIT": "ShaderClaw",
  "CATEGORIES": ["Generator", "3D"],
  "INPUTS": [
    { "NAME": "inputTex", "LABEL": "Video", "TYPE": "image" },
    { "NAME": "color1", "LABEL": "Color 1", "TYPE": "color", "DEFAULT": [0.45, 0.95, 0.30, 1.0] },
    { "NAME": "color2", "LABEL": "Color 2", "TYPE": "color", "DEFAULT": [0.95, 0.45, 0.95, 1.0] },
    { "NAME": "color3", "LABEL": "Color 3", "TYPE": "color", "DEFAULT": [0.55, 0.25, 0.85, 1.0] },
    { "NAME": "color4", "LABEL": "Color 4", "TYPE": "color", "DEFAULT": [0.10, 0.30, 0.75, 1.0] },
    { "NAME": "gradIntensity", "LABEL": "Gradient Intensity", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.0, "MAX": 3.0 },
    { "NAME": "blendMode", "LABEL": "Blend Mode", "TYPE": "long", "VALUES": [0,1,2,3,4,5,6], "LABELS": ["Normal","Multiply","Screen","Overlay","Add","Soft Light","Color Dodge"], "DEFAULT": 3 },
    { "NAME": "blendAmount", "LABEL": "Blend Amount", "TYPE": "float", "DEFAULT": 0.8, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "fisheye", "LABEL": "Video Fisheye", "TYPE": "float", "DEFAULT": 0.3, "MIN": -1.0, "MAX": 1.5 },
    { "NAME": "videoZoom", "LABEL": "Video Zoom", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.3, "MAX": 3.0 },
    { "NAME": "sceneFisheye", "LABEL": "Scene Fisheye", "TYPE": "float", "DEFAULT": 0.0, "MIN": -1.0, "MAX": 1.5 },
    { "NAME": "orbRadius", "LABEL": "Orb Size", "TYPE": "float", "DEFAULT": 1.0, "MIN": 0.2, "MAX": 2.0 },
    { "NAME": "rotSpeed", "LABEL": "Rotation Speed", "TYPE": "float", "DEFAULT": 0.25, "MIN": 0.0, "MAX": 2.0 },
    { "NAME": "driftSpeed", "LABEL": "Gradient Drift", "TYPE": "float", "DEFAULT": 0.6, "MIN": 0.0, "MAX": 3.0 },
    { "NAME": "softness", "LABEL": "Edge Softness", "TYPE": "float", "DEFAULT": 0.55, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "glow", "LABEL": "Inner Glow", "TYPE": "float", "DEFAULT": 0.55, "MIN": 0.0, "MAX": 2.0 },
    { "NAME": "blur", "LABEL": "Gradient Blur", "TYPE": "float", "DEFAULT": 0.55, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "showGrid", "LABEL": "Show Grid", "TYPE": "bool", "DEFAULT": true },
    { "NAME": "gridColor", "LABEL": "Grid Color", "TYPE": "color", "DEFAULT": [0.45, 0.85, 1.0, 1.0] },
    { "NAME": "gridSpacing", "LABEL": "Grid Spacing", "TYPE": "float", "DEFAULT": 0.5, "MIN": 0.1, "MAX": 2.0 },
    { "NAME": "gridThickness", "LABEL": "Grid Thickness", "TYPE": "float", "DEFAULT": 0.02, "MIN": 0.005, "MAX": 0.1 },
    { "NAME": "gridOpacity", "LABEL": "Grid Opacity", "TYPE": "float", "DEFAULT": 0.7, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "gridHeight", "LABEL": "Grid Height", "TYPE": "float", "DEFAULT": -1.3, "MIN": -3.0, "MAX": 0.0 },
    { "NAME": "gridFog", "LABEL": "Grid Fog", "TYPE": "float", "DEFAULT": 0.08, "MIN": 0.0, "MAX": 0.5 }
  ]
}*/

#define PI 3.14159265
#define MAX_DIST 1000.0

vec3 rotY(vec3 p, float a) {
    float c = cos(a), s = sin(a);
    return vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
}

vec3 rotX(vec3 p, float a) {
    float c = cos(a), s = sin(a);
    return vec3(p.x, c * p.y - s * p.z, s * p.y + c * p.z);
}

// Analytic sphere intersection (sphere at origin, radius r). Returns nearest positive t.
bool hitSphere(vec3 ro, vec3 rd, float r, out float t) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float h = b * b - c;
    if (h < 0.0) return false;
    h = sqrt(h);
    float t0 = -b - h;
    float t1 = -b + h;
    t = t0 > 0.0 ? t0 : t1;
    return t > 0.0;
}

// Soft weighted blend over four anchor directions on the unit sphere.
vec3 orbGradient(vec3 n, float t, float softK) {
    vec3 a1 = normalize(vec3(sin(t * 0.71),        cos(t * 0.63 + 1.3),  sin(t * 0.92 + 0.4)));
    vec3 a2 = normalize(vec3(cos(t * 0.83 + 1.1),  sin(t * 0.57 + 2.0),  cos(t * 0.41 + 2.5)));
    vec3 a3 = normalize(vec3(sin(t * 0.37 + 3.2),  cos(t * 0.91 + 0.5),  sin(t * 0.68 + 1.3)));
    vec3 a4 = normalize(vec3(cos(t * 0.54 + 2.3),  sin(t * 0.76 + 1.8),  cos(t * 0.82 + 0.2)));

    float k = mix(6.0, 1.2, softK);

    float w1 = pow(max(0.0, dot(n, a1) * 0.5 + 0.5), k);
    float w2 = pow(max(0.0, dot(n, a2) * 0.5 + 0.5), k);
    float w3 = pow(max(0.0, dot(n, a3) * 0.5 + 0.5), k);
    float w4 = pow(max(0.0, dot(n, a4) * 0.5 + 0.5), k);
    float wsum = w1 + w2 + w3 + w4 + 1e-5;
    return (color1.rgb * w1 + color2.rgb * w2 + color3.rgb * w3 + color4.rgb * w4) / wsum;
}

// --- Blend modes ---
vec3 bOverlay(vec3 b, vec3 f) {
    return mix(2.0 * b * f, 1.0 - 2.0 * (1.0 - b) * (1.0 - f), step(0.5, b));
}
vec3 bSoftLight(vec3 b, vec3 f) {
    return mix(2.0 * b * f + b * b * (1.0 - 2.0 * f),
               sqrt(b) * (2.0 * f - 1.0) + 2.0 * b * (1.0 - f),
               step(0.5, f));
}
vec3 bColorDodge(vec3 b, vec3 f) {
    return clamp(b / max(1.0 - f, 1e-3), 0.0, 2.0);
}

vec3 applyBlend(vec3 base, vec3 over, int mode, float amt) {
    vec3 r;
    if (mode == 0)      r = over;
    else if (mode == 1) r = base * over;
    else if (mode == 2) r = 1.0 - (1.0 - base) * (1.0 - over);
    else if (mode == 3) r = bOverlay(base, over);
    else if (mode == 4) r = base + over;
    else if (mode == 5) r = bSoftLight(base, over);
    else                r = bColorDodge(base, over);
    return mix(base, r, amt);
}

// Fisheye distortion of a disk coordinate p in [-1,1]
vec2 fisheyeWarp(vec2 p, float amt) {
    float r2 = dot(p, p);
    float nz = sqrt(max(0.0, 1.0 - r2));
    // amt > 0: pushes content outward (barrel / bulge lens feel)
    // amt < 0: pulls content to center (pincushion)
    float k = amt;
    vec2 pd = p * (1.0 + k * (1.0 - nz));
    return pd;
}

void main() {
    vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
    vec2 p = uv * 2.0 - 1.0;
    p.x *= RENDERSIZE.x / RENDERSIZE.y;

    vec3 ro = vec3(0.0, 0.0, 4.0);

    // Scene-wide fisheye: radial distortion of the primary ray direction.
    // Positive = barrel / bulge (whole scene warps outward, orb balloons),
    // negative = pincushion (scene pinches toward center).
    vec2 pRay = p;
    if (abs(sceneFisheye) > 0.001) {
        float r2 = dot(p, p);
        float nz = sqrt(max(0.0, 1.0 - min(r2, 1.0)));
        pRay = p * (1.0 + sceneFisheye * (1.0 - nz));
    }
    vec3 rd = normalize(vec3(pRay, -1.8));

    vec3 col = vec3(0.0);
    float tOrb = MAX_DIST;

    // ---- Orb ----
    float t;
    if (hitSphere(ro, rd, orbRadius, t)) {
        tOrb = t;
        vec3 hp = ro + rd * t;
        vec3 n = normalize(hp);

        // Rotating sample direction for drifting gradient
        vec3 sn = rotY(n, TIME * rotSpeed);
        sn = rotX(sn, TIME * rotSpeed * 0.7);

        vec3 gradient = orbGradient(sn, TIME * driftSpeed, blur) * gradIntensity;

        // Video texture masked to the orb disk, with fisheye lens
        vec3 videoCol = vec3(0.0);
        bool hasVideo = IMG_SIZE_inputTex.x > 0.0;
        if (hasVideo) {
            // Normalized disk coordinate from the orb's screen projection
            vec2 dp = hp.xy / orbRadius;
            dp = fisheyeWarp(dp, fisheye);
            // Zoom around center
            dp /= max(videoZoom, 1e-3);
            vec2 vUV = dp * 0.5 + 0.5;
            // Correct for video aspect vs orb (orb disk is square in world, video is wider)
            float vAspect = IMG_SIZE_inputTex.x / max(IMG_SIZE_inputTex.y, 1.0);
            vUV.x = (vUV.x - 0.5) / vAspect + 0.5;
            if (vUV.x >= 0.0 && vUV.x <= 1.0 && vUV.y >= 0.0 && vUV.y <= 1.0) {
                videoCol = texture2D(inputTex, vUV).rgb;
            }
        }

        int bm = int(blendMode + 0.5);
        vec3 base = hasVideo ? applyBlend(videoCol, gradient, bm, blendAmount) : gradient;

        // Rim falloff to black at silhouette
        float ndv = clamp(dot(n, -rd), 0.0, 1.0);
        float rim = pow(ndv, mix(0.25, 2.8, softness));
        vec3 c = base * rim;
        // Inner glow
        c += base * glow * pow(ndv, 5.0);

        col = c;
    }

    // ---- Ground grid ----
    if (showGrid && rd.y < 0.0) {
        float tg = (gridHeight - ro.y) / rd.y;
        if (tg > 0.0 && tg < tOrb) {
            vec3 gp = ro + rd * tg;

            float thick = gridThickness * (1.0 + tg * 0.15);
            float thickNorm = clamp(thick / gridSpacing, 0.001, 0.49);

            vec2 cell = abs(fract(gp.xz / gridSpacing) - 0.5);
            float lineX = smoothstep(0.5, 0.5 - thickNorm, cell.x);
            float lineZ = smoothstep(0.5, 0.5 - thickNorm, cell.y);
            float line = max(lineX, lineZ);

            float fog = exp(-gridFog * tg);
            float a = line * gridOpacity * fog;

            col += gridColor.rgb * a;
        }
    }

    col = clamp(col, 0.0, 4.0);
    gl_FragColor = vec4(col, 1.0);
}
