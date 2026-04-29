# 15 New ISF Shader Specs for Easel

Each shader is a single `.fs` file with an ISF JSON header followed by `void main()`.
Available built-ins assumed: `RENDERSIZE`, `gl_FragCoord`, `TIME`, `inputTex`,
`audioBass`, `audioMid`, `audioHigh`, `audioLevel`, `audioFFT` (sampler2D, 256 bins
horizontally), `mousePos` (vec2 0..1). All generators declare
`"CATEGORIES": ["Generator"]` (or `"Effect"` / `"Transition"` where noted).

Conventions:
- `uv = gl_FragCoord.xy / RENDERSIZE.xy` (0..1)
- `p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy) / RENDERSIZE.y` (centered, aspect-corrected)
- FFT bin lookup: `texture2D(audioFFT, vec2(bin/256.0, 0.5)).r`
- `inputTex` sampled via `IMG_NORM_PIXEL(inputTex, uv)` (or equivalent for the host).

---

## 1. TEXT_3D_AURORA
**Vibe:** Volumetric typography haunted by audio-driven aurora ribbons that trail each glyph like northern lights bleeding off the letterforms.
**Reference:** Hangs between Pipilotti Rist's saturated colour-bath and the literal Aurora Borealis — type as cold-light apparition rather than information.
**Technique:** Reuses the layered atlas-sampled extrusion from `text_3dtype.fs` (16+ depth slices, perspective-projected). Behind the type, a separate ribbon-field is rendered: each character spawns 2-3 ribbons whose Y-axis follows `sin(uv.x*freq + TIME)` and whose `freq`/amplitude are bound to `audioMid` and `audioHigh`. Ribbon colour comes from a perceptual HSL sweep along `uv.x + TIME*driftSpeed`. A back-to-front additive composite (depth slices behind ribbons behind front face) plus a subtle gaussian glow gives the "light pass" character. Aurora opacity uses smoothstep on `(audioBass*pulse)` so loud transients flare the curtain.
**Audio mapping:** `audioBass` → ribbon flare/global brightness pulse; `audioMid` → ribbon vertical amplitude; `audioHigh` → ribbon thickness + secondary frequency overtones; `audioLevel` → depth extrusion length.
**Texture treatment:** `inputTex` optional — when present it tints the ribbons (sampled along `(uv.x, 0.5 + ribbonY)`) so live video bleeds through the aurora as a palette source.
**Pitfalls:** Ribbon `amp` above ~0.4 stops reading as aurora and starts looking like rainbow noise. `extrudeDepth` near 0 collapses the 3D stack. `glyphCount` of 1 with high `kerning` will desync ribbon spawn positions.
**INPUTS:**
```json
[
  {"NAME":"msg","TYPE":"text","DEFAULT":"AURORA","MAX_LENGTH":48},
  {"NAME":"extrudeDepth","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"ribbonAmp","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.18},
  {"NAME":"ribbonFreq","TYPE":"float","MIN":1.0,"MAX":20.0,"DEFAULT":6.0},
  {"NAME":"driftSpeed","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.4},
  {"NAME":"glow","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"audioReact","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec3 col = vec3(0.0);
// 1. Render extruded text stack (back→front), tint by depth slice
for (int i = SLICES-1; i >= 0; --i) { col = mix(col, sliceColor(uv,i), glyphMask(uv,i)); }
// 2. Add aurora ribbons in screen space behind/around glyphs
float r = ribbonField(uv, ribbonFreq, ribbonAmp * (1.0 + audioMid*audioReact), TIME*driftSpeed);
vec3 ribbonCol = hsv2rgb(vec3(uv.x + TIME*0.05, 0.85, 1.0)) * r * (0.5 + audioBass*audioReact);
col += ribbonCol * glow;
gl_FragColor = vec4(col, 1.0);
```

---

## 2. MEMPHIS_PRIMITIVES
**Vibe:** A Sottsass / Memphis Group postcard come alive — checkered grids, squiggles, polka dots, primary triangles all rearranging themselves on the beat.
**Reference:** Ettore Sottsass + Nathalie du Pasquier (Memphis Milano, 1981); Bauhaus primaries (Itten / Albers); echoes of Saved By The Bell title cards. Bold flat colour, no gradients.
**Technique:** The screen is divided into a `gridX × gridY` cell grid. Each cell uses `hash(cellId)` to pick one of N primitive types: solid square, circle, triangle, checkerboard, diagonal stripes, squiggle (sine line), polka-dots. Primitive parameters animate via `TIME * cellHash`. Audio-coupled `layoutShift` re-rolls hash seeds at beats so cells morph type. Palette is a fixed 5-colour Memphis ramp (red `#E63946`, yellow `#F4D35E`, blue `#3A86FF`, black, off-white) selected by `int(hash*5)`.
**Audio mapping:** `audioBass` → re-roll cells (layout shift on transient); `audioMid` → primitive rotation speed; `audioHigh` → squiggle frequency; `audioLevel` → cell scale jitter.
**Texture treatment:** `inputTex` used as a *mask* — only cells where `IMG_NORM_PIXEL(inputTex, cellCenter).r > threshold` are rendered, the rest fall back to bgColor. Lets the user paint primitives into the shape of live video.
**Pitfalls:** `gridX*gridY > ~64` makes every cell tiny and reads as noise. `layoutShift` at max with quiet audio still flickers. `primitiveScale` at 1.0 makes shapes touch and breaks the grid feel — keep ≤0.85.
**INPUTS:**
```json
[
  {"NAME":"gridX","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":6.0},
  {"NAME":"gridY","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":4.0},
  {"NAME":"layoutShift","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"primitiveScale","TYPE":"float","MIN":0.3,"MAX":0.9,"DEFAULT":0.7},
  {"NAME":"rotationSpeed","TYPE":"float","MIN":0.0,"MAX":3.0,"DEFAULT":0.5},
  {"NAME":"useMask","TYPE":"bool","DEFAULT":false},
  {"NAME":"bgColor","TYPE":"color","DEFAULT":[0.96,0.94,0.88,1.0]},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 cell = vec2(gridX, gridY);
vec2 cId = floor(uv * cell);
vec2 cUV = fract(uv * cell);              // 0..1 inside cell
float seed = hash(cId + floor(audioBass*layoutShift*8.0)); // beat re-roll
int prim = int(seed * 7.0);
float ang = TIME * rotationSpeed * (0.5 + audioMid);
vec3 col = drawPrimitive(prim, cUV, ang, primitiveScale, palette(seed));
if (useMask && IMG_NORM_PIXEL(inputTex, (cId+0.5)/cell).r < 0.3) col = bgColor.rgb;
gl_FragColor = vec4(col, 1.0);
```

