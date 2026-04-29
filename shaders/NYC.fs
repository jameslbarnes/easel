/*{
  "DESCRIPTION": "NYC — procedural 3D city flythrough. Raymarched building grid with lit windows and LED ad walls (ad1/ad2/ad3 sample user textures). Time of day, mood, speed, drive/fly height, variety, ad brightness all exposed.",
  "CATEGORIES": ["Generator", "3D"],
  "INPUTS": [
    { "NAME": "ad1",           "LABEL": "Ad Texture A",       "TYPE": "image" },
    { "NAME": "ad2",           "LABEL": "Ad Texture B",       "TYPE": "image" },
    { "NAME": "ad3",           "LABEL": "Ad Texture C",       "TYPE": "image" },
    { "NAME": "timeOfDay",     "LABEL": "Time of Day",        "TYPE": "float", "DEFAULT": 0.12, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "windowLights",  "LABEL": "Window Lights",      "TYPE": "float", "DEFAULT": 0.55, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "mood",          "LABEL": "Mood (dark / bright)","TYPE": "float","DEFAULT": 0.5,  "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "adIntensity",   "LABEL": "Ad Brightness",      "TYPE": "float", "DEFAULT": 1.4,  "MIN": 0.0, "MAX": 3.0 },
    { "NAME": "variety",       "LABEL": "Building Variety",   "TYPE": "float", "DEFAULT": 0.85, "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "speed",         "LABEL": "Movement Speed",     "TYPE": "float", "DEFAULT": 0.7,  "MIN": 0.0, "MAX": 3.0 },
    { "NAME": "flightMode",    "LABEL": "Drive(0) / Fly(1)",  "TYPE": "float", "DEFAULT": 0.0,  "MIN": 0.0, "MAX": 1.0 },
    { "NAME": "fog",           "LABEL": "Fog Density",        "TYPE": "float", "DEFAULT": 0.5,  "MIN": 0.0, "MAX": 1.0 }
  ]
}*/

// NYC is a procedural city. We raymarch a grid of box-buildings, sample
// window/ad textures on surfaces, light with a time-of-day sky, and grade
// with a mood slider. No external mesh data — everything comes from hashes.

#define CELL        8.0        // grid spacing in world units
#define BUILD_HALF  3.0        // half-width of a typical building footprint
#define MAX_STEPS   96
#define MAX_DIST    260.0
#define EPS         0.015

// ──────────────────────────────────────────────────────────────────────
// Hashing
// ──────────────────────────────────────────────────────────────────────
float hash11(float p) { return fract(sin(p * 127.1) * 43758.5453); }
vec2 hash22(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}

// ──────────────────────────────────────────────────────────────────────
// Building descriptor — hashed per cell
//   height, footprint shrink, hasAd, adId (0..2)
// ──────────────────────────────────────────────────────────────────────
vec4 buildingInfo(vec2 cell) {
    vec2 r = hash22(cell);
    // Variety blends a flat baseline (1-variety) with variable heights.
    float tall = pow(r.x, 1.8);
    float h = mix(10.0, 6.0 + tall * 42.0, variety);
    // A fraction of the cells are ad buildings (LED screens on the front face).
    float hasAd = step(0.88, r.y);
    // 3 ad slots — pick which texture
    float adId = floor(hash11(cell.x * 13.7 + cell.y * 7.3) * 3.0);
    return vec4(h, r.x, hasAd, adId);
}

// SDF of an axis-aligned box centered at origin
float sdBox(vec3 p, vec3 b) {
    vec3 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

// Scene distance: closest-of-9-neighbors to handle buildings straddling cells
float mapScene(vec3 p, out vec2 outCell, out vec4 outInfo) {
    vec2 base = floor(p.xz / CELL);
    float best = 1e9;
    vec2 bestCell = base;
    vec4 bestInfo = vec4(0.0);
    // Sample the current cell + 8 neighbors — handles wide buildings.
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 cell = base + vec2(float(i), float(j));
            vec4 info = buildingInfo(cell);
            float h = info.x;
            float hw = BUILD_HALF * (0.55 + info.y * 0.45); // 0.55..1.0 of base
            vec3 c = vec3((cell.x + 0.5) * CELL, h * 0.5, (cell.y + 0.5) * CELL);
            vec3 q = p - c;
            float d = sdBox(q, vec3(hw, h * 0.5, hw));
            if (d < best) {
                best = d;
                bestCell = cell;
                bestInfo = info;
            }
        }
    }
    // Ground plane
    float ground = p.y;
    if (ground < best) {
        best = ground;
        bestCell = vec2(0.0);
        bestInfo = vec4(0.0, 0.0, -1.0, -1.0); // marker: ground
    }
    outCell = bestCell;
    outInfo = bestInfo;
    return best;
}

