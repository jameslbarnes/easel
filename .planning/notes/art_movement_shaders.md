# Art-Movement Shader Specs for Easel

15 ISF shader specs, one per major art movement of the last ~125 years (chronological).
Format mirrors `15_shader_specs.md`. Audience: senior C++/GLSL implementer batching
2–3 shaders per week. Each spec assumes the same built-ins (`RENDERSIZE`, `TIME`,
`audioBass`, `audioMid`, `audioHigh`, `audioLevel`, `audioFFT`, `inputTex`, `mousePos`).

Three movements from the brief are dropped because Easel already ships shaders that
cover them — **POSTMODERN_MEMPHIS** (covered by `MEMPHIS_PRIMITIVES`),
**OP_ART/Riley** (covered by `OPART_RILEY_WAVES`) is replaced with a fresh **Vasarely
Vega bulge** spec, and **SURREALISM/Dalí** (covered by `DALI_MELT`) is replaced with a
**Magritte** spec.

Order:
1. ART_NOUVEAU_MUCHA  (~1895–1910)
2. FAUVISM_MATISSE  (~1905–1908)
3. EXPRESSIONISM_KIRCHNER  (~1905–1925)
4. CUBISM_BRAQUE  (~1907–1914)
5. FUTURISM_BOCCIONI  (~1909–1916)
6. CONSTRUCTIVISM_LISSITZKY  (~1915–1930)
7. DADA_HOCH  (~1916–1924)
8. DESTIJL_MONDRIAN  (~1917–1931)
9. BAUHAUS_KANDINSKY  (~1919–1933)
10. SURREALISM_MAGRITTE  (~1924–1966)
11. ABSTRACT_EXPRESSIONISM_POLLOCK  (~1947–1950)
12. ABSTRACT_EXPRESSIONISM_ROTHKO  (~1950–1965) *bonus colour-field spec*
13. OPART_VASARELY  (~1965, Vega series)
14. POPART_LICHTENSTEIN  (~1962–1970)
15. MINIMALISM_STELLA  (~1959–1965)
16. VAPORWAVE_FLORAL_SHOPPE  (2010–2017)
17. GLITCH_DATAMOSH  (~2000–now)
18. AI_LATENT_DRIFT  (2022–now)