---

## 3. PROXIMITY_FIELD
**Vibe:** A swarm of luminous points wandering the canvas; every pixel glows with the colour of whichever point is closest, smeared by weighted distance to the next two.
**Reference:** Voronoi-as-art — recalls Lubalin's "letter constellations" and Höller's Light Wall; also Casey Reas / Generative Aesthetics. Reads as "field of attractors" rather than cells.
**Technique:** N points (8-20), each point has a base position on a Lissajous curve `(sin(a*TIME+φ), cos(b*TIME+φ))` plus an audio-driven offset. For every fragment, compute distance to all N points, sort the smallest 2-3 distances. Final colour = weighted blend of those nearest points' colours, weight `= 1/(d^k)` (k controls falloff sharpness — high k → hard Voronoi cells, low k → soft gradient field). Mouse acts as an extra attractor pulling the nearest point. Edges between cells emerge naturally where d1≈d2 — render them as bright leading lines via `smoothstep(0, edgeWidth, abs(d1-d2))`.
**Audio mapping:** `audioBass` → global radial scale of point positions (cluster expands on bass); `audioMid` → individual point jitter; `audioHigh` → edge-line brightness; `audioLevel` → glow halo around each point.
**Texture treatment:** `inputTex` provides per-point colour: each point samples `inputTex` at its current position, so the live video pixel at each attractor becomes that cell's colour.
**Pitfalls:** `falloffK` < 0.5 yields a grey mush — every point contributes everywhere. `falloffK` > 8 produces hard cells with no gradient. `pointCount` > 20 starts hurting frame rate (O(N) per pixel). Without `inputTex` plugged in, all cells look identical unless `paletteVariety` > 0.
**INPUTS:**
```json
[
  {"NAME":"pointCount","TYPE":"float","MIN":4.0,"MAX":20.0,"DEFAULT":10.0},
  {"NAME":"falloffK","TYPE":"float","MIN":0.5,"MAX":8.0,"DEFAULT":2.5},
  {"NAME":"motionSpeed","TYPE":"float","MIN":0.0,"MAX":3.0,"DEFAULT":0.6},
  {"NAME":"edgeWidth","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.008},
  {"NAME":"mousePull","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"paletteVariety","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.7},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy) / RENDERSIZE.y;
float d1=1e9, d2=1e9; vec3 c1=vec3(0), c2=vec3(0);
for (int i=0; i<MAX_PTS; ++i) {
  if (float(i) >= pointCount) break;
  vec2 pos = pointPath(i, TIME*motionSpeed) * (1.0 + audioBass*0.3);
  if (i==0) pos = mix(pos, mousePos*2.0-1.0, mousePull);
  float d = length(p - pos);
  vec3 c = pointColor(i, pos);
  if (d<d1) { d2=d1; c2=c1; d1=d; c1=c; } else if (d<d2) { d2=d; c2=c; }
}
float w = pow(d1, falloffK)/(pow(d1,falloffK)+pow(d2,falloffK));
vec3 col = mix(c1, c2, w);
col += smoothstep(edgeWidth, 0.0, abs(d1-d2)) * audioHigh;
gl_FragColor = vec4(col, 1.0);
```

---

## 4. LIQUID_RIPPLES_3D
**Vibe:** Sound made literally visible — concentric ripples rolling across stacked depth-planes, audio frequencies sculpting interference patterns in 3D space.
**Reference:** Ryoichi Kurokawa's audiovisual installations; the photographic record of standing waves on water (Cymatics). Suggests the viewer is suspended above a frequency-driven pond.
**Technique:** Sample the FFT at K bins to get frequency amplitudes. For each of `layers` depth-planes (3-6), drop a ripple source at a hashed position; its waveform is `sin(length(p - srcPos)*freqK - TIME*speed) * amp` with `freqK` = bin index × scale and `amp` = FFT magnitude at that bin. Sum ripple heights per plane, encode height as luminance. Stack planes with parallax: front planes get larger `mousePos`-driven offset, back planes barely move. Final blend = additive with falling alpha per layer; a depth-fog `mix(col, fogColor, planeIndex/layers)` sells the recession.
**Audio mapping:** Each ripple-source bound to a different FFT bin — bass bins drive low-frequency long-wavelength ripples in back planes, treble bins drive tight high-freq ripples in front. `audioLevel` → global ripple amplitude.
**Texture treatment:** `inputTex` reads as the surface itself — `inputTex` UV is offset by `(rippleHeight*refraction)`, simulating refraction through the rippling fluid.
**Pitfalls:** `layers` > 6 + `refraction` > 0.05 → muddy soup. `freqScale` too high → moiré aliasing. With silent audio the entire shader is flat; `idleAmp` keeps gentle motion when quiet.
**INPUTS:**
```json
[
  {"NAME":"layers","TYPE":"float","MIN":1.0,"MAX":6.0,"DEFAULT":4.0},
  {"NAME":"freqScale","TYPE":"float","MIN":4.0,"MAX":40.0,"DEFAULT":16.0},
  {"NAME":"speed","TYPE":"float","MIN":0.0,"MAX":4.0,"DEFAULT":1.5},
  {"NAME":"refraction","TYPE":"float","MIN":0.0,"MAX":0.08,"DEFAULT":0.02},
  {"NAME":"parallax","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.08},
  {"NAME":"idleAmp","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.1},
  {"NAME":"fogColor","TYPE":"color","DEFAULT":[0.02,0.04,0.08,1.0]},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy) / RENDERSIZE.y;
vec3 col = vec3(0.0); float totalH = 0.0;
for (int L=0; L<6; ++L) {
  if (float(L) >= layers) break;
  float depth = float(L)/layers;
  vec2 pp = p + (mousePos-0.5) * parallax * (1.0-depth);
  float bin = mix(0.04, 0.6, depth);                       // bass back, treble front
  float amp = texture2D(audioFFT, vec2(bin,0.5)).r + idleAmp;
  vec2 src = hashPos(L);
  float h = sin(length(pp-src)*freqScale*(1.0+depth)*0.5 - TIME*speed) * amp;
  totalH += h;
  col = mix(col, planeColor(L) * (0.5+0.5*h), 1.0/layers);
}
vec4 tex = IMG_NORM_PIXEL(inputTex, gl_FragCoord.xy/RENDERSIZE.xy + totalH*refraction);
gl_FragColor = vec4(mix(col, tex.rgb, 0.5) + fogColor.rgb*0.2, 1.0);
```

