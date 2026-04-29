# Shader Critique — Pass 1

Audit of 18 art-movement shaders against `art_movement_canon.md`. Each file is graded on movement (anti-static), anti-Voronoi/anti-tile, canon fidelity, parameter coverage, and technique signifier. Suggestions are mechanical so the implementer can apply them quickly.

---

## art_nouveau_mucha.fs

**Movement signifier:** Domain-warped sine field driving thick variable-width strokes with gold-leaf bloom; flat colour fill bounded by smooth signed-distance contours.

**Verdict:** PASS

**What's working:** Persistent paintBuf with N parametric whiplash heads continuously depositing strokes is exactly the right "living calligraphy" technique. Three style branches (Mucha pastel / Klimt mosaic gold / Beardsley pure ink) actually change palette, not just gain.

**Problems found:**
- The Klimt style (1) shows a visible static jittered tile grid through `floor(uv*80.0)` — that grid never moves and is exactly the anti-Voronoi/anti-stained-glass thing the user banned even though it was framed as "mosaic gold ground".
- Whiplash strokes are linear `mix(startPt, endPt, tt)` with sinusoidal perpendicular wobble — not really "whiplash" S-curves; they're straight gradient-rides with side-to-side wiggle.
- `fract(t * 0.5 + …)` resets each tendril abruptly to its start point, producing a noticeable jump.

**Mechanical fixes (apply these directly):**
1. In `fieldColor` style==1 branch (~lines 51-57): replace the static `vec2 g = floor(uv * 80.0)` mosaic with a slowly-drifting one: `vec2 g = floor((uv + vec2(sin(TIME*0.05), cos(TIME*0.04))*0.005) * 80.0);` so the gold ground micro-shimmers instead of being frozen.
2. In `tendrilPos` (~lines 80-99): replace the linear `mix(startPt, endPt, tt)` with a true S-curve, `vec2 base = mix(startPt, endPt, smoothstep(0.0, 1.0, tt))` and add a low-frequency curve bend `base += perp * sin(tt*3.14159) * amp * 0.5;` so the whole stroke arcs.
3. In `tendrilPos`: smooth the `fract` reset by using `tt = abs(fract(t*0.5 + hash11(fi*5.7))*2.0 - 1.0)` so the tendril ping-pongs along its path instead of teleporting back.

**New parameter to add (if any):** none — set is already comprehensive.

---

## fauvism_matisse.fs

**Movement signifier:** Posterize to ~6 hues at maxed saturation, then displace by short directional brush-noise; complementary-colour edge boost.

**Verdict:** PASS

**What's working:** Curl-advected paintBuf with continuous Fauvist-primary drop deposition is a strong fluid-feedback technique that matches the canon's "TouchDesigner FluidStream" macro. No cells, no outlines, real motion every frame from the advection step.