// Normal estimation by SDF gradient
vec3 estimateNormal(vec3 p) {
    vec2 dumCell; vec4 dumInfo;
    vec2 e = vec2(0.002, 0.0);
    return normalize(vec3(
        mapScene(p + e.xyy, dumCell, dumInfo) - mapScene(p - e.xyy, dumCell, dumInfo),
        mapScene(p + e.yxy, dumCell, dumInfo) - mapScene(p - e.yxy, dumCell, dumInfo),
        mapScene(p + e.yyx, dumCell, dumInfo) - mapScene(p - e.yyx, dumCell, dumInfo)
    ));
}

// ──────────────────────────────────────────────────────────────────────
// Sky — gradient driven by timeOfDay (0=night → 0.5=day → 1=night)
// ──────────────────────────────────────────────────────────────────────
vec3 skyColor(vec3 rd, float tod) {
    float sunAngle = tod * 6.2831853 - 1.5707963;
    float sunH = sin(sunAngle);
    vec3 night = vec3(0.02, 0.03, 0.08);
    vec3 day   = vec3(0.55, 0.72, 0.95);
    vec3 warm  = vec3(0.95, 0.50, 0.35);
    float dayMask = smoothstep(-0.1, 0.35, sunH);
    vec3 base = mix(night, day, dayMask);
    // Golden hour tint near sunrise/sunset
    float goldMask = smoothstep(0.0, 0.2, abs(sunH)) *
                     (1.0 - smoothstep(0.25, 0.55, abs(sunH)));
    vec3 col = mix(base, warm, goldMask * 0.45);
    // Horizon glow
    float horizon = 1.0 - abs(rd.y);
    col += vec3(0.6, 0.35, 0.25) * pow(horizon, 6.0) * goldMask;
    // Stars at night
    if (dayMask < 0.2) {
        vec2 sc = rd.xy * 80.0;
        float st = step(0.997, hash11(floor(sc.x) * 31.7 + floor(sc.y) * 17.3));
        col += vec3(st) * (1.0 - dayMask) * 0.8;
    }
    return col;
}