---

## 5. PARTICLE_GRID
**Vibe:** A perfect rectangular constellation; every cell is bound to one FFT bin, bass at the left, treble at the right, breathing in unison like a city seen from a plane.
**Reference:** Kraftwerk stage matrices; Ryoji Ikeda's `data.matrix`. Modernist, monospaced, scientific.
**Technique:** Render `cols × rows` grid. For each cell `(cx, cy)`, sample FFT at `bin = cx/cols * 0.95` (skip the very top to avoid aliasing). Each cell renders a circle (or square) whose radius = `cellRadius * (0.3 + amp*1.5)`, with rotation = `amp * TIME` and a small per-cell hash phase to desync. Cells optionally jitter position by `amp*jitter`. Colour interpolates left→right across a 2-stop ramp (`lowColor` → `highColor`) so the spectrum is also visualised chromatically. A trail / decay term smooths sudden FFT spikes: `displayed = max(prev*decay, fftNow)` (ISF persistent buffer or a simple `mix(amp, fftNow, 0.6)` per-frame approximation).
**Audio mapping:** Direct — each cell column is one FFT bin slice. `audioBass` brightens column 0, `audioHigh` brightens last column. `audioLevel` → global brightness floor.
**Texture treatment:** `inputTex` provides per-cell *fill colour* — each cell samples `inputTex` at cell-center, so live video becomes pixelated through the FFT-driven dot grid (a frequency-aware mosaic).
**Pitfalls:** `cols > 96` → cells become subpixel and FFT detail disappears. `jitter` > 0.4 makes the grid identity collapse. Without `decay`, the grid flickers harshly on percussive content — keep `decay` ≥ 0.85 for cinematic feel.
**INPUTS:**
```json
[
  {"NAME":"cols","TYPE":"float","MIN":8.0,"MAX":96.0,"DEFAULT":48.0},
  {"NAME":"rows","TYPE":"float","MIN":4.0,"MAX":48.0,"DEFAULT":24.0},
  {"NAME":"cellRadius","TYPE":"float","MIN":0.05,"MAX":0.5,"DEFAULT":0.32},
  {"NAME":"jitter","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.05},
  {"NAME":"decay","TYPE":"float","MIN":0.5,"MAX":0.99,"DEFAULT":0.9},
  {"NAME":"useTex","TYPE":"bool","DEFAULT":false},
  {"NAME":"lowColor","TYPE":"color","DEFAULT":[1.0,0.2,0.3,1.0]},
  {"NAME":"highColor","TYPE":"color","DEFAULT":[0.2,0.8,1.0,1.0]},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 grid = vec2(cols, rows);
vec2 cId = floor(uv*grid);
vec2 cUV = fract(uv*grid) - 0.5;
float bin = (cId.x + 0.5) / cols * 0.95;
float amp = texture2D(audioFFT, vec2(bin, 0.5)).r;
vec2 jit = (vec2(hash(cId), hash(cId+1.7))-0.5) * jitter * amp;
float r = cellRadius * (0.3 + amp*1.5);
float dot = smoothstep(r, r*0.85, length(cUV - jit));
vec3 base = useTex ? IMG_NORM_PIXEL(inputTex, (cId+0.5)/grid).rgb
                   : mix(lowColor.rgb, highColor.rgb, cId.x/cols);
gl_FragColor = vec4(base * dot * (amp + 0.1), 1.0);
```

---

## 6. SOUND_TEMPLE_OCTAGON
**Vibe:** Standing at the centre of an octagonal chamber while eight frequency bands circle you in synchronised pulses — a sound-temple where light orbits the listener.
**Reference:** Turrell's `Aten Reign` (Guggenheim, 2013) octagonal light apse; circular Buddhist mandala geometry; multi-channel surround installations (Bernhard Leitner). Designed to look correct on an 8-projector dome rig.
**Technique:** Convert fragment to polar `(r, θ)` with `θ = atan(p.y, p.x)`. Quantise: `sector = floor((θ + π)/(2π) * 8)` → 0..7. Each sector picks a different FFT band: `bin = (sector+0.5)/8 * 0.8`. Within a sector, draw a radial pulse: `pulseRing = smoothstep(width, 0, abs(r - pulsePos))` where `pulsePos = fract(TIME*speed - sector*0.125)` so pulses chase around the octagon. Sector colour from a 8-stop palette wheel. A soft inter-sector seam (anti-aliased on the octant boundary) keeps the geometry crisp without harsh edges. Centre disc shows `audioLevel` as a breathing core.
**Audio mapping:** Per-sector FFT bin (8 bins evenly distributed across the spectrum); `audioBass` drives the centre core; `audioLevel` modulates global ring glow.
**Texture treatment:** `inputTex` mapped *radially* — sample at `(θ/(2π), r)` so the input wraps around the octagon as if printed on the temple's interior wall.
**Pitfalls:** `pulseWidth` < 0.01 → invisible thin lines. `seamSoftness` too high blurs the octagon into a circle. With `inputTex` rotation enabled, an off-centre input image looks lopsided — recommend square images.
**INPUTS:**
```json
[
  {"NAME":"sectors","TYPE":"float","MIN":4.0,"MAX":12.0,"DEFAULT":8.0},
  {"NAME":"pulseSpeed","TYPE":"float","MIN":0.0,"MAX":3.0,"DEFAULT":0.6},
  {"NAME":"pulseWidth","TYPE":"float","MIN":0.01,"MAX":0.2,"DEFAULT":0.04},
  {"NAME":"seamSoftness","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.008},
  {"NAME":"coreSize","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.12},
  {"NAME":"texMix","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"paletteShift","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy)/RENDERSIZE.y;
float r = length(p);
float th = atan(p.y, p.x);
float sec = floor((th + 3.14159265)/(2.0*3.14159265) * sectors);
float bin = (sec + 0.5) / sectors * 0.8;
float amp = texture2D(audioFFT, vec2(bin, 0.5)).r;
float ring = smoothstep(pulseWidth, 0.0, abs(r - fract(TIME*pulseSpeed - sec/sectors)));
vec3 col = sectorColor(sec, paletteShift) * ring * (0.4 + amp*1.6);
col += vec3(1.0) * smoothstep(coreSize, coreSize*0.7, r) * audioBass;
vec3 tex = IMG_NORM_PIXEL(inputTex, vec2(th/6.2832 + 0.5, r)).rgb;
gl_FragColor = vec4(mix(col, tex, texMix), 1.0);
```