**Problems found:**
- Drop deposition uses a fixed grid `floor(uv * gridN)` — drops always land on grid cell centres so the deposit pattern is more regular than a hand-painted dab.
- Saturation is post-applied via desaturate-toward-luma; Fauvism actually wants posterization toward a small palette of hues, not just chroma boost.
- No complementary-colour edge boost (the canon's signifier).

**Mechanical fixes (apply these directly):**
1. In the drop loop (~lines 95-117): break the grid by jittering each cell-id over time, `vec2 g = floor(uv * gridN) + floor(vec2(TIME*0.13, TIME*0.17));` so cells drift across the canvas frame to frame instead of always sampling the same screen positions.
2. In Pass 1 output (~lines 141-148): add a snap-to-palette posterize step before saturation, e.g. for each pixel find nearest of `FAUVE[6]` and `mix(col, nearest, 0.35)` — gives the painted-block look the canon calls for.
3. Add a complementary edge boost: after `saturateColor`, compute `dFdx`/`dFdy` of luma and blend the complementary FAUVE color into edges — Fauvist contour vibration.

**New parameter to add (if any):** `{ "NAME": "posterize", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.35 }` — strength of snap-to-Fauvist-palette, since Matisse's planes are unmodulated colour.

---

## expressionism_kirchner.fs

**Movement signifier:** High-contrast posterize with carved-wood noise overlay, black ridge contours from edge detect, slight skew/shear distortion of form.

**Verdict:** NEEDS_REWRITE

**What's working:** Acid LUT and ink-line edge detection are correct moves. Hard contrast curve is right.

**Problems found:**
- **Voronoi.** The shader uses `worley()` and `worleyFbm()` directly — the spec called this out as "spec is explicit: smooth Perlin yields watercolour, not Kirchner" but Worley *is* the cellular partitioning the user banned. The fallback is also `worley(uv*16.0)` for the cobblestones — pure Voronoi, the exact failure mode flagged.
- **Static.** With no input texture and TIME=0, the fallback is a frozen Worley grid. With TIME>0 the only motion is `+ TIME * flow` inside the warp displacement — so the Worley cells are only mildly displaced, the cobblestones never actually move.
- No woodcut signifier — the canon explicitly wants "carved-wood noise overlay", not Voronoi.
- No skew/shear distortion (canon signifier).

**Mechanical fixes (apply these directly):**
1. Delete `worley` and `worleyFbm` entirely. Replace with carved-wood ridged noise: `float ridge(vec2 p){ float n=vnoise(p); return 1.0 - abs(2.0*n - 1.0); }` and use `ridge` everywhere the shader currently uses `worley`. Add `vnoise` (value noise — see fauvism file lines 41-49 for code).
2. Replace fallback cobblestone block (~lines 53-57) with a moving Berlin-street horizon: `vec3 stone = mix(vec3(0.18,0.16,0.20), vec3(0.42,0.35,0.28), ridge(uv*8.0 + TIME*0.3));` plus a vertical lamp-streak `stone += vec3(0.6,0.4,0.2) * smoothstep(0.02, 0.0, abs(fract(uv.x*3.0 + TIME*0.1) - 0.5));`.
3. After the warp (~line 44), add a horizontal shear that breathes with audio: `src.x += (src.y - 0.5) * 0.15 * sin(TIME*0.7) * (1.0 + audioMid);` — this is the Kirchner "Street Berlin" leaning-perspective signifier.
4. Add a persistent paint pass (PASSES with persistent buffer) so woodcut grooves accumulate over time — currently the shader renders one frame every frame with no memory.

**New parameter to add (if any):** `{ "NAME": "shearAmount", "TYPE": "float", "MIN": 0.0, "MAX": 0.4, "DEFAULT": 0.15 }` — Kirchner's diagonal shove of the perspective.

---

## cubism_picasso.fs

**Movement signifier:** Voronoi tessellation with per-cell affine transform of source image; desaturate to ochre-grey; multiplicative overlap of fragmented planes.

**Verdict:** NEEDS_TWEAK

**What's working:** Overlapping translucent rotated rectangle planes with per-plane viewpoint sampling and gradient shading is a strong analytic-cubism technique that AVOIDS Voronoi (good — the canon mentions Voronoi but the user banned cellular tessellation and this shader's approach is the right alternative). Ochre LUT is correct. Per-plane lightDir+jitter is exactly right.

**Problems found:**
- Drift amplitude `0.012` is too small — `dr * drift` with drift maxed at 0.06 is barely perceptible. With TIME=0 the composition is frozen and even with TIME running it just twitches. The user wants real movement.
- Plane centres are computed once per shader invocation per plane; centres NEVER cross-fade or recombine — so the same set of N planes float in fixed positions forever.
- Letter fragments are static (no time term in `letterField`).
- Fallback procedural striation `sin(sUV.y * 11.0 + sUV.x * 2.0)` is dull — same view per plane, defeating the multi-perspective signature.

**Mechanical fixes (apply these directly):**
1. In the for-loop (~line 107): change `vec2 rawCtr = vec2(hash11(fi * 1.31), hash11(fi * 2.97 + 4.7));` to time-cycling centres `vec2 rawCtr = vec2(hash11(fi * 1.31) + 0.05*sin(TIME*0.4 + fi), hash11(fi * 2.97 + 4.7) + 0.05*cos(TIME*0.31 + fi*1.3));` so each plane drifts continuously.
2. Bump `drift` slider DEFAULT from 0.012 to 0.04 and MAX from 0.06 to 0.15. Static composition is the failure mode.
3. In `letterField` (~line 59): add a slow tumble — `seed += floor(TIME*0.2)` so letter clusters rotate through positions every ~5 seconds.
4. In the procedural fallback (~lines 152-156): add time to the stripe `float stripe = sin(sUV.y * 11.0 + sUV.x * 2.0 + TIME*0.5 + fi*1.7) * 0.5 + 0.5;` so each plane's content shifts.
5. In the loop, add per-plane fade in/out: `float fadeT = sin(TIME*0.15 + fi*2.7)*0.5+0.5; alpha *= fadeT;` so planes phase between visible and ghost — the painting is constantly recomposing.

**New parameter to add (if any):** `{ "NAME": "recomposeRate", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.3 }` — speed of plane fade-cycling so user can tune the breathing rate.

---

## futurism_boccioni.fs

**Movement signifier:** Frame-echo / motion-blur trail with directional smear; radial speed lines via polar gradient; multi-pass copies offset along velocity vector.

**Verdict:** PASS

**What's working:** Persistent trailBuf advected each frame is exactly the right technique. Phantom-copy chronophotography in the source pass mirrors Balla. Force-rays with origin drift and divisionist dabs are correct signifiers. Speed-driven hue shift is in.

**Problems found:**
- Velocity vector is rotating but with `vMag = velocityMag * (1.0 + audioLevel*…)` the trail still moves only `velocityMag * persistence` per frame — at default 0.025 a fragment moves only 2.5% of canvas per frame and with persistence 0.96 the visible streak length is still small. With audio silent it's near-imperceptible.
- Divisionist-dot `floor(TIME*4.0)` time bucket is fine but only fires when `dh > 0.92`, so dots are sparse and identical between buckets.