// ──────────────────────────────────────────────────────────────────────
// Window pattern on a building face + optional LED ad wall
//   p       : hit point
//   n       : surface normal
//   cell    : building cell (for window variation)
//   info    : building info (hasAd / adId)
//   outCol  : diffuse color
//   outEm   : emission multiplier
// ──────────────────────────────────────────────────────────────────────
void surfaceShade(vec3 p, vec3 n, vec2 cell, vec4 info,
                  out vec3 outCol, out float outEm)
{
    vec3 concrete = mix(vec3(0.15, 0.16, 0.2),
                        vec3(0.35, 0.36, 0.42),
                        hash11(dot(cell, vec2(1.7, 3.1))));
    outCol = concrete;
    outEm  = 0.0;

    // Ground: dark asphalt with occasional road markings
    if (info.z < 0.0) {
        outCol = vec3(0.05, 0.05, 0.07);
        float laneZ = fract(p.z * 0.04);
        float laneX = abs(fract(p.x / CELL) - 0.5);
        // Center road-dashes (only at x near cell center)
        if (laneX < 0.015 && laneZ > 0.5 && laneZ < 0.6) {
            outCol = vec3(0.95, 0.9, 0.45);
            outEm = 0.2;
        }
        return;
    }

    // Pick the dominant-axis face for UV projection
    vec3 an = abs(n);
    vec2 uv;
    float faceSign;
    if (an.x > an.z) {
        uv = vec2(p.z, p.y);
        faceSign = sign(n.x);
    } else {
        uv = vec2(p.x, p.y);
        faceSign = sign(n.z);
    }
    // Roofs: don't tile windows on the top; stay concrete
    if (an.y > max(an.x, an.z)) return;

    // Window tiling — ~1.2m wide, 2m tall per cell
    vec2 tile = vec2(1.2, 2.0);
    vec2 winCell = floor(uv / tile);
    vec2 winLocal = fract(uv / tile);

    // Window frame: glass rect inside each cell
    float mask = step(0.15, winLocal.x) * step(winLocal.x, 0.85)
               * step(0.15, winLocal.y) * step(winLocal.y, 0.85);

    // Which windows are lit — hash + windowLights threshold
    float litRnd = hash11(dot(winCell, vec2(12.7, 7.3)) + cell.x * 2.1 + cell.y * 0.7);
    float lit = step(1.0 - windowLights, litRnd) * mask;

    // Window color: mix warm indoor and cool fluorescent at random per cell
    vec3 warmInterior = vec3(1.00, 0.82, 0.55);
    vec3 coolInterior = vec3(0.80, 0.90, 1.00);
    vec3 winCol = mix(warmInterior, coolInterior, hash11(litRnd * 7.0));

    // Base surface = concrete where no window, lit window otherwise
    vec3 glass = mask * mix(vec3(0.08, 0.12, 0.18), winCol, lit);
    outCol = mix(concrete, glass, mask);
    outEm  = lit * 1.6;

    // ── LED ad wall ─────────────────────────────────────────────────
    // Buildings tagged hasAd get an ad surface on their +X or +Z face
    // spanning a vertical band. Sample one of 3 ad textures.
    if (info.z > 0.5 && faceSign > 0.5) {
        vec3 c = vec3((cell.x + 0.5) * CELL, info.x * 0.5, (cell.y + 0.5) * CELL);
        float adY0 = info.x * 0.15;
        float adY1 = info.x * 0.85;
        if (p.y > adY0 && p.y < adY1) {
            // UV on the ad panel
            float u, v;
            if (an.x > an.z) {
                u = (p.z - (cell.y + 0.5) * CELL + BUILD_HALF) / (BUILD_HALF * 2.0);
            } else {
                u = (p.x - (cell.x + 0.5) * CELL + BUILD_HALF) / (BUILD_HALF * 2.0);
            }
            v = (p.y - adY0) / (adY1 - adY0);
            u = clamp(u, 0.0, 1.0);
            v = clamp(v, 0.0, 1.0);
            // Which ad?
            vec3 adCol;
            if (info.w < 0.5)      adCol = texture2D(ad1, vec2(u, 1.0 - v)).rgb;
            else if (info.w < 1.5) adCol = texture2D(ad2, vec2(u, 1.0 - v)).rgb;
            else                    adCol = texture2D(ad3, vec2(u, 1.0 - v)).rgb;
            // Fallback when image input is unbound (black): use a default
            // neon gradient so the ad panel still reads as a screen.
            if (dot(adCol, vec3(1.0)) < 0.01) {
                adCol = vec3(0.4 + 0.6 * sin(u * 8.0 + TIME),
                             0.3 + 0.7 * sin(v * 6.0 - TIME * 1.3),
                             0.6 + 0.4 * sin((u + v) * 4.0 + TIME * 0.7));
                adCol = abs(adCol);
            }
            outCol = adCol;
            outEm  = dot(adCol, vec3(0.33)) * adIntensity;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// Main
// ──────────────────────────────────────────────────────────────────────
void main() {
    // Camera path — drives down a corridor, sways gently left/right.
    // flightMode raises the camera and pitches up slightly.
    float t = TIME * speed;
    float flyH   = mix(1.7, 26.0, flightMode);
    float lookH  = mix(0.3, 6.0,  flightMode);
    float swayX  = sin(t * 0.35) * mix(1.5, 4.0, flightMode);
    float swayY  = sin(t * 0.22) * mix(0.2, 3.0, flightMode);

    vec3 ro = vec3(swayX, flyH + swayY, t * 8.0);
    vec3 target = ro + vec3(sin(t * 0.17) * 2.0, lookH - flyH, 10.0);
    vec3 fwd = normalize(target - ro);
    vec3 rgt = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
    vec3 up  = cross(rgt, fwd);

    vec2 uv = (gl_FragCoord.xy / RENDERSIZE.xy) * 2.0 - 1.0;
    uv.x *= RENDERSIZE.x / RENDERSIZE.y;
    vec3 rd = normalize(fwd + rgt * uv.x * 0.95 + up * uv.y * 0.95);

    // Raymarch
    float td = 0.0;
    vec2 hitCell = vec2(0.0);
    vec4 hitInfo = vec4(0.0);
    bool hit = false;
    vec3 hitPos;
    for (int i = 0; i < MAX_STEPS; i++) {
        hitPos = ro + rd * td;
        float d = mapScene(hitPos, hitCell, hitInfo);
        if (d < EPS) { hit = true; break; }
        if (td > MAX_DIST) break;
        td += max(d * 0.9, 0.02);
    }

    vec3 sky = skyColor(rd, timeOfDay);
    vec3 col;

    if (hit) {
        vec3 n = estimateNormal(hitPos);
        vec3 diffuse;
        float emission;
        surfaceShade(hitPos, n, hitCell, hitInfo, diffuse, emission);

        // Lighting — cheap two-term model: sky dome ambient + sun directional.
        float sunHeight = sin(timeOfDay * 6.2831853 - 1.5707963);
        vec3 sunDir = normalize(vec3(0.6, max(sunHeight, -0.1) + 0.15, 0.4));
        float diff = max(dot(n, sunDir), 0.0);
        float ambient = 0.18 + 0.35 * max(sunHeight, 0.0);
        vec3 lit = diffuse * (ambient + diff * max(sunHeight, 0.0) * 0.8);
        // Window/ad emission ignores daylight so lit windows still read at noon.
        lit += diffuse * emission;

        col = lit;

        // Fog — distance-based, tinted toward sky
        float fogAmt = 1.0 - exp(-td * fog * 0.018);
        col = mix(col, sky, clamp(fogAmt, 0.0, 0.95));
    } else {
        col = sky;
    }

    // Mood: lift or crush the whole frame
    col = mix(col * 0.35, col * 1.25, mood);

    // Tone map + mild contrast so emission reads without blowing out
    col = col / (1.0 + col);
    col = pow(col, vec3(0.85));

    gl_FragColor = vec4(col, 1.0);
}