---

## 7. DALI_MELT
**Vibe:** The image surrenders to gravity. Pixels droop, sag, smear downward in slow heavy waves like Dalí's melting clocks; audio bursts widen the drips.
**Reference:** Salvador Dalí, *The Persistence of Memory* (1931). Also Cocteau Twins album art (Vaughan Oliver). Soft chromatic separation borrows from the Krzysztof Kieślowski colour-grade.
**Technique:** For each fragment, compute a *vertical sag offset* `sag = sin(uv.x*sagFreq + TIME*flow) * sagAmp * (1.0 + audioMid*reactive)`. Sample `inputTex` at `vec2(uv.x + smear*sin(uv.y*4.0), uv.y - sag)`. Perform tiny per-channel offsets for chromatic separation: R sampled at sag, G at sag*0.95, B at sag*1.08, producing a wet aberration. Apply a heavy gaussian blur weighted vertically (drip blur) then tone-map with a gentle S-curve. Optional drip-tendril field: where `sag` exceeds threshold, extend a thin downward streak by sampling N times along `-y` and averaging.
**Audio mapping:** `audioMid` → sag amplitude; `audioBass` → drip threshold (transients pop out heavy droplets); `audioHigh` → chromatic separation width; `audioLevel` → flow speed multiplier.
**Texture treatment:** `inputTex` is the **content** itself. Without `inputTex`, render a procedural Dalí desert (gradient sky → ochre ground) so the shader still works as a generator.
**Pitfalls:** `sagAmp` > 0.4 collapses image into vertical stripes. `chroma` > 0.05 reads as RGB-shift glitch instead of liquid. `flow` at 0 freezes — keep ≥ 0.05 for "still wet" feel.
**INPUTS:**
```json
[
  {"NAME":"sagAmp","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.12},
  {"NAME":"sagFreq","TYPE":"float","MIN":0.5,"MAX":8.0,"DEFAULT":2.5},
  {"NAME":"flow","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.3},
  {"NAME":"chroma","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.012},
  {"NAME":"smear","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.01},
  {"NAME":"reactive","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"dripBlur","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
float sag = sin(uv.x*sagFreq + TIME*flow*(1.0+audioLevel)) * sagAmp * (1.0 + audioMid*reactive);
vec2 base = vec2(uv.x + smear*sin(uv.y*4.0 + TIME*flow), uv.y - sag);
float ch = chroma * (1.0 + audioHigh);
float r = IMG_NORM_PIXEL(inputTex, base + vec2(0.0,  ch)).r;
float g = IMG_NORM_PIXEL(inputTex, base                   ).g;
float b = IMG_NORM_PIXEL(inputTex, base + vec2(0.0, -ch)).b;
vec3 col = vec3(r,g,b);
col = verticalDripBlur(col, base, dripBlur);                // 5-tap downward
gl_FragColor = vec4(col, 1.0);
```

---

## 8. TURRELL_CHROMA
**Vibe:** A breathing volume of pure colour-light — almost no motion, just slow chromatic drift you only notice if you watch for a minute. Reads as architectural light, not screen content.
**Reference:** James Turrell, `Aten Reign` (Guggenheim 2013) and the Skyspace series; the *Ganzfeld* perceptual works. Ad Reinhardt's near-monochrome black paintings sit nearby.
**Technique:** Two analog-feeling colour fields (`colorA`, `colorB`) interpolate via a slow Perlin noise in time only — `t = noise(TIME*0.02)`. Spatially, a soft radial vignette darkens edges (`pow(1-r, vignette)`). A second very-low-frequency 2D noise (`noise(uv*0.5 + TIME*0.005)`) modulates a tiny hue rotation per region so the field is almost-but-not-quite uniform. No hard edges anywhere. Cycle period configurable from 30s to 5min. Tiny film-grain (`hash(uv*RENDERSIZE + TIME)*0.02`) keeps the image alive on projection (otherwise OLED-banding becomes visible).
**Audio mapping:** Intentionally subtle — `audioLevel` adds at most 5% brightness pulse, scaled by `audioInfluence` (default low). This is a *meditative* shader; user can crank `audioInfluence` if they want it more reactive.
**Texture treatment:** `inputTex` used only for *colour sampling* — averaged colour (sample 3 widely-spaced pixels, mean) injects into the palette rotation. So a live camera of the sunset slowly bleeds its tones into the room.
**Pitfalls:** `cyclePeriod` < 5s breaks the meditative quality and feels like a screensaver. `vignette` > 4 makes the corners pure black and breaks the immersive light-field illusion. `grain` at 0 → visible OLED gradient bands on projection.
**INPUTS:**
```json
[
  {"NAME":"colorA","TYPE":"color","DEFAULT":[0.92,0.35,0.55,1.0]},
  {"NAME":"colorB","TYPE":"color","DEFAULT":[0.25,0.35,0.85,1.0]},
  {"NAME":"cyclePeriod","TYPE":"float","MIN":5.0,"MAX":300.0,"DEFAULT":60.0},
  {"NAME":"vignette","TYPE":"float","MIN":0.0,"MAX":4.0,"DEFAULT":1.4},
  {"NAME":"grain","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.015},
  {"NAME":"audioInfluence","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.05},
  {"NAME":"texInfluence","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 c = uv - 0.5;
float r = length(c) * 1.4;
float t = 0.5 + 0.5 * sin(TIME * 6.2832 / cyclePeriod);
vec3 base = mix(colorA.rgb, colorB.rgb, t);
base = mix(base, IMG_NORM_PIXEL(inputTex, vec2(0.5)).rgb, texInfluence);
base *= 1.0 - pow(r, vignette);
base += (audioLevel * audioInfluence);
base += (hash(uv*RENDERSIZE + TIME) - 0.5) * grain;
gl_FragColor = vec4(base, 1.0);
```