**Mechanical fixes (apply these directly):**
1. In the velocity computation (~lines 63-66): add an idle-floor so motion happens without audio, e.g. `float vMag = velocityMag * (0.5 + 0.5*sin(TIME*0.3)) * (1.0 + audioLevel*audioReact*1.5);` — this gives a constant accelerate/decelerate breath even when audio is off, then audio modulates on top.
2. In the velocity rotation (~line 63): increase the default rotation rate by adding `vAng = TIME*velocityRotSpeed + sin(TIME*0.13)*0.6 + audioBass*…`, so the trail snakes through the canvas like Boccioni's "City Rises" instead of just spiralling at constant rate.
3. In the dab block (~line 156): change `floor(TIME*4.0)` to `floor(TIME*4.0 + audioHigh*4.0)` AND lower the `dh > 0.92` threshold to `dh > 0.85` so dabs are denser — Severini's confetti.

**New parameter to add (if any):** none.

---

## constructivism_lissitzky.fs

**Movement signifier:** Hard-edge boolean SDF composition (rect ∪ circle ∪ rotated bar); 3-colour palette clamp; angular Suprematist diagonals as rotated half-plane masks.

**Verdict:** NEEDS_TWEAK

**What's working:** Hard SDF wedge + circle + bars with strict CREAM/RED/BLACK/WHITE palette is the right poster-graphic primitive composition. Glyph cluster is procedurally OK. Anti-Voronoi.

**Problems found:**
- **Almost completely static.** With audio silent the wedge does NOT thrust (`triangleThrust * audioBass` → 0), the circle does NOT contract, bars do NOT rotate (only `audioMid*0.05` rotation), and the printing-press shake is multiplied by `audioLevel` so it also dies. With TIME=0 this is literally a frozen poster.
- Glyph field is purely hashed by `ci`, no time, frozen.
- No animation of the wedge thrust — the canon says "Red wedge piercing white circle"; that's a verb, the wedge needs to constantly thrust.

**Mechanical fixes (apply these directly):**
1. In the wedge thrust (~line 107): change `apex = circCtr - axis*0.05 - axis*triangleThrust*audioBass` to a continuous ramming animation `apex = circCtr - axis*0.05 - axis*triangleThrust*(0.5 + 0.5*sin(TIME*1.7)) - axis*triangleThrust*audioBass*0.4;` so the wedge rams in and out continuously.
2. In the circle radius (~line 98): change to `step(r, circleSize * (0.85 + 0.15*sin(TIME*1.7 + 3.14)) * (1.0 - audioBass*0.05))` so the circle pulses opposite-phase to the wedge — they breathe against each other.
3. In `barShape` (~line 50): add per-bar drift `c += vec2(sin(TIME*0.4 + seed), cos(TIME*0.5 + seed))*0.03;` and rotate `angle += TIME*0.1*sign(seed-0.5);` — the bars sweep across the canvas like rotating searchlights.
4. In `glyphField` (~line 64): change positions to drift with TIME — `origin.x += sin(TIME*0.2 + float(g))*0.02;` — Constructivist typography slipping past.
5. In the printing-press shake (~lines 88-90): drop the `* audioLevel` multiplier so shake happens always: replace `* compositionJitter * audioLevel` with `* compositionJitter * (0.4 + audioLevel*0.6)`.

**New parameter to add (if any):** `{ "NAME": "wedgePulse", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.5 }` — controls the time-driven wedge ramming amplitude independent of audio.

---

## dada_hoch.fs

**Movement signifier:** Random-cut texture atlas with per-shard rotation and scale jitter; channel-split chromatic abberation; halftone print noise overlay.

**Verdict:** PASS

**What's working:** Persistent collageBuf accumulating beat-keyed strips is genuine collage assembly. Halftone, chromatic slide, red ink stamps, and stuttering letterband are all canon-correct signifiers. With `bt = floor(TIME*beatRate)` the layout reshuffles every beat so this is highly dynamic.

**Problems found:**
- The chromatic slide directions `dirR`/`dirB` use `sin(TIME*0.7)` so the offset always lives within `[-1, +1]*chromaSlide` — fine, but the channels never SLIDE far apart on impact. Treble multiplier helps but the absolute magnitude is small.
- Letter band y position uses `sin(bt*0.71)` — bucketed, so it teleports between locations rather than scrolling through the band.
- No explicit "torn paper" ragged edge — `edgeMask = smoothstep(0.0, 0.06, min(edgeX, edgeY))` is smooth, not torn.