That's 18 specs total, including a Rothko colour-field bonus inside Abstract
Expressionism (since Pollock and Rothko represent the two distinct halves of the
movement and a single shader can't honestly do both). The user can drop the bonus
if they want exactly 15 — but I'd ship Rothko, it's the easiest of the lot.

---

## 1. ART_NOUVEAU_MUCHA

**Vibe:** Sinuous whiplash tendrils of hair and vines coil over a pastel field, framing
an implied oval portrait that breathes with audio. Decorative, ornamental, never
geometric.

**Reference:** Alphonse Mucha's 1896 *Job* poster; *The Four Seasons* (1896) lithograph
series; Sarah Bernhardt's 1894 *Gismonda* poster. The whiplash line as defined by V&A
is "an asymmetrical, sinuous S-curve mimicking vine and tendril motion." Hair-as-tendril
is the central motif.

**Technique:** Generate a field of N (4–8) parametric S-curves: each curve is
`y(t) = A·sin(ωt + φ) + B·sin(2ωt + ψ)` traced through the canvas as a thick stroke.
Distance-to-curve gives a smoothstep mask. Curves' amplitude and phase modulate slowly
(LFO at 0.1 Hz). Behind them, a soft pastel radial gradient (cream → dusty rose → sage)
fills the background. An *implied oval frame* — `step(ellipse(uv), 1.0)` — clips the
hair so it spills outside the frame at the top, classic Mucha decorative composition.
Gold accent strokes (every 3rd curve) get an extra outer stroke at 1.5× thickness in
muted gold (`vec3(0.78, 0.65, 0.32)`).

**Audio mapping:** `audioBass` → curve amplitude (the hair flows); `audioMid` → phase
drift speed; `audioHigh` → secondary overtone amplitude (fine wisps); `audioLevel` →
overall stroke brightness.

**Texture treatment:** `inputTex` optional — when present, sampled inside the oval
frame as the "portrait" the hair surrounds. Without it, fill the oval with a soft
sepia gradient.

**Pitfalls:** Curve count > 10 reads as noise, not ornament. Stroke thickness must be
generous (4–8 px equivalent) or it looks like wireframe. Do not let `B` (overtone
amp) exceed `A` — the line stops being whiplash and becomes squiggle. Background
must stay desaturated; saturated background fights the curves.

**INPUTS:**
```json
[
  {"NAME":"curveCount","TYPE":"float","MIN":3.0,"MAX":10.0,"DEFAULT":6.0},
  {"NAME":"curveAmp","TYPE":"float","MIN":0.05,"MAX":0.4,"DEFAULT":0.18},
  {"NAME":"strokeWidth","TYPE":"float","MIN":0.002,"MAX":0.02,"DEFAULT":0.008},
  {"NAME":"flowSpeed","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.15},
  {"NAME":"goldAccent","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"frameOval","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.7},
  {"NAME":"bgWarmth","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec3 bg = mix(vec3(0.96,0.93,0.85), vec3(0.92,0.78,0.78), uv.y) * (0.7 + bgWarmth*0.3);
vec3 col = bg;
float oval = smoothstep(1.0, 0.95, length((uv-vec2(0.5,0.45))/vec2(0.32,0.42)));
col = mix(col, IMG_NORM_PIXEL(inputTex, uv).rgb, oval * frameOval * 0.5);
for (int i=0; i<10; ++i) {
  if (float(i) >= curveCount) break;
  float ph = float(i)*1.7 + TIME*flowSpeed*(1.0+audioMid);
  float A = curveAmp * (1.0 + audioBass*0.5);
  float y = 0.5 + A*sin(uv.x*6.28 + ph) + 0.4*A*sin(uv.x*12.56 + ph*1.3)*audioHigh;
  float d = abs(uv.y - y);
  float stroke = smoothstep(strokeWidth, 0.0, d);
  vec3 sc = (mod(float(i),3.0)<0.5) ? mix(vec3(0.78,0.65,0.32), vec3(1.0), goldAccent*0.3)
                                    : vec3(0.15,0.08,0.05);
  col = mix(col, sc, stroke);
}
gl_FragColor = vec4(col*(0.9+audioLevel*0.1), 1.0);
```

---

## 2. FAUVISM_MATISSE

**Vibe:** Pure unmodulated colour blocks slammed against each other — hot pink water,
orange masts, lime sky. No shading, no atmosphere. Joy as a colour wheel.

**Reference:** Henri Matisse, *Open Window, Collioure* (1905); André Derain's
*Charing Cross Bridge* (1906). Critic Louis Vauxcelles called the group *fauves*
("wild beasts") at the 1905 Salon d'Automne; Matisse: "When I put a green, it is
not grass. When I put a blue, it is not the sky."

**Technique:** Voronoi-style large-cell partition (5–9 cells across the canvas) with
*irregular* cell shapes — use jittered Voronoi with high jitter so cells look
hand-painted, not geometric. Each cell is filled with a flat saturated colour from a
hand-tuned 8-stop fauvist palette (hot pink `#E8538A`, sap green `#5DA34A`, vermilion
`#E64A2C`, cobalt `#2D5BA3`, lemon `#F4D041`, mauve `#9B5DA0`, ultra-blue `#1E3F8A`,
paper-white `#F2EBD9`). Brushwork is faked by adding a slight per-pixel noise inside
each cell (`hash(uv*200)*0.04`) so the colours don't read as digital flats. No outlines,
no shading, no anti-aliasing inside cells — only at cell boundaries (gentle 1-px
smoothstep).

**Audio mapping:** `audioBass` → palette rotation (cells re-pick colour from palette
on big beats); `audioMid` → cell jitter (cells deform); `audioHigh` → noise grain
amplitude (more brushwork energy); `audioLevel` → overall saturation.

**Texture treatment:** `inputTex` optional. When present, each cell samples its
*centre pixel* from `inputTex` then *snaps it to the nearest fauvist palette
colour* (8-way snap). The result: live video reduced to a 5–8-cell fauvist
abstraction.

**Pitfalls:** Cell count < 4 reads as colour-blocks, not Fauvism. Cell count > 12
reads as Voronoi mosaic. Saturation under 0.85 makes it look like Cézanne, not
Matisse — keep colours hot. Anti-aliased gradients inside cells kill the flatness;
use hard `step()` fills.

**INPUTS:**
```json
[
  {"NAME":"cellScale","TYPE":"float","MIN":3.0,"MAX":10.0,"DEFAULT":6.0},
  {"NAME":"jitter","TYPE":"float","MIN":0.4,"MAX":1.0,"DEFAULT":0.85},
  {"NAME":"saturation","TYPE":"float","MIN":0.6,"MAX":1.4,"DEFAULT":1.1},
  {"NAME":"brushNoise","TYPE":"float","MIN":0.0,"MAX":0.08,"DEFAULT":0.025},
  {"NAME":"snapToPalette","TYPE":"bool","DEFAULT":true},
  {"NAME":"paletteShift","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"audioReact","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 g = uv * cellScale;
vec2 gi = floor(g), gf = fract(g);
float d1=1e9; vec2 cellId; vec2 cellPos;
for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
  vec2 nb = vec2(i,j);
  vec2 pt = nb + vec2(hash(gi+nb), hash(gi+nb+9.1)) * jitter * (1.0+audioMid*audioReact*0.3);
  float d = length(pt - gf);
  if (d<d1) { d1=d; cellId=gi+nb; cellPos=pt; }
}
float seed = hash(cellId + floor(audioBass*audioReact*4.0)*paletteShift);
vec3 base = fauvistPalette(seed);
if (snapToPalette) base = snapToFauvistPalette(IMG_NORM_PIXEL(inputTex,(cellId+0.5)/cellScale).rgb);
base += (hash(uv*200.0)-0.5) * brushNoise * (1.0+audioHigh*audioReact);
gl_FragColor = vec4(saturate(base, saturation), 1.0);
```

---

## 3. EXPRESSIONISM_KIRCHNER

**Vibe:** A jagged, smeared world seen through nervous breakdown — angular brushstrokes
in unnatural acid colours, distorted forms, off-kilter perspective. Berlin street at
night.

**Reference:** Ernst Ludwig Kirchner, *Street, Berlin* (1913), *Potsdamer Platz* (1914);
Edvard Munch, *The Scream* (1893). Die Brücke aesthetic — distorted figures,
psychologically charged colour, rough hewn brushwork. Yale's *Munch and Kirchner*
exhibit captures the lineage.

**Technique:** Take `inputTex` (or fallback gradient) and apply: (1) a *jagged warp*
— displace UV by `jaggedNoise(uv*scale + TIME*flow)`, where `jaggedNoise` is fbm of
*Worley* noise (cellular, sharp-edged, not smooth Perlin); (2) palette-shift toward
acid colours — multiply RGB by an unnatural tint LUT (acid green, magenta, sulphur
yellow, cobalt); (3) overlay diagonal "brushstroke" streaks — sample input along a
line offset perpendicular to a noise-driven angle, so smears appear in the direction of
panic; (4) heavy contrast curve `pow(col, 0.7)` then `(col-0.5)*1.4+0.5` to flatten
mid-tones into stark light/dark; (5) optional thick black ink lines at high-gradient
edges (`length(dFdx(luma) + dFdy(luma)) > thresh`).

**Audio mapping:** `audioBass` → jagged warp amplitude; `audioMid` → ink-line
threshold (more bass = more outlines); `audioHigh` → palette-acid intensity;
`audioLevel` → smear length.

**Texture treatment:** `inputTex` is the content being expressionistically distorted.
Without it, render a procedural cobblestone-street gradient.

**Pitfalls:** Smooth (Perlin) noise yields a watercolour, not Kirchner — must be
Worley/cellular for the jagged edge. Contrast curve over-cranked turns it
monochrome. Acid palette over-applied looks like a bad 90s filter; keep tint
strength ≤ 0.6.

**INPUTS:**
```json
[
  {"NAME":"jaggedAmp","TYPE":"float","MIN":0.0,"MAX":0.15,"DEFAULT":0.05},
  {"NAME":"jaggedScale","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":5.0},
  {"NAME":"smearLength","TYPE":"float","MIN":0.0,"MAX":0.05,"DEFAULT":0.015},
  {"NAME":"acidTint","TYPE":"float","MIN":0.0,"MAX":0.8,"DEFAULT":0.4},
  {"NAME":"contrast","TYPE":"float","MIN":1.0,"MAX":2.0,"DEFAULT":1.4},
  {"NAME":"inkLines","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"flow","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.2},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 jw = vec2(worleyFbm(uv*jaggedScale + TIME*flow),
               worleyFbm(uv*jaggedScale + 7.3 - TIME*flow));
vec2 src = uv + (jw-0.5) * jaggedAmp * (1.0+audioBass);
vec3 a = IMG_NORM_PIXEL(inputTex, src).rgb;
vec3 b = IMG_NORM_PIXEL(inputTex, src + vec2(smearLength*sin(jw.x*6.0),0)).rgb;
vec3 col = mix(a, b, 0.5);
vec3 acid = vec3(0.6,1.4,0.5)*0.5 + vec3(1.3,0.6,1.4)*0.5;
col = mix(col, col*acid, acidTint*(0.5+audioHigh*0.5));
col = (col-0.5)*contrast + 0.5;
float edge = length(vec2(dFdx(dot(col,vec3(0.33))), dFdy(dot(col,vec3(0.33)))));
col = mix(col, vec3(0.05), smoothstep(0.05, 0.15, edge*audioMid)*inkLines);
gl_FragColor = vec4(col, 1.0);
```

---

## 4. CUBISM_BRAQUE

**Vibe:** The image is shattered into faceted planes seen from many angles
simultaneously, recomposed in earth tones. Analytic cubism — sober, monochrome,
fragmented.

**Reference:** Georges Braque, *Houses at L'Estaque* (1908), *Violin and Candlestick*
(1910); Picasso, *Portrait of Daniel-Henry Kahnweiler* (1910). Picked Braque rather
than Picasso because Braque's *analytic* phase is more shader-tractable. Shahriar
Shahrabi's *Real-time Cubism Shader* (Medium, 2020) is the implementation reference —
"each fragment of the cubist painting is shaded in a different way with different
colors... divide the space into segments and alter the MVP matrix per segment."

**Technique:** Partition canvas into 12–24 irregular polygonal facets via jittered
Voronoi. For each facet, choose an *independent UV transform*: a rotation by
`hash(cellId)*π`, a small scale `0.7 + hash*0.5`, and a translation. Sample
`inputTex` through that per-facet transform. Result: each facet shows the same input
from a different pretend-perspective, simulating multi-viewpoint cubism. Pass the result
through a desaturating/sepia LUT (Braque's analytic palette is ochre, umber, slate
grey, with rare warm white). Add hairline black outlines at facet boundaries
(`smoothstep(0.005, 0, d2-d1)`). Letters and tiny numbers (a Braque signature
move — fragments of newspaper) optional via a sparse text overlay.

**Audio mapping:** `audioBass` → per-facet transform magnitude (cells reframe their
samples on beats); `audioMid` → outline thickness; `audioHigh` → palette-warmth
shift (more treble pushes ochres warmer); `audioLevel` → facet rotation drift.

**Texture treatment:** `inputTex` is the *subject* — fed through 12–24 simultaneous
faux-perspectives. Without it, the shader is dull; design assumes it's bound.

**Pitfalls:** Per-facet transform too aggressive (rotation > π/2) makes facets show
unrelated content and kills cohesion. Saturation off-by-one — Braque's analytic
phase is *near* monochrome; tune saturation to ~0.3, not 1.0. Without text
fragments the result is just "Voronoi'd photo" — adding 3-4 sparse glyph stamps at
hashed positions is what makes it read Cubist.

**INPUTS:**
```json
[
  {"NAME":"facetScale","TYPE":"float","MIN":3.0,"MAX":12.0,"DEFAULT":7.0},
  {"NAME":"jitter","TYPE":"float","MIN":0.5,"MAX":1.0,"DEFAULT":0.85},
  {"NAME":"facetRotMax","TYPE":"float","MIN":0.0,"MAX":1.5,"DEFAULT":0.6},
  {"NAME":"outlineWidth","TYPE":"float","MIN":0.0,"MAX":0.02,"DEFAULT":0.004},
  {"NAME":"sepiaStrength","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.7},
  {"NAME":"textFragments","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"audioReact","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 g = uv * facetScale;
vec2 gi = floor(g), gf = fract(g);
float d1=1e9,d2=1e9; vec2 cellId;
for (int j=-1;j<=1;j++) for (int i=-1;i<=1;i++) {
  vec2 nb = vec2(i,j);
  vec2 pt = nb + vec2(hash(gi+nb), hash(gi+nb+5.7))*jitter;
  float d = length(pt - gf);
  if (d<d1){d2=d1;d1=d;cellId=gi+nb;} else if (d<d2) d2=d;
}
float r = (hash(cellId)*2.0-1.0) * facetRotMax * (1.0+audioBass*audioReact);
mat2 R = mat2(cos(r),-sin(r),sin(r),cos(r));
vec2 ctr = (cellId+0.5)/facetScale;
vec2 sUV = ctr + R*(uv-ctr) * (0.7 + hash(cellId+1.3)*0.5);
vec3 col = IMG_NORM_PIXEL(inputTex, sUV).rgb;
col = mix(col, sepia(col), sepiaStrength);
col = mix(col, vec3(0.05), smoothstep(outlineWidth, 0.0, d2-d1));
col = mix(col, vec3(0.9), textGlyphField(uv, cellId)*textFragments);
gl_FragColor = vec4(col, 1.0);
```

---

## 5. FUTURISM_BOCCIONI

**Vibe:** A figure or shape blasted through space, repeated in echoing trails of
motion-lines and radiating force-vectors. Speed as visual concept.

**Reference:** Umberto Boccioni, *Dynamism of a Cyclist* (1913), *Unique Forms of
Continuity in Space* (sculpture, 1913); Giacomo Balla's *Dog on a Leash* (1912).
Boccioni's "force lines" — his *linee-forza* — are the grammar.

**Technique:** Take `inputTex` as the subject, sample it N times along a *velocity
vector* (driven by `mousePos` or `audioBass`), each tap with decreasing opacity →
classic motion-blur trail. *On top* of that, render radiating "force lines" from
the centre of mass: cast 12–24 rays out from a hashed origin, each ray is a thick
streak `smoothstep(rayWidth, 0, distToRay)`, rays' thickness and extent driven by
audio. Underlay a divisionist-dot brushwork field (Boccioni used divisionist
technique) by adding small bright dots at hash positions perpendicular to the
velocity direction. Final colour grade: warm earth + accent vermilion in the
force-line direction.

**Audio mapping:** `audioBass` → trail length (number of motion-blur taps);
`audioMid` → force-ray brightness; `audioHigh` → divisionist-dot density;
`audioLevel` → velocity vector magnitude.

**Texture treatment:** `inputTex` is the moving subject. Without it, render a
silhouetted abstract triangle/blob and treat *that* as the moving body — the
shader still works as a generator.

**Pitfalls:** Too many trail taps (>12) eats fillrate. Force lines need to *radiate*
from a *moving* origin (otherwise it looks like a star-burst, not motion); the
origin must drift with audio. Without the divisionist undertone, the shader looks
like a Photoshop motion blur — the dots are what make it Boccioni.

**INPUTS:**
```json
[
  {"NAME":"trailSamples","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":7.0},
  {"NAME":"velocity","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.12},
  {"NAME":"velocityAngle","TYPE":"float","MIN":0.0,"MAX":6.2832,"DEFAULT":0.4},
  {"NAME":"forceRays","TYPE":"float","MIN":4.0,"MAX":24.0,"DEFAULT":14.0},
  {"NAME":"rayBrightness","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.8},
  {"NAME":"divisionistDots","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"warmth","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy / RENDERSIZE.xy;
vec2 vel = vec2(cos(velocityAngle),sin(velocityAngle)) * velocity * (1.0+audioLevel);
vec3 col = vec3(0.0); float wsum = 0.0;
for (int i=0;i<12;++i){ if (float(i)>=trailSamples) break;
  float w = 1.0 - float(i)/trailSamples;
  col += IMG_NORM_PIXEL(inputTex, uv - vel*float(i)/trailSamples).rgb * w;
  wsum += w;
}
col /= wsum;
vec2 origin = vec2(0.5) + vel*0.5;
float th = atan(uv.y-origin.y, uv.x-origin.x);
float rays = pow(abs(sin(th*forceRays*0.5)), 12.0);
col += rays * vec3(1.0,0.4,0.2) * rayBrightness * audioMid;
col += divisionistField(uv, vel) * divisionistDots * audioHigh;
col = mix(col, col*vec3(1.1,0.95,0.8), warmth);
gl_FragColor = vec4(col, 1.0);
```

---

## 6. CONSTRUCTIVISM_LISSITZKY

**Vibe:** A red wedge slamming into a white circle on a stark cream field. Diagonal
typography, geometric forces in collision. Revolutionary poster as moving image.

**Reference:** El Lissitzky, *Beat the Whites with the Red Wedge* (1919); Alexander
Rodchenko's *Books! In all branches of knowledge* poster (1924); Varvara Stepanova's
fabric designs. Wikipedia and DailyArt treat *Beat the Whites* as the canonical
example: red triangle piercing white circle on diagonal axis.

**Technique:** Procedural composition of geometric primitives on a cream background:
(1) one large red triangle (vertex pointing into the canvas at a 30° upward
diagonal), (2) one large white circle, (3) 2–4 smaller black bars at varying
diagonals, (4) sparse Cyrillic-looking glyph clusters (procedural rectangles
forming letters) at three positions. Each primitive's transform animates: triangle
*pierces forward* (translates along its axis, depth=0..1), circle *recedes* (scale
shrinks), bars rotate slightly. On audio bass impact, triangle apex jumps forward.
Hard edges everywhere — `step()` not `smoothstep()`. Palette is fixed:
`#E31E24` (constructivist red), `#F5EBD0` (cream paper), `#1A1A1A` (black ink),
`#FFFFFF` (white).

**Audio mapping:** `audioBass` → triangle thrust forward (translation along axis);
`audioMid` → bar rotation drift; `audioHigh` → glyph flicker; `audioLevel` →
small composition jitter (printing-press shake).

**Texture treatment:** `inputTex` optional — when bound, replaces the *circle's
fill* with a desaturated, high-contrast version of the input. The triangle and
bars stay flat red/black. Lets a face, logo or abstract feed live inside the
"white circle being beaten."

**Pitfalls:** Anti-aliased edges destroy the lithograph feel — keep `step()` hard
or use ≤1px smoothstep. Too much animation breaks the poster gravitas; keep
amplitudes modest (translation < 0.1 of canvas). More than ~6 primitives
becomes Bauhaus, not Constructivism — restraint matters.

**INPUTS:**
```json
[
  {"NAME":"triangleThrust","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.1},
  {"NAME":"triangleAngle","TYPE":"float","MIN":0.0,"MAX":1.5,"DEFAULT":0.55},
  {"NAME":"circleSize","TYPE":"float","MIN":0.15,"MAX":0.4,"DEFAULT":0.25},
  {"NAME":"barCount","TYPE":"float","MIN":0.0,"MAX":4.0,"DEFAULT":2.0},
  {"NAME":"glyphIntensity","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"compositionJitter","TYPE":"float","MIN":0.0,"MAX":0.02,"DEFAULT":0.005},
  {"NAME":"useTex","TYPE":"bool","DEFAULT":false},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
uv += (hash(vec2(floor(TIME*8.0)))-0.5)*compositionJitter*audioLevel;
vec3 col = vec3(0.96,0.92,0.82);
vec2 c = vec2(0.55,0.45);
float circ = step(length(uv-c), circleSize*(1.0-audioBass*0.05));
vec3 circCol = useTex ? hardContrast(IMG_NORM_PIXEL(inputTex,uv).rgb) : vec3(1.0);
col = mix(col, circCol, circ);
vec2 ta = vec2(cos(triangleAngle), sin(triangleAngle));
vec2 t0 = c - ta*0.05 - ta*triangleThrust*audioBass;
float tri = triangle(uv, t0, ta, 0.35);
col = mix(col, vec3(0.89,0.12,0.14), tri);
for (int i=0; i<4; ++i){ if (float(i)>=barCount) break;
  col = mix(col, vec3(0.1), barShape(uv, i, audioMid));
}
col = mix(col, vec3(0.05), glyphField(uv)*glyphIntensity*(0.8+audioHigh*0.4));
gl_FragColor = vec4(col, 1.0);
```

---

## 7. DADA_HOCH

**Vibe:** Chaotic photomontage — fragments of input video hand-cut and pasted at random
angles, with black-and-white newspaper clippings, hand-stamped letters, bits of
machinery. Anti-art, anti-composition, joyful destruction.

**Reference:** Hannah Höch, *Cut with the Dada Kitchen Knife through the Last Weimar
Beer-Belly Cultural Epoch in Germany* (1919–1920); Raoul Hausmann; Kurt Schwitters'
*Merz* collages. Höch and Hausmann co-invented photomontage as *anti-art* — using
mass-media reproductions and pure scissor-and-glue technique.

**Technique:** Divide canvas into ~10–20 *irregular rectangular patches* (different
sizes, hashed positions and rotations, occasional overlaps). Each patch samples
`inputTex` from a *different* random region of the input (sample at
`hash(patchId)*2.0` UV — wraps), at a different scale, sometimes desaturated,
sometimes inverted, sometimes with paper-grain-noise overlay. Behind/around patches:
a beige paper-collage background with subtle paper texture. Random "stamped"
marks — circles, crosses, stencilled numerals — at sparse hash positions, in red
ink. The patches *jitter slightly* every few frames (re-rolling positions on bass
hits) — the collage rearranges itself.

**Audio mapping:** `audioBass` → patch re-roll (positions resample on transients);
`audioMid` → patch rotation drift; `audioHigh` → stamp/mark density;
`audioLevel` → patch desaturation (loud → more colour, quiet → newspaper-grey).

**Texture treatment:** `inputTex` is the *raw material* — Höch literally cut from
illustrated magazines; here we cut from live video. Each patch is an
independently sampled, transformed window into the input.

**Pitfalls:** Without overlaps it doesn't read as collage. Without rotation it reads
as grid. Without paper-noise + occasional desaturation it looks too clean (Höch's
sources were halftone-printed magazines — a tiny halftone dot pattern overlay
sells the era). Patch count > 25 turns into mosaic — keep ~15.

**INPUTS:**
```json
[
  {"NAME":"patchCount","TYPE":"float","MIN":6.0,"MAX":25.0,"DEFAULT":14.0},
  {"NAME":"patchScale","TYPE":"float","MIN":0.05,"MAX":0.3,"DEFAULT":0.15},
  {"NAME":"rotationRange","TYPE":"float","MIN":0.0,"MAX":0.6,"DEFAULT":0.3},
  {"NAME":"rerollSpeed","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":0.5},
  {"NAME":"stampDensity","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"halftoneAmount","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"paperWarmth","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.7},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
float seed = floor(TIME*rerollSpeed) + floor(audioBass*4.0);
vec3 col = vec3(0.93,0.88,0.78) * (0.7+paperWarmth*0.3);
col += paperGrain(uv)*0.05;
for (int i=0; i<25; ++i){ if (float(i)>=patchCount) break;
  vec2 c  = vec2(hash(vec2(float(i),seed)), hash(vec2(float(i)+0.7,seed)));
  float r = (hash(vec2(float(i),seed+1.3))-0.5)*rotationRange + audioMid*0.05;
  float s = patchScale * (0.5+hash(vec2(float(i),seed+2.1)));
  vec2 q = uv - c; q = mat2(cos(r),-sin(r),sin(r),cos(r))*q;
  if (abs(q.x)<s && abs(q.y)<s*0.7){
    vec2 sUV = fract((q/s)*0.5+0.5 + vec2(hash(vec2(i,seed+3.0))*2.0));
    vec3 patch = IMG_NORM_PIXEL(inputTex, sUV).rgb;
    if (hash(vec2(i,seed+4.0))<0.4) patch = vec3(dot(patch,vec3(0.33)));
    patch = mix(patch, halftone(patch, q*100.0), halftoneAmount);
    col = patch;
  }
}
col = mix(col, vec3(0.85,0.1,0.1), stampField(uv)*stampDensity*audioHigh);
gl_FragColor = vec4(col, 1.0);
```

---

## 8. DESTIJL_MONDRIAN

**Vibe:** A field of black-edged white rectangles with a few primary-coloured cells —
red, blue, yellow — in asymmetric balance. Pulsates gently as if the grid is
breathing in 4/4.

**Reference:** Piet Mondrian, *Composition with Red, Blue and Yellow* (1930);
*Broadway Boogie Woogie* (1942–43) — the *late* grids, post-tree-period. Wikipedia
and Smarthistory both note: thick black borders, only red/blue/yellow as accents on
a white field, asymmetrical balance with one large dominant block. Picking *Broadway
Boogie Woogie* as the late reference because its pulsating-grid energy maps better
to audio than the static earlier compositions.

**Technique:** Recursive rectangular subdivision (binary splits) of canvas into 8–20
cells. Cell aspect ratios biased toward squares and tall/wide rectangles, never
narrow. ~70% of cells fill white, ~25% fill `vec3(0.96,0.94,0.9)` (off-white), 5%
fill primary red/blue/yellow chosen so total areas roughly balance (one dominant
red, smaller blue, smallest yellow). Black grid lines between cells of fixed
hairline width. *Boogie-Woogie variant*: instead of solid colour fills, primary
cells have small *moving coloured dots* that march along the cell edge —
this is the late pulsation Mondrian discovered in NYC. Bass triggers a subdivision
re-roll.

**Audio mapping:** `audioBass` → grid re-roll (cells redivide on big beats);
`audioMid` → dot-march speed (Boogie Woogie pulse); `audioHigh` → grid-line
flicker (subtle); `audioLevel` → primary-cell saturation.

**Texture treatment:** `inputTex` optional. When bound, *only one cell* (the dominant
red cell) shows the input, mapped through a hard 3-colour quantize to red/black/white.
Lets video bleed through the composition without breaking the Mondrian read.

**Pitfalls:** Rounded corners destroy it — must be sharp 90° corners. Anti-aliased
grid lines look fine; aliased look bad. Too many primary cells and it becomes
"colourful grid"; keep primaries to 3–4 maximum (one per primary). Grid lines
must be a *constant* pixel width across all zoom levels — compute in pixel
space, not UV space.

**INPUTS:**
```json
[
  {"NAME":"subdivisionDepth","TYPE":"float","MIN":2.0,"MAX":6.0,"DEFAULT":4.0},
  {"NAME":"lineWidth","TYPE":"float","MIN":0.002,"MAX":0.015,"DEFAULT":0.006},
  {"NAME":"redArea","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.18},
  {"NAME":"blueArea","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.08},
  {"NAME":"yellowArea","TYPE":"float","MIN":0.0,"MAX":0.2,"DEFAULT":0.04},
  {"NAME":"boogieWoogie","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.6},
  {"NAME":"useTex","TYPE":"bool","DEFAULT":false},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
float seed = floor(audioBass*4.0);
RectCell cell = recursiveSubdiv(uv, int(subdivisionDepth), seed);
vec3 col = vec3(1.0);
float colorRoll = hash(cell.id);
if (colorRoll < redArea)            col = vec3(0.89,0.12,0.14);
else if (colorRoll < redArea+blueArea)   col = vec3(0.10,0.18,0.65);
else if (colorRoll < redArea+blueArea+yellowArea) col = vec3(0.97,0.85,0.10);
else if (colorRoll < 0.65)          col = vec3(0.96,0.94,0.9);
if (useTex && cell.isDominantRed) col = quantizeMondrian(IMG_NORM_PIXEL(inputTex,uv).rgb);
float dotMarch = boogieWoogiePulse(uv, cell, TIME*audioMid);
col = mix(col, vec3(1.0)-col, dotMarch*boogieWoogie);
float line = gridLine(uv, cell, lineWidth);
col = mix(col, vec3(0.05), line);
gl_FragColor = vec4(col, 1.0);
```

---

## 9. BAUHAUS_KANDINSKY

**Vibe:** Geometric primaries — yellow triangle, red square, blue circle — floating in
balanced motion across a white field, with thin black supporting lines. Pure form,
pure colour, weightless.

**Reference:** Wassily Kandinsky's Bauhaus survey assignment of *yellow → triangle,
red → square, blue → circle* (verified by his own 1923 questionnaire); his works
*Yellow-Red-Blue* (1925), *Several Circles* (1926); Itten's colour wheel. The
mapping form↔primary is the spec's organising principle.

**Technique:** Spawn N (8–18) shape instances, each with random size, position
trajectory (Lissajous curve), and *forced* colour-shape pairing (Kandinsky's rule):
triangles always yellow, squares always red, circles always blue. Plus a few
black thin straight lines as supporting structure. Background pure white. Each
shape can rotate, scale, translate independently — they orbit and collide
without occlusion (use additive-then-clamp blend). On bass impact, shapes spring
outward briefly. Layout is *balanced* — sample positions along a latin-square
distribution rather than uniform random, so the canvas looks composed not random.

**Audio mapping:** `audioBass` → outward spring (shapes pulse away from centre);
`audioMid` → rotation speed; `audioHigh` → secondary line opacity; `audioLevel` →
overall scale of all shapes (the world breathes).

**Texture treatment:** `inputTex` optional. When bound, treat as a *colour-source*
— sample 3 specific pixels from `inputTex` and let those override the
yellow/red/blue palette. So a sunset photo could turn the Kandinsky composition
into orange/pink/violet while preserving the form-colour pairing.

**Pitfalls:** Breaking Kandinsky's mapping (e.g. red triangle) feels wrong even to
viewers who don't know the rule — keep it strict. Too many shapes (>20)
becomes confetti; under 6 looks empty. Black supporting lines must be *thin* —
Kandinsky's geometry is delicate, not heavy.

**INPUTS:**
```json
[
  {"NAME":"shapeCount","TYPE":"float","MIN":6.0,"MAX":20.0,"DEFAULT":12.0},
  {"NAME":"shapeScale","TYPE":"float","MIN":0.05,"MAX":0.2,"DEFAULT":0.1},
  {"NAME":"orbitSpeed","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"lineCount","TYPE":"float","MIN":0.0,"MAX":8.0,"DEFAULT":4.0},
  {"NAME":"springReact","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.12},
  {"NAME":"strictPairing","TYPE":"bool","DEFAULT":true},
  {"NAME":"useTexPalette","TYPE":"bool","DEFAULT":false},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 col = vec3(1.0);
vec3 yellow = useTexPalette ? IMG_NORM_PIXEL(inputTex,vec2(0.2,0.5)).rgb : vec3(0.98,0.85,0.10);
vec3 red    = useTexPalette ? IMG_NORM_PIXEL(inputTex,vec2(0.5,0.5)).rgb : vec3(0.89,0.12,0.14);
vec3 blue   = useTexPalette ? IMG_NORM_PIXEL(inputTex,vec2(0.8,0.5)).rgb : vec3(0.10,0.18,0.65);
for (int i=0; i<20; ++i){ if (float(i)>=shapeCount) break;
  vec2 cen = lissajousLatin(i, TIME*orbitSpeed);
  cen += normalize(cen-0.5)*springReact*audioBass;
  float s  = shapeScale * (0.7+hash(vec2(float(i),0.3))*0.6) * (1.0+audioLevel*0.1);
  int shapeType = i % 3;   // 0=triangle, 1=square, 2=circle
  vec3 shapeCol = (shapeType==0)?yellow : (shapeType==1)?red : blue;
  float m = drawShape(uv-cen, s, shapeType, TIME*audioMid);
  col = mix(col, shapeCol, m);
}
for (int j=0; j<8; ++j){ if (float(j)>=lineCount) break;
  col = mix(col, vec3(0.05), supportLine(uv, j, audioHigh)*0.6);
}
gl_FragColor = vec4(col, 1.0);
```

---

## 10. SURREALISM_MAGRITTE

**Vibe:** Daylight blue sky, perfectly painted clouds, and an impossible object —
a rock, a bowler hat, a green apple — floating where it shouldn't. Photographic
realism in the impossible.

**Reference:** René Magritte, *The Castle of the Pyrenees* (1959) — a rock with a
castle on top, floating above a stormy sea; *The Son of Man* (1964); *The Empire of
Light* series. Magritte's style is *deadpan illustration*, not painterly — clean
edges, soft graduated sky, no gestural brushwork. The horror/wonder comes from
the impossibility, not the technique.

**Technique:** Three layers. (1) **Sky**: a smooth vertical gradient from horizon
cream-yellow to high cobalt blue, with several *Magritte clouds* — fluffy,
illustrative, drawn via fbm-of-fbm with a sharp threshold and slight
self-shadowing on the underside. Clouds drift slowly. (2) **Sea or ground**: a
darker gradient at the bottom, water using subtle noise as wave texture. (3)
**Floating object**: a pre-defined silhouette (apple-shape, rock-shape, hat-shape —
selectable by `objectChoice`) rendered via SDF, hovering with slow vertical
oscillation. The object's *internal lighting* is consistent with sky direction so
it looks photographically composited (left-bright, right-shadow). On audio, the
object can multiply or scatter — bass impact spawns ghost duplicates that fade.

**Audio mapping:** `audioBass` → ghost-duplicate spawn (Magritte's *Golconda* effect
where many bowlers fall from sky on big hits); `audioMid` → cloud drift speed;
`audioHigh` → object shimmer; `audioLevel` → vertical oscillation amplitude.

**Texture treatment:** `inputTex` optional — when bound, *paints onto the floating
object's silhouette* as if the live video were the surface of the apple/rock.
Outside the object, sky and ground are procedural.

**Pitfalls:** The clouds *cannot* be photoreal Perlin lumps — they need that
illustrative, slightly-too-symmetrical Magritte cloud shape. Threshold the noise
hard, soften only the silhouette edge. Object too large fills frame and breaks the
"in landscape" composition; keep it ≤ 30% of canvas. Ghost duplicates with no
fade-out look like a glitch — fade over 0.5s.

**INPUTS:**
```json
[
  {"NAME":"objectChoice","TYPE":"long","DEFAULT":0,"VALUES":[0,1,2],"LABELS":["apple","bowlerHat","castleRock"]},
  {"NAME":"objectSize","TYPE":"float","MIN":0.05,"MAX":0.3,"DEFAULT":0.15},
  {"NAME":"horizonY","TYPE":"float","MIN":0.3,"MAX":0.8,"DEFAULT":0.65},
  {"NAME":"cloudCoverage","TYPE":"float","MIN":0.0,"MAX":0.8,"DEFAULT":0.4},
  {"NAME":"cloudDrift","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.08},
  {"NAME":"hoverAmp","TYPE":"float","MIN":0.0,"MAX":0.1,"DEFAULT":0.025},
  {"NAME":"ghostMultiply","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 sky = mix(vec3(0.95,0.88,0.75), vec3(0.30,0.55,0.85), smoothstep(horizonY-0.1, 1.0, uv.y));
vec3 ground = mix(vec3(0.18,0.22,0.30), vec3(0.42,0.45,0.50), uv.y/horizonY);
vec3 col = (uv.y>horizonY) ? sky : ground;
float clouds = magritteClouds(uv, TIME*cloudDrift+audioMid*0.1);
col = mix(col, vec3(0.95), clouds*step(horizonY,uv.y)*cloudCoverage);
vec2 objPos = vec2(0.5, horizonY+0.1 + sin(TIME*0.3)*hoverAmp*(1.0+audioLevel));
float obj = objectSDF(uv-objPos, objectSize, int(objectChoice));
vec3 objCol = IMG_NORM_PIXEL(inputTex, (uv-objPos)/objectSize+0.5).rgb;
objCol *= 0.6 + 0.4*smoothstep(0.0, objectSize, objPos.x-uv.x);
col = mix(col, objCol, obj);
for (int g=0;g<5;++g){
  vec2 gp = objPos + ghostOffset(g, TIME);
  col = mix(col, objCol, objectSDF(uv-gp,objectSize,int(objectChoice))*ghostMultiply*audioBass);
}
gl_FragColor = vec4(col, 1.0);
```

---

## 11. ABSTRACT_EXPRESSIONISM_POLLOCK

**Vibe:** Drips, splatters, ribbons of paint laid down by a body in motion — chaotic
order, all-over composition, gestural. Layered black, white, ochre, deep red.

**Reference:** Jackson Pollock, *Autumn Rhythm: Number 30* (1950), *Number 1A* (1948),
*Blue Poles* (1952). Met Museum and AbExpressionist scholarship: Pollock's "controlled
chaos" — each gesture instinctive yet deliberate, recording the artist's body in
motion. Picking the *drip* phase (1947–1950), not the early figurative work.

**Technique:** Procedural drip-trajectory tracing. Spawn N (~8–24) "drip paths,"
each a parametric curve `pos(t) = base + curl(noise) * t` (curl-noise advection
gives convincingly liquid trajectories). For each fragment, compute distance to
the nearest drip-path point set; render thick lines along the paths in one of
4 colours (black, white, ochre, deep red) chosen per-path. Drip-tail thickness
varies with path-time (thicker near "where the artist's stick was"). Multiple
overlapping passes (5–8 layers, latest on top) build up the all-over composition.
Background is raw canvas off-white.

**Audio mapping:** `audioBass` → drip-spawn rate (new gestures appear on beats);
`audioMid` → curl-noise frequency (more turbulent paths); `audioHigh` →
splatter density (bright dots scattered along paths); `audioLevel` → overall
opacity / "wetness."

**Texture treatment:** `inputTex` optional. When bound, replaces the four-colour
palette: paths sample colour from `inputTex` at hashed positions. So a live
camera feeds the colours but Pollock's structure stays.

**Pitfalls:** Smooth Bezier curves don't drip — must use curl noise advection
to get the right wandering. Path count too low (<4) reads sparse; >25 reads as
mud. The four colours need *separation*: black for skeleton, white for
highlights, ochre as ground, red as accent — not 4 random colours. No
gradients inside a stroke; thick paint is *flat colour*.

**INPUTS:**
```json
[
  {"NAME":"pathCount","TYPE":"float","MIN":4.0,"MAX":24.0,"DEFAULT":12.0},
  {"NAME":"pathLength","TYPE":"float","MIN":0.5,"MAX":3.0,"DEFAULT":1.5},
  {"NAME":"strokeWidth","TYPE":"float","MIN":0.002,"MAX":0.015,"DEFAULT":0.006},
  {"NAME":"turbulence","TYPE":"float","MIN":0.5,"MAX":4.0,"DEFAULT":2.0},
  {"NAME":"splatterDensity","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"layerCount","TYPE":"float","MIN":2.0,"MAX":8.0,"DEFAULT":5.0},
  {"NAME":"useTexColor","TYPE":"bool","DEFAULT":false},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 col = vec3(0.92,0.88,0.78);
const vec3 PAL[4] = vec3[4](vec3(0.05),vec3(0.95),vec3(0.55,0.42,0.18),vec3(0.65,0.10,0.10));
for (int L=0; L<8; ++L){ if (float(L)>=layerCount) break;
  for (int i=0; i<24; ++i){ if (float(i)>=pathCount) break;
    int idx = L*24+i;
    vec2 base = vec2(hash(vec2(float(idx),0.1)), hash(vec2(float(idx),0.7)));
    vec2 dripPos = base + curlAdvect(base, TIME*0.1+float(idx), turbulence, pathLength);
    float d = distToPath(uv, base, dripPos, turbulence);
    float w = strokeWidth * (1.5-float(L)/layerCount);
    float m = smoothstep(w, 0.0, d);
    vec3 pc = useTexColor ? IMG_NORM_PIXEL(inputTex, base).rgb : PAL[idx&3];
    col = mix(col, pc, m*(0.6+audioLevel*0.4));
  }
}
col += splatter(uv, TIME, audioHigh)*splatterDensity;
gl_FragColor = vec4(col, 1.0);
```

---

## 12. ABSTRACT_EXPRESSIONISM_ROTHKO  *(bonus — ships if budget allows)*

**Vibe:** Two or three luminous rectangular colour-fields stacked vertically with
soft, blurred edges that bleed into each other. Atmospheric, meditative, slow.

**Reference:** Mark Rothko, *Orange, Red, Yellow* (1961), *No. 14* (1960), the Rothko
Chapel paintings (1971). Artobiography and Wikipedia: Rothko soaked layers of
diluted pigment so colours seem to glow from within; edges feathered using
turpentine-on-rag ("turpentine burn"); colours stacked but never sharp.

**Technique:** Procedural 2- or 3-band stacked colour field. Each band has a base
colour from a 2-stop ramp and a vertical position. Edges between bands are
soft — `smoothstep(bandY-feather, bandY+feather, uv.y)` with `feather` ≥ 0.05.
Inside each band, a *very* low-frequency 2D noise (scale ~3, drifting at 0.01 Hz)
modulates brightness by ±5% so bands feel *painted*, never digital-flat.
Vignette darkens corners 10–15%. Subtle film-grain (`hash*0.015`). Only two
audio responses, both gentle: `audioLevel` brightens by ≤5%, and on bass impact a
slow ripple crosses the canvas (think: someone walked into the room).

**Audio mapping:** `audioBass` → slow ripple wave (very subtle); `audioMid` →
ignored (stays meditative); `audioHigh` → grain amplitude;
`audioLevel` → ≤5% brightness pulse with `audioInfluence` cap.

**Texture treatment:** `inputTex` optional. When bound, *averages* the live video
to a 3-pixel-wide column-mean and uses those means as the band colours. So a
sunset photo or video drives the Rothko bands' palette while preserving the
soft-edge field structure.

**Pitfalls:** Hard edges kill it — feather must always be > 0.04. Saturated colours
look wrong; Rothko is *muted intensity*, slightly desaturated and warm. Fast
audio reaction destroys meditative feel; cap `audioInfluence` at 0.1.

**INPUTS:**
```json
[
  {"NAME":"bandCount","TYPE":"float","MIN":2.0,"MAX":3.0,"DEFAULT":3.0},
  {"NAME":"feather","TYPE":"float","MIN":0.04,"MAX":0.2,"DEFAULT":0.09},
  {"NAME":"colorTop","TYPE":"color","DEFAULT":[0.95,0.55,0.20,1.0]},
  {"NAME":"colorMid","TYPE":"color","DEFAULT":[0.85,0.20,0.15,1.0]},
  {"NAME":"colorBot","TYPE":"color","DEFAULT":[0.95,0.78,0.30,1.0]},
  {"NAME":"vignette","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.15},
  {"NAME":"audioInfluence","TYPE":"float","MIN":0.0,"MAX":0.1,"DEFAULT":0.04},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 cTop = colorTop.rgb, cMid = colorMid.rgb, cBot = colorBot.rgb;
if (false) { /* useTex column-mean override (host-defined) */ }
float t1 = smoothstep(0.66-feather, 0.66+feather, uv.y);
float t2 = smoothstep(0.33-feather, 0.33+feather, uv.y);
vec3 col = mix(mix(cBot, cMid, t2), cTop, t1);
col *= 1.0 + slowNoise(uv*3.0 + TIME*0.01)*0.05;
col *= 1.0 - pow(length(uv-0.5)*1.4, 3.0)*vignette;
col += slowRipple(uv, TIME, audioBass)*audioInfluence;
col += (hash(uv*RENDERSIZE)-0.5)*0.015*audioHigh;
gl_FragColor = vec4(col*(1.0+audioLevel*audioInfluence), 1.0);
```

---

## 13. OPART_VASARELY  *(replaces Riley spec already shipped)*

**Vibe:** A perfectly square checkerboard grid, but the squares' sizes warp
spherically as if pushed out from behind by an invisible bulging glass dome. The
flat surface acquires impossible volume.

**Reference:** Victor Vasarely, *Vega III* (1968), *Vega-Nor* (1969); the *Zebra*
(1937) is the early opening shot. Singulart and TheArtStory describe the *Vega*
series' technique: chromatic checkerboard distorted by spherical bulge that
"creates illusions of concave and convex shapes." This is the inverse of Riley's
parallel-stripe destabilisation — Vasarely uses *grids*, not stripes, to fake
volume.

**Technique:** Compute distance from canvas centre `r = length(uv-0.5)`. Apply a
*spherical bulge* warp: sampled grid cell index = `floor((uv-0.5) * grid *
(1.0 + bulge*pow(1.0-r, 2.0))) ... + 0.5`. Cells inside the bulge get smaller
(more cells visible), creating apparent convexity. Each cell is filled
checkerboard-style: `(cellId.x+cellId.y) % 2` chooses light or dark, but with
*two-tone hue rotation* — the colour pair drifts across the canvas
(`mix(magenta, cyan, uv.x)` style). On audio bass, the bulge amplitude
pulses. Optional second bulge at `mousePos` for interactive use.

**Audio mapping:** `audioBass` → bulge magnitude (the dome inflates);
`audioMid` → hue rotation speed; `audioHigh` → cell colour saturation;
`audioLevel` → bulge centre micro-jitter.

**Texture treatment:** `inputTex` optional. When bound, replaces the two
checkerboard tones with two complementary samples *from the input* (top-left and
bottom-right pixel), so the live video drives the palette while the warp
structure stays.

**Pitfalls:** Grid size > 40 → moiré at the bulge centre (the cells become
sub-pixel). Bulge amplitude > 0.6 inverts the warp. Without hue rotation it
reads as plain checkerboard; the *colour shift across the canvas* is what makes
it Vasarely.

**INPUTS:**
```json
[
  {"NAME":"gridDensity","TYPE":"float","MIN":8.0,"MAX":36.0,"DEFAULT":20.0},
  {"NAME":"bulgeAmount","TYPE":"float","MIN":0.0,"MAX":0.6,"DEFAULT":0.25},
  {"NAME":"bulgeCenter","TYPE":"point2D","DEFAULT":[0.5,0.5]},
  {"NAME":"hueShift","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"hueRotateSpeed","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.05},
  {"NAME":"useTexPalette","TYPE":"bool","DEFAULT":false},
  {"NAME":"audioReact","TYPE":"float","MIN":0.0,"MAX":2.0,"DEFAULT":1.0},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec2 centered = uv - bulgeCenter;
float r = length(centered);
float bulge = bulgeAmount * (1.0 + audioBass*audioReact*0.4);
vec2 warped = centered * (1.0 + bulge * pow(max(0.0, 1.0-r*1.4), 2.0));
vec2 g = warped * gridDensity;
vec2 cellId = floor(g);
float check = mod(cellId.x+cellId.y, 2.0);
vec3 a = useTexPalette ? IMG_NORM_PIXEL(inputTex,vec2(0.1,0.5)).rgb : vec3(0.92,0.18,0.55);
vec3 b = useTexPalette ? IMG_NORM_PIXEL(inputTex,vec2(0.9,0.5)).rgb : vec3(0.18,0.78,0.92);
float h = uv.x + TIME*hueRotateSpeed*audioMid;
vec3 col = (check<0.5) ? a : b;
col = hueRotate(col, h*hueShift);
col *= 0.7 + audioHigh*0.3;
gl_FragColor = vec4(col, 1.0);
```

---

## 14. POPART_LICHTENSTEIN

**Vibe:** A panel torn from a comic book — bold black outlines, primary flat fills,
and Ben-Day dots filling the shadow areas. Speech bubble with a sound-word
("WHAAM!", "POW!", "ZOK!") flares on bass hits.

**Reference:** Roy Lichtenstein, *Whaam!* (1963), *Drowning Girl* (1963),
*Look Mickey* (1961). Singulart and TheCollector: Lichtenstein magnified the
mechanical Ben-Day dots used in comic printing into the centrepiece of his work.
Hand-painted via perforated metal stencil from 1962 onwards.

**Technique:** Take `inputTex`, posterize harshly to 4 levels (or 3-level if very
high contrast). Map each level to a fixed palette: white, primary yellow, primary
red, ink-black. The *shadow* tone (level 1, second-darkest) is replaced not with
flat colour but with **Ben-Day dots** — a regular dot grid where the dot radius
is proportional to that pixel's underlying intensity. Detect edges
(`length(grad)`) and overlay thick black outlines. Optional speech bubble: a
white blob with a tail at hashed position, containing a procedural "sound word"
glyph that scales with bass impact.

**Audio mapping:** `audioBass` → speech-bubble flash (bubble appears and word
scales up on hit then decays); `audioMid` → dot density variation;
`audioHigh` → outline thickness; `audioLevel` → palette saturation.

**Texture treatment:** `inputTex` is the *content* being comic-fied. Without it,
fall back to a procedural face silhouette.

**Pitfalls:** Posterize threshold matters more than dot frequency; without crisp
4-level quantization it just looks half-tone. Outlines need to be ~2–3 px,
not 1 — Lichtenstein's lines are *fat*. Without the speech bubble it reads as
"comic filter," not Lichtenstein; the bubble is the signature flourish.

**INPUTS:**
```json
[
  {"NAME":"posterizeLevels","TYPE":"float","MIN":2.0,"MAX":5.0,"DEFAULT":4.0},
  {"NAME":"dotDensity","TYPE":"float","MIN":40.0,"MAX":200.0,"DEFAULT":100.0},
  {"NAME":"outlineWidth","TYPE":"float","MIN":0.001,"MAX":0.01,"DEFAULT":0.003},
  {"NAME":"outlineThreshold","TYPE":"float","MIN":0.05,"MAX":0.4,"DEFAULT":0.15},
  {"NAME":"speechBubble","TYPE":"bool","DEFAULT":true},
  {"NAME":"speechWord","TYPE":"long","DEFAULT":0,"VALUES":[0,1,2,3],"LABELS":["WHAAM","POW","ZOK","BAM"]},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 raw = IMG_NORM_PIXEL(inputTex, uv).rgb;
float lum = dot(raw, vec3(0.33));
float lvl = floor(lum * posterizeLevels) / posterizeLevels;
const vec3 PAL[5] = vec3[5](vec3(0.05),vec3(0.85,0.10,0.15),vec3(0.10,0.30,0.85),vec3(0.97,0.85,0.10),vec3(0.98));
vec3 col = PAL[clamp(int(lvl*4.0),0,4)];
if (lvl < 0.4) {  // shadow zone: Ben-Day dots
  vec2 dGrid = uv * dotDensity;
  float dotR = (1.0-lum)*0.4 + 0.05*audioMid;
  float dotMask = step(length(fract(dGrid)-0.5), dotR);
  col = mix(vec3(0.95), PAL[1], dotMask);
}
float edge = length(vec2(dFdx(lum), dFdy(lum)));
col = mix(col, vec3(0.05), smoothstep(outlineThreshold, outlineThreshold+0.05, edge*audioHigh*outlineWidth*1000.0));
if (speechBubble) col = mix(col, speechBubbleAndWord(uv, int(speechWord), audioBass), bubbleMask(uv, audioBass));
gl_FragColor = vec4(col, 1.0);
```

---

## 15. MINIMALISM_STELLA

**Vibe:** Concentric square bands marching outward from the centre, each band a
single uniform colour, hairline gaps between them. Pure geometric reduction —
"what you see is what you see."

**Reference:** Frank Stella, *Hyena Stomp* (1962, concentric squares),
*Marrakech* (1964, *Concentric Squares* series), the early *Black Paintings*
(1958–60) for the band-and-gap structure. Stella: "My painting is based on the
fact that only what can be seen there is there."

**Technique:** Compute Chebyshev distance from canvas centre:
`d = max(abs(uv.x-0.5), abs(uv.y-0.5))`. Quantise `d` into `bandCount` rings:
`band = floor(d * bandCount * 2.0)`. Each band fills with a colour from a
*sequenced* palette — Stella's *Hyena Stomp* uses a marching rainbow; the *Black
Paintings* use only black with raw-canvas gaps. Between bands, a hairline gap —
`smoothstep(0.49, 0.5, fract(d*bandCount*2.0))` reveals raw-canvas off-white.
Optional: rotate the squares slightly off-axis (Stella's *Concentric Squares*
were sometimes oriented diagonally inside diamond-shaped canvases).

**Audio mapping:** `audioBass` → palette march speed (bands shift colour outward
on beats); `audioMid` → square rotation; `audioHigh` → gap-line glow;
`audioLevel` → overall saturation.

**Texture treatment:** `inputTex` optional. When bound, each band samples a single
pixel from `inputTex` at its corresponding radius (band 0 from centre pixel,
outer band from edge), creating a radial palette extracted from live video.

**Pitfalls:** Anti-aliased band edges look wrong — the gap *is* the edge, so
hard `step()` is correct, with the gap thickness as the only smoothness.
Rainbow palette can look toy; better to ship 4 preset palettes (Hyena Stomp
march, Black Paintings minimal, Marrakech metallic, Stella late synthetic) and
let the user pick. Band count > 20 reads as bullseye, not Stella; keep ~6–12.

**INPUTS:**
```json
[
  {"NAME":"bandCount","TYPE":"float","MIN":4.0,"MAX":16.0,"DEFAULT":9.0},
  {"NAME":"gapWidth","TYPE":"float","MIN":0.005,"MAX":0.04,"DEFAULT":0.012},
  {"NAME":"palettePreset","TYPE":"long","DEFAULT":0,"VALUES":[0,1,2,3],"LABELS":["HyenaStomp","BlackPaintings","Marrakech","SyntheticLate"]},
  {"NAME":"rotation","TYPE":"float","MIN":0.0,"MAX":1.5,"DEFAULT":0.0},
  {"NAME":"paletteMarch","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.0},
  {"NAME":"useTex","TYPE":"bool","DEFAULT":false},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy - 0.5;
float r = rotation + audioMid*0.05;
uv = mat2(cos(r),-sin(r),sin(r),cos(r)) * uv;
float d = max(abs(uv.x), abs(uv.y));
float bf = d * bandCount * 2.0;
float bandIdx = floor(bf + paletteMarch*audioBass*4.0);
vec3 col = useTex ? IMG_NORM_PIXEL(inputTex, vec2(d,0.5)).rgb
                  : stellaPalette(int(palettePreset), int(bandIdx));
float gap = smoothstep(0.5-gapWidth, 0.5, fract(bf));
col = mix(col, vec3(0.94,0.91,0.83), gap);
col *= 0.85 + audioLevel*0.2;
gl_FragColor = vec4(col + gap*audioHigh*0.1, 1.0);
```

---

## 16. VAPORWAVE_FLORAL_SHOPPE

**Vibe:** A pink-and-teal sunset over a checkerboard grid receding to a low-poly
horizon, classical Greek bust silhouette in foreground, scanlines, mistranslated
Japanese katakana. Late-night mall in a dream.

**Reference:** Macintosh Plus, *Floral Shoppe* (2011) album cover — bubblegum-pink
background, mint katakana ("マッキントッシュ・プラス"), marble bust of Helios on a
checkerboard plinth. Wikipedia/VaporwaveWiki canonical. The visual idiom
borrows from Windows 95 demos, late-Soviet computer-art, mall-aesthetic CD-ROM
covers.

**Technique:** Three composed elements: (1) **Sky gradient** — vertical
`#FF6FB5` (bubblegum) at top → `#5DD9C1` (teal) at horizon. (2) **Checkerboard
floor** — perspective-warped grid receding to vanishing point at horizon
(`uv.y > horizon` is sky, below is floor with `gridUV = uv.x / (uv.y*persp)`).
Grid colours alternate hot pink and dark magenta. (3) **Marble bust** —
silhouette SDF (carved profile, semi-circle skull, neck) centred on canvas at
mid-depth. Bust filled with a marble-noise texture. (4) **Scanlines** —
`(0.85 + 0.15*sin(uv.y*RENDERSIZE.y*0.5))` overlay. (5) **Katakana glyphs** —
sparse procedural rectangle clusters at top edge, in mint colour, simulating
the album's Japanese title. (6) **Sun** — large flat circle behind the bust,
pure magenta. Subtle CRT bloom.

**Audio mapping:** `audioBass` → sun pulses larger, scanlines flicker; `audioMid` →
checkerboard-recession warp ("floor breathes"); `audioHigh` → katakana glitch
(glyphs flicker / shift); `audioLevel` → CRT bloom intensity.

**Texture treatment:** `inputTex` optional. When bound, replaces the marble
bust's surface with a desaturated, slightly-pink-tinted version of the input.
Live video as the face of Helios.

**Pitfalls:** RGB pink/teal slightly off looks like wrong-decade nostalgia (Memphis,
not Vaporwave). The bust can't be photoreal — it must be a flat silhouette
with marble noise; sculpted detail breaks the 2D-cutout feel. Scanlines too
strong fight the gradient; keep amplitude ≤ 0.2.

**INPUTS:**
```json
[
  {"NAME":"horizonY","TYPE":"float","MIN":0.4,"MAX":0.7,"DEFAULT":0.55},
  {"NAME":"sunSize","TYPE":"float","MIN":0.1,"MAX":0.4,"DEFAULT":0.22},
  {"NAME":"checkerboardPersp","TYPE":"float","MIN":0.5,"MAX":3.0,"DEFAULT":1.6},
  {"NAME":"bustSize","TYPE":"float","MIN":0.15,"MAX":0.45,"DEFAULT":0.3},
  {"NAME":"scanlineAmp","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.15},
  {"NAME":"katakanaIntensity","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.7},
  {"NAME":"crtBloom","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.4},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
vec3 sky = mix(vec3(1.0,0.42,0.71), vec3(0.36,0.85,0.76), smoothstep(horizonY-0.1, horizonY+0.05, uv.y));
vec3 col = sky;
if (uv.y < horizonY){
  vec2 fl = vec2((uv.x-0.5)/((horizonY-uv.y)*checkerboardPersp+0.01), TIME*0.2 + 1.0/(horizonY-uv.y));
  float chk = mod(floor(fl.x*8.0)+floor(fl.y*8.0), 2.0);
  col = mix(vec3(1.0,0.42,0.71), vec3(0.55,0.10,0.45), chk);
}
float sun = smoothstep(sunSize*(1.0+audioBass*0.1), sunSize*0.95, length(uv-vec2(0.5,horizonY)));
col = mix(col, vec3(1.0,0.20,0.60), sun);
float bust = bustSDF(uv-vec2(0.5,horizonY+0.05), bustSize);
vec3 bustCol = IMG_NORM_PIXEL(inputTex, uv).rgb;
bustCol = mix(marbleNoise(uv), bustCol, 0.4);
col = mix(col, bustCol, bust);
col *= 0.85 + scanlineAmp*sin(gl_FragCoord.y * 1.0)*audioBass;
col = mix(col, vec3(0.7,1.0,0.85), katakanaField(uv)*katakanaIntensity*audioHigh);
col += col*crtBloom*0.3*audioLevel;
gl_FragColor = vec4(col, 1.0);
```

---

## 17. GLITCH_DATAMOSH

**Vibe:** Horizontal bands of corrupt scanlines, RGB channel split, blocky DCT
compression artefacts, occasional motion-vector smear bleeding the previous
frame across the current one. The signal is dying in interesting ways.

**Reference:** Rosa Menkman's compression-artefact work; Takeshi Murata's
*Monster Movie* (2005) datamosh; Kanye West's *Welcome to Heartbreak* music video.
Datamosh = removing I-frames so motion vectors apply forever; shifted scanlines
= row-shift artefacts; JPEG decay = block-DCT mis-quantisation.

**Technique:** Stack of corruptions on `inputTex`: (1) **Horizontal row-shift**:
divide canvas into rows of varying heights, each row offset by
`hash(rowId+TIME)*tearAmp`. (2) **RGB channel split**: sample R/G/B at slightly
different x-offsets (`±chroma`). (3) **JPEG-ish block corruption**: in random
8×8 blocks, replace pixel values with the *block average* + per-channel quantize
(simulates DCT quantization steps). (4) **Datamosh smear**: every K frames,
read a frame-old buffer (requires persistent FBO support — fall back to
self-feedback if unavailable) and bleed it forward in motion-vector direction.
(5) **Block-replacement glitches**: every few frames, replace a random region
with hashed garbage. Bass triggers smear bursts.

**Audio mapping:** `audioBass` → glitch burst probability (signal collapses);
`audioMid` → row-tear amplitude; `audioHigh` → chroma split width;
`audioLevel` → block-corruption density.

**Texture treatment:** `inputTex` is the *signal being corrupted*. Without it,
fall back to procedural test pattern. Optional `prevFrameTex` for true
datamosh smear.

**Pitfalls:** Without proper feedback FBO the datamosh trail looks wrong (too
clean). Chroma > 0.05 reads as broken display, not glitch art (intentional
design choice — but worth flagging). Block corruption needs to feel *blocky*
(8×8 grid aligned), not randomly speckled.

**INPUTS:**
```json
[
  {"NAME":"tearAmp","TYPE":"float","MIN":0.0,"MAX":0.15,"DEFAULT":0.05},
  {"NAME":"rowDensity","TYPE":"float","MIN":4.0,"MAX":40.0,"DEFAULT":18.0},
  {"NAME":"chroma","TYPE":"float","MIN":0.0,"MAX":0.04,"DEFAULT":0.012},
  {"NAME":"blockCorruption","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"datamoshSmear","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.5},
  {"NAME":"glitchBurstProb","TYPE":"float","MIN":0.0,"MAX":0.5,"DEFAULT":0.08},
  {"NAME":"smearDir","TYPE":"point2D","DEFAULT":[0.02,0.0]},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
float rowH = 1.0/rowDensity;
float rowId = floor(uv.y/rowH);
float tear = (hash(vec2(rowId, floor(TIME*8.0)))-0.5) * tearAmp * (1.0+audioMid*2.0);
uv.x += tear;
float ch = chroma*(1.0+audioHigh*2.0);
float r = IMG_NORM_PIXEL(inputTex, uv+vec2(ch,0)).r;
float g = IMG_NORM_PIXEL(inputTex, uv             ).g;
float b = IMG_NORM_PIXEL(inputTex, uv-vec2(ch,0)).b;
vec3 col = vec3(r,g,b);
vec2 blk = floor(uv*RENDERSIZE.xy/8.0)*8.0/RENDERSIZE.xy;
if (hash(blk+floor(TIME*4.0)) < blockCorruption*audioLevel)
  col = quantize(IMG_NORM_PIXEL(inputTex, blk).rgb, 4);
col = mix(col, IMG_NORM_PIXEL(inputTex, uv-smearDir).rgb, datamoshSmear*audioBass);
if (hash(vec2(rowId, floor(TIME*16.0))) < glitchBurstProb*audioBass)
  col = vec3(hash(uv*TIME), hash(uv*TIME+1.3), hash(uv*TIME+2.7));
gl_FragColor = vec4(col, 1.0);
```

---

## 18. AI_LATENT_DRIFT

**Vibe:** Slow morphing washes of soft-focus colour and form — patches of
recognisable-but-not imagery (faces? landscapes? architectures?) bleeding into
abstract clouds and back, as if a diffusion model is trapped mid-denoise. Not a
filter on input — a *generated dream* that drifts.

**Reference:** Refik Anadol, *Machine Hallucinations* (2019–2024), *Unsupervised
— Machine Hallucinations* MoMA installation (2022); Mario Klingemann's
*Memories of Passersby I*; the look of a Stable Diffusion latent-walk
animation (Keras tutorial). The defining feel is *coherent local detail without
global structure* — every region looks like it *might* be something but never
quite resolves.

**Technique:** Two layered noise fields driven by *three slow LFOs* (latent
walk simulation): a low-frequency fbm (`fbm(uv*2 + LFO1*time)`) defines large-scale
form; a higher-frequency fbm (`fbm(uv*8 + LFO2*time)`) adds mid-detail; a curl-noise
domain warp (`uv += curl(uv*4)*LFO3*0.1`) gives the "drifting" character. Pass
through a soft palette LUT (a wide hue arc at low saturation — cream → mauve
→ teal → soft orange — sampled by `dot(noise, fixedDir)`). Apply a strong
*directional blur* (≥ 8 taps along a curl-vector) to smear edges into that
diffusion-model softness. Add subtle film-grain. Optional "phantom feature"
pass: a sparse FFT of the input image (or `audioFFT`) overlaid at low opacity to
suggest hidden recognisable forms.

**Audio mapping:** `audioBass` → LFO acceleration (latent walk speeds up on
beats); `audioMid` → palette saturation; `audioHigh` → phantom-feature
opacity; `audioLevel` → overall softness/brightness.

**Texture treatment:** `inputTex` optional. When bound, *blends* into the latent
field at very low opacity — the input becomes a "prompt" the dream
half-remembers. Pass through several taps of directional blur first so it
loses recognisability.

**Pitfalls:** Sharp edges break the illusion entirely — directional blur must
always run, even at low setting. Saturation too high reads as "dreamy filter,"
not latent space; keep palette muted (≤ 0.6 saturation). Without the phantom-feature
overlay it's pretty fbm but doesn't *feel* AI; the suggestion of hidden form
is the whole point.

**INPUTS:**
```json
[
  {"NAME":"latentWalkSpeed","TYPE":"float","MIN":0.01,"MAX":0.3,"DEFAULT":0.06},
  {"NAME":"detailScale","TYPE":"float","MIN":2.0,"MAX":12.0,"DEFAULT":6.0},
  {"NAME":"warpAmount","TYPE":"float","MIN":0.0,"MAX":0.3,"DEFAULT":0.12},
  {"NAME":"directionalBlurTaps","TYPE":"float","MIN":4.0,"MAX":16.0,"DEFAULT":8.0},
  {"NAME":"saturation","TYPE":"float","MIN":0.2,"MAX":0.8,"DEFAULT":0.5},
  {"NAME":"phantomFeatures","TYPE":"float","MIN":0.0,"MAX":1.0,"DEFAULT":0.3},
  {"NAME":"texPromptInfluence","TYPE":"float","MIN":0.0,"MAX":0.4,"DEFAULT":0.1},
  {"NAME":"inputTex","TYPE":"image"}
]
```

**main() skeleton:**
```glsl
vec2 uv = gl_FragCoord.xy/RENDERSIZE.xy;
float ws = latentWalkSpeed * (1.0+audioBass);
vec2 warp = curlField(uv*4.0 + TIME*ws*0.7) * warpAmount;
uv += warp;
float low  = fbm(uv*2.0 + TIME*ws);
float high = fbm(uv*detailScale + TIME*ws*1.7);
float field = low*0.65 + high*0.35;
vec3 col = latentPalette(field, uv.x*0.3 + TIME*ws*0.5);
col = directionalBlur(col, uv, normalize(warp), int(directionalBlurTaps));
col = mix(vec3(dot(col,vec3(0.33))), col, saturation*(0.7+audioMid*0.3));
col += phantomFeatureField(uv, audioFFT)*phantomFeatures*audioHigh;
col = mix(col, IMG_NORM_PIXEL(inputTex, uv+warp).rgb, texPromptInfluence);
col += (hash(uv*RENDERSIZE+TIME)-0.5)*0.012;
gl_FragColor = vec4(col*(0.85+audioLevel*0.2), 1.0);
```

---

## Cross-cutting implementation notes

- **Shared helpers** (extend `_lib.glsl`):
  `worleyFbm`, `curlField`, `curlAdvect`, `magritteClouds`, `marbleNoise`,
  `latentPalette`, `fauvistPalette`, `stellaPalette[4]`, `objectSDF`,
  `bustSDF`, `triangle`, `recursiveSubdiv`, `gridLine`, `quantize`,
  `directionalBlur`, `halftone`, `glyphField`, `katakanaField`,
  `phantomFeatureField`, `hueRotate`, `sepia`, `hardContrast`.
- **Palette discipline**: each shader's character lives in its palette — define
  fixed palettes inside the shader, never use generic `hsv2rgb(t)` rainbows.
- **Sample order suggested for batched implementation** (2–3 per week, easy → hard):
  1. ROTHKO (simplest — gradients + soft edges; validates LUT pipeline)
  2. MONDRIAN, STELLA (procedural geometry, no inputTex deps)
  3. MUCHA, KANDINSKY, CONSTRUCTIVISM (hand-crafted procedural, no heavy math)
  4. FAUVISM, BAUHAUS-adjacent variants (palette/voronoi + audio)
  5. CUBISM, FUTURISM, EXPRESSIONISM (inputTex + warps; reuse existing helpers)
  6. POPART, GLITCH_DATAMOSH, VAPORWAVE (inputTex-heavy with effect stacks)
  7. POLLOCK, VASARELY (curl-noise advection / spherical bulge — gnarlier math)
  8. DADA, MAGRITTE, AI_LATENT (compositional / multi-element scenes)
- **Audio idle floor**: every shader should `max(audioX, 0.05)` so quiet rooms
  still drift gently. ROTHKO is the exception — leave it static when silent.
- **Authenticity**: every spec calls out specific named works. Drift from the
  reference work and the historical fidelity collapses; resist the temptation
  to "improve" Mondrian's palette or invent a new Magritte object.
- **References**:
  [Mucha whiplash – V&A](https://www.vam.ac.uk/articles/the-whiplash) ·
  [Open Window, Collioure – NGA](https://www.nga.gov/artworks/106384-open-window-collioure) ·
  [Munch & Kirchner – Yale](https://artgallery.yale.edu/press-release/munch-and-kirchner-anxiety-and-expression) ·
  [Real-time Cubism Shader – Shahriar Shahrabi](https://shahriyarshahrabi.medium.com/real-time-cubism-shader-5c8e0c79195c) ·
  [Dynamism of a Cyclist – Wikipedia](https://en.wikipedia.org/wiki/Dynamism_of_a_Cyclist) ·
  [Beat the Whites with the Red Wedge – DailyArt](https://www.dailyartmagazine.com/beat-the-whites-with-the-red-wedge/) ·
  [Hannah Höch – TheArtStory](https://www.theartstory.org/artist/hoch-hannah/) ·
  [Composition with Red, Blue, Yellow – Wikipedia](https://en.wikipedia.org/wiki/Composition_with_Red,_Blue_and_Yellow) ·
  [Bauhaus Color Theory – Color Meanings](https://www.color-meanings.com/bauhaus-color-theory-itten-kandinsky-albers-klee/) ·
  [Castle of the Pyrenees – Wikipedia](https://en.wikipedia.org/wiki/The_Castle_of_the_Pyrenees) ·
  [Autumn Rhythm – Met Museum](https://www.metmuseum.org/art/collection/search/488978) ·
  [Orange, Red, Yellow – Wikipedia](https://en.wikipedia.org/wiki/Orange,_Red,_Yellow) ·
  [Vega-Nor – Singulart](https://www.singulart.com/blog/en/2024/02/25/vega-nor-by-victor-vasarely/) ·
  [Whaam! – Singulart](https://www.singulart.com/blog/en/2024/05/16/whaam-by-roy-lichtenstein/) ·
  [Frank Stella – Wikipedia](https://en.wikipedia.org/wiki/Frank_Stella) ·
  [Floral Shoppe – Wikipedia](https://en.wikipedia.org/wiki/Floral_Shoppe) ·
  [Glitch Art – Wikipedia](https://en.wikipedia.org/wiki/Glitch_art) ·
  [Latent space walks – Keras](https://keras.io/examples/generative/random_walks_with_stable_diffusion/)