---

## 9. OPART_RILEY_WAVES
**Vibe:** Dense black-and-white parallel waves bending and swimming until your eyes refuse to focus. Op-art that breathes with the music.
**Reference:** Bridget Riley, *Movement in Squares* (1961), *Cataract 3* (1967), *Fall* (1963). Also Victor Vasarely. Pure perceptual destabilisation.
**Technique:** Generate a stripe field: `stripe = sin(uv.y * freq + warp(uv,TIME))` then `bw = step(0.0, stripe)` for hard contrast. The `warp` function is the trick: `warp = sin(uv.x*xFreq + TIME*flow) * warpAmp + audioCurl(uv)*audioMid`. Optional accent colour: every Nth stripe substitutes `accentColor` instead of black (makes it pop without ruining the op-art read). Add slight contrast easing at edges via smoothstep so projection doesn't strobe at high freq.
**Audio mapping:** `audioBass` → global warp amplitude (the pattern bends harder); `audioMid` → warp curl turbulence; `audioHigh` → stripe frequency modulation (waves get tighter); `audioLevel` → optional brief invert flash at peaks.
**Texture treatment:** `inputTex` used as a *displacement guide* — its luminance offsets `uv.y` before computing the stripe, so live video drives the bend pattern. Reads as "Riley waves around the silhouette".
**Pitfalls:** `freq` > 200 + projection at low res = aliased shimmer (need supersample or cap). `warpAmp` > 0.5 destroys the parallel structure entirely. `accentEvery` = 1 turns every stripe into accent and kills the BW read.
**INPUTS:**
```json
[
  {"NAME":"freq","TYPE":"float","MIN":10.0,"MAX":160.0,"DEFAULT":60.0},
  {"NAME":"warpAmp","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.12},
  {"NAME":"xFreq","TYPE":"float","MIN":0.5,"MAX":12.0,"DEFAULT":3.0},
  {"NAME":"flow","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.4},
  {"NAME":"accentEvery","TYPE":"float","MIN":2.0,"MAX":20.0,"DEFAULT":7.0},
  {"NAME":"accentColor","TYPE":"color","DEFAULT":[0.95,0.15,0.25,1.0]},
  {"NAME":"texDisplace","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
float guide = (texDisplace>0.0) ? IMG_NORM_PIXEL(inputTex, uv).r - 0.5 : 0.0;
float warp = sin(uv.x*xFreq + TIME*flow) * warpAmp * (1.0 + audioBass);
warp += guide * texDisplace;
float y = uv.y + warp;
float stripe = sin(y * freq * (1.0 + audioHigh*0.1));
float bw = smoothstep(-0.02, 0.02, stripe);
float idx = floor(y * freq / 3.14159);
vec3 col = (mod(idx, accentEvery) < 0.5) ? accentColor.rgb : vec3(bw);
col = mix(vec3(0.0), col, bw);
gl_FragColor = vec4(col, 1.0);
```

---

## 10. CHLADNI_FIGURES
**Vibe:** Sand on a vibrating plate. Audio frequencies sculpt the classic Chladni nodal lines into living constellations.
**Reference:** Ernst Chladni's 1787 plate experiments — sand settles on nodal lines where vibration is zero. Also Hans Jenny, *Cymatics*. Pure physics-as-art.
**Technique:** The Chladni equation: `f(x,y) = sin(n·π·x)·sin(m·π·y) - sin(m·π·x)·sin(n·π·y)`. `n` and `m` are integer-ish modes driven by audio: `n = floor(2 + audioBass*8)`, `m = floor(2 + audioHigh*8)`. Compute `f` per fragment over `uv ∈ [0,1]`. Visualise either as (a) **nodal lines**: `pattern = smoothstep(threshold, 0, abs(f))` — sharp lines where f≈0, or (b) **sand particles**: render bright dots at fragments that are local minima (use `abs(f) < eps`). Smooth `n,m` transitions across frames with exponential ease-in to avoid mode-jump strobing. Background is dark plate material; sand colour is a warm off-white.
**Audio mapping:** `audioBass` → mode `n` (low-frequency major divisions); `audioHigh` → mode `m` (fine subdivisions); `audioMid` → vibration jitter (perturbs `(x,y)` slightly); `audioLevel` → particle density / line brightness.
**Texture treatment:** `inputTex` used as the *plate texture* — the dark background samples `inputTex` so a video can be the surface the sand rests on (great for projection onto a real plate).
**Pitfalls:** `n` or `m` jumping by more than 1 per frame = strobing. Apply `smoothstep` damping or temporal lerp. `lineSharpness` near 0 dissolves into noise. `n=m` produces a degenerate (zero everywhere) pattern — clamp to `n != m`.
**INPUTS:**
```json
[
  {"NAME":"baseN","TYPE":"float","MIN":1.0,"MAX":12.0,"DEFAULT":3.0},
  {"NAME":"baseM","TYPE":"float","MIN":1.0,"MAX":12.0,"DEFAULT":5.0},
  {"NAME":"audioModeRange","TYPE":"float","MIN":0.0,"MAX":8.0,"DEFAULT":4.0},
  {"NAME":"lineSharpness","TYPE":"float","MIN":0.001,"MAX":0.1,"DEFAULT":0.02},
  {"NAME":"jitter","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.005},
  {"NAME":"sandColor","TYPE":"color","DEFAULT":[0.95,0.88,0.7,1.0]},
  {"NAME":"plateColor","TYPE":"color","DEFAULT":[0.06,0.05,0.05,1.0]},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
float n = baseN + audioBass*audioModeRange;          // smoothed by host or persistent buf
float m = baseM + audioHigh*audioModeRange;
if (abs(n-m) < 0.5) m += 1.0;
vec2 q = uv + (vec2(hash(uv), hash(uv+1.3))-0.5) * jitter * audioMid;
float f = sin(n*3.14159*q.x)*sin(m*3.14159*q.y) - sin(m*3.14159*q.x)*sin(n*3.14159*q.y);
float line = smoothstep(lineSharpness, 0.0, abs(f));
vec3 plate = mix(plateColor.rgb, IMG_NORM_PIXEL(inputTex, uv).rgb, 0.5);
gl_FragColor = vec4(mix(plate, sandColor.rgb, line * (0.4 + audioLevel)), 1.0);
```