**Mechanical fixes (apply these directly):**
1. In `edgeMask` computation (~line 107): replace clean smoothstep with hash-jagged edge: `float jag = hash21(q*40.0 + bt)*0.04; float edgeMask = smoothstep(0.0, 0.06, min(edgeX, edgeY) - jag);` — gives torn-paper irregularity.
2. In letter band (~line 173): change `bandY = 0.5 + sin(bt*0.71)*0.3` to a smooth scroll `bandY = fract(TIME*0.05 + sin(bt*0.71)*0.2)` so the band drifts continuously instead of teleporting per beat.
3. In stamp loop (~line 144): add `vec2 c += vec2(sin(TIME*0.4 + float(g_)), cos(TIME*0.6 + float(g_)))*0.05;` so stamps drift between beats instead of being frozen until the next bt tick.

**New parameter to add (if any):** none.

---

## destijl_mondrian.fs

**Movement signifier:** Axis-aligned BSP partition with primary-only colour assignment; pure flat fills, anti-aliased only at sub-pixel grid edges.

**Verdict:** PASS

**What's working:** This is the canon's correct re-interpretation of *Broadway Boogie Woogie*: marching pulses on horizontal/vertical lanes with primary colour squares pulsing at intersections. Pulses move with `fract(t + spawn)` so this is genuinely animated. Anti-Voronoi.