---

## 11. HOLOGRAM_GLITCH
**Vibe:** A signal trying to hold itself together — scanlines, RGB shift, vertical tear, EMI bursts. The image *transmits* rather than displays.
**Reference:** *Blade Runner 2049* hologram aesthetics; `Ghost in the Shell` opening; Nam June Paik's TV-as-medium. Lo-fi cyberpunk reverence for analog decay.
**Technique:** Take `inputTex` (or fallback procedural test pattern) and apply a stack: (1) horizontal RGB shift `R(uv+chroma), G(uv), B(uv-chroma)`; (2) scanlines `(0.85 + 0.15*sin(uv.y*RENDERSIZE.y*scanFreq))`; (3) vertical tear: at random TIME intervals, shift a horizontal band by a hash-driven offset (`if (uv.y in [bandY, bandY+h]) uv.x += tearAmount`); (4) signal break: every K seconds, replace a slice with `hash(uv*TIME)*0.2`; (5) edge bloom `pow(luminance, 1.4)*glow`. Bass triggers `tearAmount` and break frequency.
**Audio mapping:** `audioBass` → glitch burst probability + tear magnitude (heavy bass = the hologram fritzes); `audioHigh` → scanline contrast & RGB shift width; `audioMid` → flicker; `audioLevel` → overall transmission strength (low audio = the hologram dims).
**Texture treatment:** `inputTex` is the **content being holographically projected**. Without it, a procedural circuit-grid stands in.
**Pitfalls:** `chroma` > 0.04 looks like a broken display rather than a hologram. `tearProbability` > 0.3 is unwatchable — recommend ≤ 0.1. `scanFreq` mismatched to projector resolution causes moiré; expose `scanFreq` so user can tune per device.
**INPUTS:**
```json
[
  {"NAME":"chroma","TYPE":"float","MIN":0.0,"MAX":0.04,"DEFAULT":0.008},
  {"NAME":"scanFreq","TYPE":"float","MIN":1.0,"MAX":4.0,"DEFAULT":2.0},
  {"NAME":"tearProbability","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.06},
  {"NAME":"breakAmount","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"glow","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.7},
  {"NAME":"hologramTint","TYPE":"color","DEFAULT":[0.4,1.0,0.95,1.0]},
  {"NAME":"audioReact","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
float bandH = 0.04;
float bandY = floor(uv.y / bandH) * bandH;
float tearTrig = step(1.0 - tearProbability*(1.0 + audioBass*audioReact), hash(vec2(bandY, floor(TIME*8.0))));
uv.x += tearTrig * (hash(vec2(bandY, TIME))-0.5) * 0.15;
float ch = chroma * (1.0 + audioHigh*audioReact);
float r = IMG_NORM_PIXEL(inputTex, uv + vec2(ch,0)).r;
float g = IMG_NORM_PIXEL(inputTex, uv               ).g;
float b = IMG_NORM_PIXEL(inputTex, uv - vec2(ch,0)).b;
vec3 col = vec3(r,g,b) * hologramTint.rgb;
col *= 0.85 + 0.15*sin(gl_FragCoord.y * scanFreq);
col = mix(col, vec3(hash(uv*TIME)), breakAmount * audioBass*audioReact * 0.3);
gl_FragColor = vec4(col * (audioLevel*0.5+0.5) + col*glow*0.3, 1.0);
```

---

## 12. SOLAR_FLARE_CORONA
**Vibe:** Looking at the sun. Convective surface roils, magnetic ropes arc off the limb in fiery loops, the corona flares with the music.
**Reference:** SDO/SOHO solar imagery; classic fire-shader lineage (Inigo Quilez); Olafur Eliasson's *The Weather Project* (Tate, 2003). Awe-scale.
**Technique:** The disc: render a bright circle, surface texture is fbm noise `fbm(p*scale + TIME*flow)` mapped through a fire palette (black → deep red → orange → yellow → white). The corona: outside the disc, do a curl-noise field — sample noise gradients at neighboring points, take perpendicular vector — and trace short streamlines from the limb outward. Magnetic loops: pick K hashed footpoint pairs on the limb, parametrise an arc between them as a half-ellipse; render the arc with a thickness-falloff and an additive glow. `audioMid` adds extra arcs; `audioBass` triggers a global flare brightening pulse. Rim glow via `pow(1.0 - distFromCentre/discRadius, 2.0)`.
**Audio mapping:** `audioBass` → flare-burst flash + rim glow magnitude; `audioMid` → number of magnetic loops visible; `audioHigh` → corona turbulence scale; `audioLevel` → overall brightness.
**Texture treatment:** `inputTex` mixed into the surface palette — useful to inject other live colour into the photosphere (e.g. a red-tinted camera feed warms the sun further).
**Pitfalls:** Without LUT-baked fire palette, naive `mix(red,yellow,t)` looks plasticky — implement a proper 5-stop gradient. `loopCount` > 12 is GPU-heavy. `coronaReach` > 1.2 fills the whole frame and the disc disappears.
**INPUTS:**
```json
[
  {"NAME":"discRadius","TYPE":"float","MIN":0.2,"MAX":0.6,"DEFAULT":0.35},
  {"NAME":"surfaceScale","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":5.0},
  {"NAME":"flow","TYPE":"float","MIN":0.0,"MAX":1.5,"DEFAULT":0.3},
  {"NAME":"loopCount","TYPE":"float","MIN":0.0,"MAX":12.0,"DEFAULT":5.0},
  {"NAME":"coronaReach","TYPE":"float","MIN":0.1,"MAX":1.2,"DEFAULT":0.45},
  {"NAME":"flareIntensity","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"texMix","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy)/RENDERSIZE.y;
float r = length(p);
float surf = fbm(p*surfaceScale + vec2(TIME*flow, 0.0));
vec3 disc = firePalette(surf + audioBass*0.3) * step(r, discRadius);
float rim = pow(max(0.0, 1.0 - r/discRadius), 2.0);
vec3 corona = firePalette(curlNoise(p, TIME*flow*0.5)) * smoothstep(discRadius+coronaReach, discRadius, r);
vec3 loops = magneticLoops(p, discRadius, int(loopCount + audioMid*4.0), TIME);
vec3 col = disc + rim * vec3(1.0,0.7,0.3) * (1.0+audioBass*flareIntensity) + corona*audioHigh + loops;
col = mix(col, IMG_NORM_PIXEL(inputTex, gl_FragCoord.xy/RENDERSIZE.xy).rgb * col, texMix);
gl_FragColor = vec4(col * (0.7 + audioLevel*0.5), 1.0);
```

---

## 13. DEEP_CAVERN
**Vibe:** Falling slowly into an infinite tunnel that breathes. Concentric rings recede forever; the camera tilts with the mouse; deep hypnosis.
**Reference:** Eliasson's `Your Spiral View` (2002), Höller's `Test Site` slides at Tate; M.C. Escher's recursive perspectives; Stanley Kubrick's *2001* star-gate.
**Technique:** Polar coordinates from screen centre with a mouse-driven offset (the centre moves toward `mousePos`). Tunnel coord: `u = atan(p.y, p.x)/(2π); v = 1.0 / length(p)`. Animate `v += TIME*pullSpeed` so rings march toward the camera. Render concentric rings: `ring = step(0.5, fract(v*ringDensity))` — alternating bands. Apply radial fog: `fog = pow(1.0 - exp(-length(p)*fogDensity), 1.8)`, dimming distant rings. Each ring band has a different colour drawn from a 4-stop palette indexed by `floor(v*ringDensity)`. Add edge-glow on ring boundaries via `smoothstep(0.05, 0, abs(fract(v*ringDensity)-0.5))`. Audio bass squeezes the tunnel rhythmically.
**Audio mapping:** `audioBass` → tunnel "breathe" (radial squeeze: multiply `length(p)` by `1.0 + bass*0.15`); `audioMid` → ring colour rotation; `audioHigh` → edge-glow brightness; `audioLevel` → overall pull speed gain.
**Texture treatment:** `inputTex` mapped onto the tunnel walls — sample at `(u, fract(v))`. The video literally lines the cavern's interior, receding to vanishing point.
**Pitfalls:** `ringDensity` > 60 → moiré at the centre. Without `fogDensity` ≥ 1.0 the rings stack into a flat bullseye. `pullSpeed` > 4 induces real motion sickness — flag in UI.
**INPUTS:**
```json
[
  {"NAME":"pullSpeed","TYPE":"float","MIN":0.0,"MAX":4.0,"DEFAULT":0.7},
  {"NAME":"ringDensity","TYPE":"float","MIN":4.0,"MAX":60.0,"DEFAULT":20.0},
  {"NAME":"fogDensity","TYPE":"float","MIN":0.5,"MAX":4.0,"DEFAULT":1.5},
  {"NAME":"breathe","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.12},
  {"NAME":"mouseTilt","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.2},
  {"NAME":"edgeGlow","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.8},
  {"NAME":"texMix","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 p = (gl_FragCoord.xy - 0.5*RENDERSIZE.xy)/RENDERSIZE.y;
p -= (mousePos - 0.5) * mouseTilt;
float r = length(p) * (1.0 + audioBass*breathe);
float th = atan(p.y, p.x);
float v = 1.0/max(r, 1e-3) + TIME*pullSpeed*(1.0 + audioLevel*0.5);
float u = th / 6.2832 + 0.5;
float band = floor(v*ringDensity);
vec3 ringCol = ringPalette(band + audioMid*2.0);
float fog = exp(-r*fogDensity);
float edge = smoothstep(0.06, 0.0, abs(fract(v*ringDensity)-0.5)) * edgeGlow * (0.4+audioHigh);
vec3 tex = IMG_NORM_PIXEL(inputTex, vec2(u, fract(v))).rgb;
gl_FragColor = vec4(mix(ringCol, tex, texMix) * fog + edge*ringCol, 1.0);
```

---

## 14. STAINED_GLASS
**Vibe:** Cathedral stained-glass cells — irregular jewel-toned panels divided by black lead lines, with God-rays angling through the windows.
**Reference:** Chartres Cathedral rose windows; Frank Lloyd Wright's leaded-glass panels; Gerhard Richter's Cologne Cathedral window (2007). Sacred geometry meets digital voronoi.
**Technique:** Compute jittered Voronoi cells over `uv` (standard 3x3 neighbour scan with hash-displaced cell points). For each fragment, find nearest cell point F1 and second-nearest F2. The boundary `abs(F2-F1) < leadWidth` is rendered as black "leading". Inside the cell, fill colour comes from: (a) optional `inputTex` sample at the cell's centre, OR (b) jewel-tone palette indexed by cell hash (`hash(cellId) → {ruby, sapphire, amber, emerald, violet}`). God-rays: a directional gradient `dot(uv, lightDir)` adds a soft white highlight that tilts with `mousePos`. Per-cell brightness modulates with FFT bin assigned by cell hash.
**Audio mapping:** Each cell takes one FFT bin (from cell hash); `audioBass` → light-ray intensity (sun blooms); `audioMid` → cell brightness pulse; `audioHigh` → lead-line glow.
**Texture treatment:** Each cell can hold one *sampled pixel* from `inputTex` (sampled at its center). With `texPerCell` ≥ 0.5 the live video is reduced to a stained-glass mosaic.
**Pitfalls:** `leadWidth` > 0.05 → cells so small they vanish. `jitter` near 0 → rectangular grid, not stained glass. With heavy `texPerCell`, ensure colour palette saturation boosts (multiply by 1.2) so cells stay jewel-like.
**INPUTS:**
```json
[
  {"NAME":"cellScale","TYPE":"float","MIN":2.0,"MAX":20.0,"DEFAULT":7.0},
  {"NAME":"jitter","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.85},
  {"NAME":"leadWidth","TYPE":"float","MIN":0.001,"MAX":0.04,"DEFAULT":0.012},
  {"NAME":"lightAngle","TYPE":"float","MIN":0.0,"MAX":6.2832,"DEFAULT":0.8},
  {"NAME":"lightIntensity","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.7},
  {"NAME":"texPerCell","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"saturation","TYPE":"float","MIN":0.5,"MAX":1.6,"DEFAULT":1.2},
  {"NAME":"inputTex","TYPE":"image"}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 g = uv * cellScale;
vec2 gi = floor(g), gf = fract(g);
float d1=1e9, d2=1e9; vec2 cellId; vec2 cellPos;
for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
  vec2 nb = vec2(i,j);
  vec2 pt = nb + vec2(hash(gi+nb), hash(gi+nb+7.3)) * jitter;
  float d = length(pt - gf);
  if (d<d1) { d2=d1; d1=d; cellId=gi+nb; cellPos=pt; } else if (d<d2) d2=d;
}
vec3 cellCol = mix(jewelPalette(hash(cellId)), IMG_NORM_PIXEL(inputTex,(cellId+0.5)/cellScale).rgb, texPerCell);
cellCol *= saturation * (0.5 + texture2D(audioFFT, vec2(hash(cellId)*0.8,0.5)).r*1.5);
float lead = smoothstep(leadWidth, 0.0, d2 - d1);
float ray = pow(max(0.0, dot(normalize(vec2(cos(lightAngle),sin(lightAngle))), uv-0.5+(mousePos-0.5)*0.3)), 4.0);
gl_FragColor = vec4(mix(vec3(0.0), cellCol, 1.0-lead) + ray*lightIntensity*audioBass, 1.0);
```