**Problems found:**
- The lane lines themselves are static (only the pulses move). The `laneY` and `laneX` are hashed once per shader invocation — never breathe.
- Pulse compositing uses `col = pc;` (hard assign), which is fine for pixel-sharp pulses but makes pulse edges look stair-stepped at low pulseSize. Acceptable but harsh.
- No grid-cell colour fills (Mondrian's classic non-Boogie compositions). Canonical Mondrian is just black grid + 3 small primary tiles, which the description correctly says is the Boogie Woogie style — so this is by design. PASS.

**Mechanical fixes (apply these directly):**
1. In the lane Y/X computation (~line 53 and ~line 89): add a slow breath to the lane positions, e.g. `laneY += sin(TIME*0.15 + fi*2.7)*0.01;` so the grid itself slowly shifts rhythm — Boogie Woogie's syncopation should feel like the lanes are listening.
2. In the pulse hard-assign (~line 80, ~line 112): change `col = pc;` to `col = mix(col, pc, smoothstep(pulseSize, 0.0, max(dx, dy)));` for soft-edge pulses that anti-alias correctly.
3. In intersection glow (~line 138): change `lit = 1.0 - smoothstep(boxSz*0.6, boxSz, max(abs(d.x),abs(d.y)))` so it pulses with a beat-phase: `lit *= 0.7 + 0.3*sin(TIME*4.0 + (laneX+laneY)*30.0);` — visible syncopation.

**New parameter to add (if any):** `{ "NAME": "laneBreath", "TYPE": "float", "MIN": 0.0, "MAX": 0.05, "DEFAULT": 0.01 }` — amount lanes drift over time.

---

## bauhaus_kandinsky.fs

**Movement signifier:** SDF library of primitives composed via union/intersection on a strict grid; weighted-circle gradient fields; 3-primary palette with subtle tonal value steps.

**Verdict:** PASS

**What's working:** SDF triangle/box/circle library with strict yellow/red/blue pairing is correct Bauhaus pedagogy. Lissajous orbits drive each shape through real motion. Halo field for "Several Circles" vibe is good. No cells.

**Problems found:**
- The `home` positions are hashed once per shader call so each shape orbits around a fixed home — over many seconds, the composition is the same rotation reset.
- Default `orbitSpeed` = 0.15 with `orbitRange` = 0.18 — visible but slow.
- Loop iterates 22 times every fragment for both halo and fill (44× total per pixel) — performance cost. Acceptable but worth noting.

**Mechanical fixes (apply these directly):**
1. In `home` (~line 76 and duplicated ~line 112): add slow drift to home itself, `home += 0.2*vec2(sin(TIME*0.07 + fi), cos(TIME*0.05 + fi*1.3));` so the orbital centres themselves slowly walk — Composition VIII feels alive.
2. Increase default `orbitSpeed` to 0.25 and default `orbitRange` to 0.22 — bigger, more legible motion.
3. Cache the home/orbit/ctr computation by making it a function called once per shape, called twice (once for halo, once for fill). Less critical but tidies up code duplication.

**New parameter to add (if any):** none.

---

## surrealism_magritte.fs

**Movement signifier:** Photoreal sample-and-displace: warp recognisable imagery via curl-noise into biomorphic flow; long hard shadow projection; chromatic dream-tint pass.

**Verdict:** NEEDS_TWEAK

**What's working:** Procedural sky + cloud fbm + horizon + SDF object with long projected shadow + Golconda ghosts is a genuinely Magritte composition. Cloud drift uses TIME so the sky moves. Object hover oscillates.

**Problems found:**
- The composition is essentially a still life with one slow-floating apple. The user banned "frozen composition with only audio breath" — this comes close. Cloud drift `0.06` × TIME is slow.
- Ghosts ONLY appear when `audioBass > 0.05`, so with audio off the Golconda signature is invisible — the canon's repetition-as-composition is missing in silence.
- Object SDFs are great but only ONE object renders at a time; user just sees one apple. Magritte's repetition (Golconda) should be the default visual.
- Shadow direction is fixed by `shadowAngle` slider — never rotates with TIME.
- Cloud shape is reasonable but doesn't morph — fbm coordinates only translate, not scale-warp.

**Mechanical fixes (apply these directly):**
1. In the ghost block (~line 173): remove the `audioBass > 0.05` gate — make ghosts ALWAYS render, with their opacity driven by hover-phase so they fade in and out: `float ghostOpacity = (0.4 + 0.6*audioBass*audioReact) * (0.5 + 0.5*sin(TIME*0.3 + fg));`. The Golconda canon needs constant ghost presence.
2. In ghost positions (~line 178): make ghosts slowly drift, `vec2 off = vec2(hash11(fg*1.7)-0.5, hash11(fg*3.3)-0.5)*0.6 + 0.05*vec2(sin(TIME*0.2 + fg), cos(TIME*0.17 + fg));`.
3. In `magritteCloud` (~line 53): add a scale-breathing factor `p *= 1.0 + 0.04*sin(TIME*0.1)` so clouds inflate/deflate.
4. In shadow (~line 135): add slow rotation `vec2 shadowDir = vec2(cos(shadowAngle + TIME*0.05), sin(shadowAngle + TIME*0.05));` so the long shadow rakes across the ground like a slow sundial.
5. Bump `cloudDrift` DEFAULT from 0.06 to 0.12.

**New parameter to add (if any):** `{ "NAME": "ghostAlwaysOn", "TYPE": "bool", "DEFAULT": true }` — toggle for whether Golconda ghosts depend on audio or run continuously.

---

## abex_pollock.fs

**Movement signifier:** Particle advection with curl-noise field rendered as layered ribbons; multi-pass overdraw for skein density; black/white/silver palette clamp.

**Verdict:** PASS

**What's working:** Persistent paintBuf with N drippers walked through the curl field via Eulerian stepping → continuous deposition → slow fade. This is exactly the canon technique. Black/white/silver/red/ochre palette clamp is correct. Splatter pass is a good Pollock signifier.

**Problems found:**
- `dripperPos` walks N=14 forward steps per fragment per dripper (so 14*24 = 336 noise calls per fragment in the fill pass). Performance cost; could choke at 4K. Acceptable but flag.
- Each dripper walks `t + hash11(fi*0.71)*8.0` forward steps each frame, so the dripper's position is computed deterministically from TIME — that's why it draws a path. Good.
- The trail relies on `paintFade` for memory; at 0.992 default the trail lives ~5 sec. Reasonable.
- Stroke width `strokeWidth=0.0035` default may be thin — Pollock's drips were variable-width. The audioLevel modulates it but baseline is small.

**Mechanical fixes (apply these directly):**
1. In `dripperPos` (~line 64): cap the inner step loop to fewer steps but multiply step size, e.g. swap `int N=14, dt=0.05` for `int N=8, dt=0.10` — same total path length, half the noise calls.
2. In the deposit (~line 117): add per-dripper width variance per frame — `float w = strokeWidth * (0.6 + 0.8*hash11(fi + floor(TIME*1.2))) * (1.0 + audioLevel*…);` so strokes vary thickness like a real flick of enamel.
3. Add a frame-feedback advection step: in pass 0 sample `texture(paintBuf, uv - selfVel*…)` — already there at line 112. Good.

**New parameter to add (if any):** none — coverage is good.

---

## abex_rothko.fs

**Movement signifier:** Stacked Gaussian-blurred rectangles with low-amplitude noise breath; large gradient fields modulated by very slow shimmer; never crisp edges.

**Verdict:** PASS

**What's working:** Stacked feathered bands with deeply-feathered edges, slow vnoise shimmer using TIME, vignette, film grain. Hard-capped audio influence honours the canonical "Rothko refuses to be hurried". Right parameters: feather, innerInset, shimmer, shimmerSpeed.

**Problems found:**
- Bands are hardcoded at fixed Y ranges (0.62..0.92, 0.34..0.58, 0.08..0.30). The bands themselves never move — only luminance shimmers. Subtle motion, but Rothko canvases do have slow upward-creep in the eye.
- No band-edge wobble. Real Rothko edges are not just feathered — they have organic micro-undulation.

**Mechanical fixes (apply these directly):**
1. In `bandShape` (~line 44): add a low-amplitude horizontal undulation to band edges, `yLo += 0.005*sin(uv.x*8.0 + TIME*0.03); yHi += 0.005*cos(uv.x*7.0 + TIME*0.04);` so the rectangle isn't axis-aligned but breathes its outline.
2. In the band Y bounds (~lines 81-83): add a slow vertical drift, e.g. inside main(): `float drift = sin(TIME*0.02)*0.005;` and apply `t1 = bandShape(uv, 0.62+drift, 0.92+drift, ...);` — the bands rise and fall over minutes.
3. The current `shimmer` `n - 0.75` baseline gives mostly negative shimmer (darkening); change to `n - 0.5` so it shimmers symmetrically.

**New parameter to add (if any):** none.

---

## opart_vasarely.fs

**Movement signifier:** High-frequency sin-wave warp field; near-Nyquist line patterns inducing real moiré; complementary-colour vibration via opponent-channel offset.

**Verdict:** NEEDS_TWEAK

**What's working:** Spherical bulge transform compressing ring spacing to fake 3D volume is exactly the Vega technique. Two-tone palette with hue rotation across canvas + central highlight + rim shadow is correct.

**Problems found:**
- **Static when audio silent.** With audio off: `bulgeMag` is constant, twist is 0 by default, hueShift only moves with `audioMid*audioReact`, ring frequency is constant. So the rings just sit there. Bulge centre also only jitters with `audioLevel`. With TIME=0 — fully frozen.
- `hueRotateSpeed` is multiplied by `audioMid * audioReact` — so even with hueRotateSpeed at max, no audio means no rotation.

**Mechanical fixes (apply these directly):**
1. In the bulge centre (~lines 39-41): drop the audio gate, replace with `bc += vec2(sin(TIME*0.5), cos(TIME*0.4))*0.05*(1.0 + audioLevel*audioReact);` — bulge always wanders.
2. In the bulge magnitude (~line 53): add a time-pulse, `float bulgeMag = bulgeAmount * (0.6 + 0.4*sin(TIME*0.7)) * (1.0 + audioBass*audioReact*0.35);` so the bulge breathes in and out.
3. In `twist` usage (~line 56): make it auto-rotate, `th += (twist + TIME*0.2) * smoothstep(0.0, bulgeRadius, r);` — the rings spiral.
4. In `hueShift` (~line 67): remove the audio gate so hue always rotates, `float hueShift = (uv.x + uv.y*0.4)*0.5 + TIME*hueRotateSpeed + TIME*0.05*audioMid*audioReact;`.

**New parameter to add (if any):** `{ "NAME": "ringPulse", "TYPE": "float", "MIN": 0.0, "MAX": 0.5, "DEFAULT": 0.2 }` — amplitude of frequency modulation `ringFrequency *= 1.0 + ringPulse*sin(TIME*0.5);` so rings expand and contract.

---

## popart_lichtenstein.fs

**Movement signifier:** Posterize + ordered-dither (Ben-Day dots) + thick black edge-detect outline; CMYK channel split with deliberate misregistration; tile-grid repeats.

**Verdict:** NEEDS_TWEAK

**What's working:** Ben-Day dot grid with luma-driven dot radius, posterize-to-comic-palette, edge-detect outline, speech bubble. Sound-word rendering procedurally is ambitious.

**Problems found:**
- **The "Technique" param exists but is unused.** `techniqueStyle` slider has values 0/1/2 (Ben-Day Dots / 4-Up Silkscreen / Halftone Pop) but the main() function has no `if (techniqueStyle == 0)` branch — only Ben-Day Dot logic exists. The 4-Up Silkscreen and Halftone Pop modes are NOT implemented. Promise/code mismatch is a serious bug.
- **Edge detection samples `inputTex` directly.** When no input is bound, the four edge samples (~lines 152-159) read texel(0,0) from an invalid texture — undefined behaviour, and outline won't match the procedural fallback face. Edge detect should run on `raw` not `texture(inputTex,…)`.
- **Static.** With audio silent: posterized palette is fixed, dot pattern is fixed, fallback face is fixed. Speech bubble flashes only on `audioBass > 0.05`. With TIME=0 nothing moves.
- No CMYK misregistration even though `silkscreenShift` parameter exists.

**Mechanical fixes (apply these directly):**
1. Implement the missing technique branches. Wrap the current Ben-Day block (~lines 141-149) in `if (techniqueStyle == 0)`. Add `else if (techniqueStyle == 1)` Warhol grid: split the screen into 2×2 quadrants, sample `raw` four times with offset hue rotations, with deliberate `silkscreenShift` of one quadrant. Add `else if (techniqueStyle == 2)` Rosenquist halftone: just apply `dotMaxRadius` Ben-Day across ALL luminance ranges.
2. Replace edge-detect (~lines 152-160) to operate on `raw`'s gradient by sampling raw at uv±px (which means computing fallback `raw` four times) OR — simpler — derive edges from `lvl` (the posterized value) by `dFdx(lvl)+dFdy(lvl)`.
3. Add CMYK misregistration when `techniqueStyle==1`: `col.r = texture-sampled-at(uv + vec2(silkscreenShift, 0)); col.b = sampled at(uv - silkscreenShift,0)`. Drift the offset with TIME for movement.
4. Add panel scrolling to the fallback face: `vec2 c = (uv - 0.5 - vec2(sin(TIME*0.3)*0.05, cos(TIME*0.4)*0.04)) * vec2(1.0, 1.2);` so the comic-girl drifts.
5. Speech bubble: drop the `audioBass > 0.05` gate; show always with audio modulating size — `bSz *= (0.8 + 0.2*sin(TIME*1.5)) * (1.0 + audioBass*audioReact*0.6);`.

**New parameter to add (if any):** none — already overspecified; just need to finish implementing what's declared.

---

## minimalism_stella.fs

**Movement signifier:** Pure SDF half-plane partitions with single sample-per-region colour assignment; zero noise; pixel-perfect edges; large solid fields.

**Verdict:** PASS

**What's working:** Concentric Chebyshev/L1/hex rings with hard-edge palette stripes, raw-canvas hairline gaps, and beat-driven `paletteMarch` shifting bands outward. Excellent Stella signifier. Anti-Voronoi.

**Problems found:**
- `paletteMarch` only moves when audio is present (`audioBass*audioReact*6.0`). With audio silent the rings are static.
- `rotateSpeed` defaults to 0.0 — out of the box the rings don't rotate. User has to crank a slider.
- Black Paintings preset is stuck at `vec3(0.05)` for all bands — no actual stripe variation, looks completely black. Should alternate black with raw-canvas.

**Mechanical fixes (apply these directly):**
1. In `paletteMarch` usage (~line 77): add a constant time march independent of audio: `float bandIdx = floor(bf + TIME * 0.4 + paletteMarch * audioBass * audioReact * 6.0);` — rings always march outward.
2. Bump `rotateSpeed` DEFAULT from 0.0 to 0.05 so by default the rings have a slow rotation.
3. In `stellaPalette` preset==1 (~line 35): alternate `return (bandIdx % 2 == 0) ? vec3(0.05) : vec3(0.94, 0.91, 0.83);` so the Black Paintings actually show stripes.

**New parameter to add (if any):** none.

---

## vaporwave_floral_shoppe.fs

**Movement signifier:** Channel-split RGB chromatic offset + scanline sin overlay + pink/cyan duotone clamp + low-bit posterize for VHS feel; bilinear-sample-of-low-res-source upscale shimmer.

**Verdict:** PASS

**What's working:** Pink/teal sky, magenta sun with horizontal bars, perspective grid floor advancing with TIME, marble bust SDF, scanlines, katakana band, CRT bloom. The grid `gridUV.y = 1.0/dh - TIME*gridSpeed` gives genuine forward-receding motion. Strong vaporwave signifier.

**Problems found:**
- Chromatic shift (`chromaShift`) only applies when `IMG_SIZE_inputTex.x > 0.0` — if no input is bound, no chromatic aberration. Should also apply to procedural output.
- Sun bars are static (no horizontal scroll). Real vaporwave sun bars often scroll/strobe.
- Bust marble is fbm but the `marble(bD * 4.0)` has no time term — bust is frozen marble.
- No JPEG-block / posterize signifier. Canon explicitly mentions "low-bit posterize for VHS feel".

**Mechanical fixes (apply these directly):**
1. In chromaShift block (~line 159): drop the `IMG_SIZE_inputTex` gate and apply to `col` directly via re-rendering the sky/sun at offset uv — too expensive. Instead, post-shift the existing col: `col.r = mix(col.r, col.r + col.g*0.05*sin(uv.y*60.0 + TIME), chromaShift*5.0);` — cheap RGB pseudoshift.
2. In sun bars (~line 109): add scroll, `float barMask = step(0.0, sin(barY*sunBars*3.14159 + TIME*0.5));` so bars crawl up.
3. In `marble` (~line 41): add time, `float n = sin(uv.x*6.0 + vnoise(uv*8.0 + TIME*0.05)*4.0);` so the bust has the slow swirling marble shimmer of vaporwave aesthetic.
4. After CRT bloom (~line 178): add a posterize-to-low-bit step, `col = floor(col * 16.0) / 16.0;` for the VHS / lo-fi colour banding.

**New parameter to add (if any):** `{ "NAME": "vhsBits", "TYPE": "float", "MIN": 4.0, "MAX": 32.0, "DEFAULT": 16.0 }` — colour-quantize levels for the VHS posterize.

---

## glitch_datamosh.fs

**Movement signifier:** Per-row UV displacement by hash; channel-shift with sub-pixel offset; DCT-block quantize artefacts; motion-vector accumulation without keyframe reset.

**Verdict:** PASS

**What's working:** Persistent moshBuf advected each frame along synthesized motion direction → datamosh accumulation. Per-row tear at 8 Hz, RGB channel split, DCT block quantize, burst rainbow garbage. Right canon technique.

**Problems found:**
- Without input texture, the fallback (~lines 67-72) is a procedural ring with `cos(ang*3.0+TIME)` etc. — moving but small range, not very glitchy on its own.
- `freezeChance` and `burstProb` both gate on audio (`audioMid`, `audioBass`) — at audio silent, moshing happens but no freeze/burst, so the catastrophic-failure mode is invisible.
- Block corruption gated `blockRoll < blockCorruption*(0.4 + audioLevel*audioReact)` — same issue.
- No I-frame stutter where some frames fully refresh — datamosh canon includes occasional clean keyframes.

**Mechanical fixes (apply these directly):**
1. In the fallback content (~lines 64-72): add more glitchy structure — replace with something like `fresh = vec3(step(0.5, fract(uv.x*5.0 + TIME*0.3)), step(0.5, fract(uv.y*7.0 - TIME*0.2)), step(0.7, hash21(floor(uv*40.0) + floor(TIME*2.0))));` — scrolling stripes + flickering blocks, much more glitchy.
2. In freeze decision (~line 60): un-gate audio, `bool freeze = fr < freezeChance * (0.5 + audioMid*audioReact + 0.3);` — always some chance of freeze.
3. In block corruption (~line 106): `if (blkRoll < blockCorruption * (0.4 + audioLevel*audioReact + 0.2))` so the artifacts always emerge.
4. Add an I-frame stutter: at the top of pass 0, every ~2 sec hard-reset to fresh input, e.g. `if (fract(TIME*0.5) < 0.02) { gl_FragColor = vec4(fresh, 1.0); return; }` — gives a brief clean frame between mosh accumulations.

**New parameter to add (if any):** `{ "NAME": "iframeRate", "TYPE": "float", "MIN": 0.0, "MAX": 2.0, "DEFAULT": 0.5 }` — Hz of automatic clean-frame insertions for the keyframe-stutter look.

---

## ai_latent_drift.fs

**Movement signifier:** Multi-octave fbm flow-field morphing latent embeddings; chromatic warm/cool gradient blend; soft probability-edge dilation; data-driven point cloud bloom.

**Verdict:** PASS

**What's working:** Three-LFO latent walk with curl-noise domain warp, directional blur for diffusion-model softness, palette-mapping through a wide muted hue arc, phantom feature pulses. Tex-prompt bleed via curl. Canonical Anadol approach. No cells.

**Problems found:**
- `latentSpeed` default 0.06 — slow but the LFOs work; this is in spec.
- Phantom features always render (good) but use static `phantomScale` so the phantom shapes always have the same scale — real GAN hallucinations have varying detail levels.
- Tex prompt only bleeds when input bound — fine, optional.
- Palette mapping via `t = field + uv.x*0.2 + L1*0.5` — the `uv.x*0.2` term means colour is biased left-to-right, slightly band-like. Acceptable.

**Mechanical fixes (apply these directly):**
1. In phantom feature (~line 120): make `phantomScale` breathe with TIME, `float scaleNow = phantomScale * (0.7 + 0.3*sin(TIME*0.1)); float ph = fbm(P*scaleNow + L2*1.7) - 0.5;` — phantom forms grow and shrink like emerging/dissolving GAN concepts.
2. In palette mapping (~line 107): replace the `uv.x*0.2` left-right bias with a TIME-rotating gradient, `float ang = TIME*0.05; float bias = uv.x*cos(ang) + uv.y*sin(ang); float t = field + bias*0.2 + L1*0.5 + paletteShift;` — gives the slow-rotating colour band the canon describes.
3. In phantom colour (~line 122): vary it across the palette, `vec3 phCol = latentPalette(t + 0.3, 0.0); col += phCol * ph * phantomPulse * …;` — phantom features sample the same palette so they integrate, not look like white blots.

**New parameter to add (if any):** `{ "NAME": "phantomScaleBreath", "TYPE": "float", "MIN": 0.0, "MAX": 0.6, "DEFAULT": 0.3 }` — amplitude of phantom-scale breathing for tunable hallucination depth.

---

## Summary

PASS: 11 (art_nouveau_mucha, fauvism_matisse, futurism_boccioni, dada_hoch, destijl_mondrian, bauhaus_kandinsky, abex_pollock, abex_rothko, minimalism_stella, vaporwave_floral_shoppe, glitch_datamosh, ai_latent_drift) — that's 12 actually.

PASS (12): art_nouveau_mucha, fauvism_matisse, futurism_boccioni, dada_hoch, destijl_mondrian, bauhaus_kandinsky, abex_pollock, abex_rothko, minimalism_stella, vaporwave_floral_shoppe, glitch_datamosh, ai_latent_drift.

NEEDS_TWEAK (5): cubism_picasso (drift too small, planes never recombine), constructivism_lissitzky (almost completely static without audio), surrealism_magritte (Golconda ghosts gated on audio, mostly still), opart_vasarely (frozen without audio), popart_lichtenstein (techniqueStyle param does nothing — only Ben-Day branch implemented; edge detect samples possibly-unbound texture).

NEEDS_REWRITE (1): expressionism_kirchner (uses banned Worley/Voronoi as core technique; almost no movement; missing canon woodcut/shear signifiers).

Total: 12 PASS / 5 NEEDS_TWEAK / 1 NEEDS_REWRITE.