---

## 15. SMART_TRANSITION_KALEIDO  *(Transition)*
**Vibe:** Source A implodes through a kaleidoscope and reassembles as source B — a brief shattered, mirrored vortex between two video worlds.
**Reference:** The Pipilotti Rist circular split-screen; classic Pioneer DJ video transitions; the kaleidoscope sequence in *2001: A Space Odyssey*. Lives in Easel's `transitions/` directory next to `dissolve_noise.fs` and `wet_paint.fs`.
**Technique:** Follows the existing transition pattern: declares `from`, `to`, `progress` (0..1) inputs. Build a curve `s` from `progress` that rises sharply mid-transition: `s = sin(progress*π)` (peaks at progress=0.5). Apply kaleidoscope to UV: shift to centre, polar `(r,θ)`, fold θ into `slices` segments via `θ' = abs(mod(θ + slices_offset, 2π/slices) - π/slices)` — this gives mirror symmetry. Add radial zoom: `r' = r * (1.0 - s*zoomAmount)`. Convert back to cartesian, sample `from` at progress<0.5 mapping and `to` at progress>0.5 mapping. Crossfade weight: `mix(from_kaleido, to_kaleido, smoothstep(0.4, 0.6, progress))`. As `s` returns to 0 at progress=1, both sources reconverge to plain UV — `to` wins. Audio optional but supported: `audioBass` adds extra rotation kick.
**Audio mapping:** Optional. `audioBass` → momentary `slices_offset` rotation impulse (so the transition matches a beat-drop); other bands inactive (transitions should be deterministic by default).
**Texture treatment:** Both `from` and `to` are images; the shader is the bridge between them. `progress` is driven by Easel's transition system, not user-modulated.
**Pitfalls:** `slices` > 16 → kaleido too dense, content unreadable mid-transition. `zoomAmount` > 1 inverts UVs (negative radius). At progress=0 or 1 the output MUST be exact `from` / `to` respectively — verify by setting `s=0` clamps and ensuring identity sampling.
**INPUTS:**
```json
[
  {"NAME":"from","TYPE":"image"},
  {"NAME":"to","TYPE":"image"},
  {"NAME":"progress","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"slices","TYPE":"float","MIN":3.0,"MAX":16.0,"DEFAULT":8.0},
  {"NAME":"zoomAmount","TYPE":"float","MIN":0.0,"MAX":0.9,"DEFAULT":0.5},
  {"NAME":"swirl","TYPE":"float","MIN":0.0,"MAX":6.2832,"DEFAULT":1.5},
  {"NAME":"audioKick","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0}
]
```
**`main()` skeleton:**
```glsl
vec2 uv = isf_FragNormCoord;
float s = sin(progress * 3.14159265);                       // 0→1→0 envelope
vec2 p = uv - 0.5;
float r = length(p);
float th = atan(p.y, p.x) + swirl*s + audioBass*audioKick;
float seg = 6.2832 / slices;
th = abs(mod(th, seg) - seg*0.5);                           // mirror fold
float r2 = r * (1.0 - s*zoomAmount);
vec2 uv2 = vec2(cos(th), sin(th)) * r2 + 0.5;
vec4 a = IMG_NORM_PIXEL(from, mix(uv, uv2, s));
vec4 b = IMG_NORM_PIXEL(to,   mix(uv, uv2, s));
gl_FragColor = mix(a, b, smoothstep(0.4, 0.6, progress));
```

---

## Cross-cutting implementation notes

- **Helpers to share** (put in a `_lib.glsl` include or inline per shader):
  `hash(vec2)`, `vnoise`, `fbm`, `hsv2rgb`, `firePalette`, `jewelPalette`,
  `sectorColor`, `ringPalette`. Keeps shaders compact and visually consistent.
- **Audio idle floor**: every audio-driven shader should `max(audioX, 0.05)` so a
  silent room still has gentle motion.
- **Projection safety**: high-contrast / high-frequency shaders (Riley, Hologram,
  Cavern) should expose a `safeMode` bool that caps `freq`/`scanFreq` to avoid
  stroboscopic seizure-risk frequencies (>3Hz at full contrast).
- **Texture fallback**: where `inputTex` is unbound, host typically returns a
  1x1 black sampler. Guard with `useTex` bool or detect via a sentinel sample
  to avoid the shader going dark when no source is plugged in.
- **Order of creation suggested**:
  1. Helpers + Turrell (simplest, validates color pipeline)
  2. Memphis, Op-Art (procedural, no inputTex deps)
  3. Proximity, Particle Grid, Octagon (audio-coupled core)
  4. Liquid Ripples, Chladni, Solar (heavier math)
  5. Stained Glass, Cavern, Hologram, Dali (inputTex-heavy)
  6. TEXT_3D_AURORA (extends existing text shader — touch last)
  7. Kaleido transition (separate dir, separate pattern)
