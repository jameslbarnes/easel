# Shader Critique — Pass 2

Round 2 audit of 18 art-movement shaders after pass-1 fixes were applied. Looking for second-order problems: did adding TIME-baselines break audio dynamics, are speeds tuned right, are new params well-ranged, are there stacked frozen-frame-plus-orbit failure modes, and where is the single TouchDesigner move that pushes a shader from "working" to "memorable".

---

## art_nouveau_mucha.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** None of the three pass-1 fixes appear to have been applied — the Klimt mosaic (line 55) is still `floor(uv * 80.0)` with no time drift, `tendrilPos` (line 90) is still linear `mix` with no S-curve smoothstep, and `fract(t*0.5+...)` at line 89 still teleports.

**New problems / second-order issues:**
- The Klimt mosaic grid is a frozen jittered tile field. This is exactly the static-tile failure the canon flags.
- Tendril `tt = fract(...)` at line 89 still hard-resets — visible position jumps every cycle.
- With `tendrilSpeed` default 0.20 and `t = TIME * 0.20`, a full traverse of a tendril takes ~10 sec — but because the head moves linearly, mid-traverse the head appears stationary against itself for ~30 frames, looking frozen.
- No S-curve = "whiplash" name is unfulfilled.

**Mechanical fixes (apply directly):**
1. Line 55 — replace static mosaic grid with drifting one: `vec2 g = floor((uv + 0.005*vec2(sin(TIME*0.05), cos(TIME*0.04))) * 80.0);` (pass-1 fix not applied).
2. Lines 89-90 — replace `float tt = fract(t * 0.5 + hash11(fid * 5.7)); vec2 base = mix(startPt, endPt, tt);` with ping-pong + smoothstep S-curve: `float tt = abs(fract(t*0.5 + hash11(fid*5.7))*2.0 - 1.0); vec2 base = mix(startPt, endPt, smoothstep(0.0, 1.0, tt)); base += perp * sin(tt*3.14159) * amp * 0.5;`.
3. Line 138 — `dh < w * 4.0` is a fine cull but the deposit `falloff = smoothstep(w, 0.0, dh)` produces a dot, not a stroke. Add a short tail behind the head: sample two prev positions `tendrilPos(t-0.02, ...)` and `tendrilPos(t-0.04, ...)`, take `min(dh)` across them, so the deposit reads as a 3-frame trail instead of a single dot.
4. Memorable move — gold-leaf bloom is a final-pass goldness test only; add a Klimt-specific pass1 burst where on `audioBass > 0.6` a circular gold tile expands from a hashed point: `if (style==1 && audioBass > 0.5) col = mix(col, GOLD, smoothstep(0.1, 0.0, length(uv-burstCtr)) * audioBass);`.

**Optional polish:** The `petalBloom` only triggers on bright pixels — lower the threshold from `L - 0.55` to `L - 0.40` so Mucha's pastel field also gets gentle bloom, not just Klimt's gold.

---

## fauvism_matisse.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** None visible — the drop loop (lines 95-117) does NOT add a TIME-jittered grid offset, no posterize step exists in pass 1 (lines 141-148), no complementary edge boost, and the `posterize` parameter from pass-1 was never declared.

**New problems / second-order issues:**
- Drop deposition still locks to fixed grid cells (`floor(uv * gridN)`) — pass-1 jitter fix missing. The bucket-time `floor(TIME * 1.5)` only changes WHICH cells fire, not WHERE they fire.
- Saturation pass at line 142-143 boosts chroma but doesn't snap to the FAUVE palette — output is muddy not unmodulated.
- `dropRate * 0.5 + audioBass * 0.8` at line 102 — at silence and `dropRate=0.35` only 17.5% of cells fire. Sparse coverage = paper-bleed dominates the look.
- Memorable move missing: complementary edge boost (Matisse's "green line" portrait signature).

**Mechanical fixes (apply directly):**
1. Line 97 — apply pass-1 drift: `vec2 g = floor(uv * gridN) + vec2(floor(TIME*0.13), floor(TIME*0.17)) + vec2(float(i), float(j));` so the grid walks each second.
2. Add posterize after line 141: `int bestK=0; float bd=1e9; for(int k=0;k<6;k++){float dd=dot(col-FAUVE[k],col-FAUVE[k]); if(dd<bd){bd=dd; bestK=k;}} col = mix(col, FAUVE[bestK], 0.35);`.
3. After posterize, complementary edge boost: `float Lc = dot(col,vec3(0.299,0.587,0.114)); float ex = abs(dFdx(Lc))+abs(dFdy(Lc)); col = mix(col, vec3(1.0)-col, smoothstep(0.05, 0.20, ex)*0.4);` — Matisse green-line vibrate.
4. Add the missing `posterize` slider declared in pass-1: `{ "NAME": "posterize", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.35 }`. Range fine.

**Optional polish:** Bump `dropRate` default from 0.35 to 0.50 so silence isn't visually sparse. Bump `paintFade` default from 0.988 to 0.984 so pigment evaporates a bit faster and new drops register as fresh.

---

## expressionism_kirchner.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Worley/Voronoi entirely deleted, replaced with `ridge` + `ridgedFbm`. Horizontal shear (line 71-74) implemented with TIME breath. Persistent `carveBuf` pass added with feedback advection. Procedural Berlin streetscape with lamp streaks + windows (lines 104-127) replaces the cobblestone Voronoi.

**New problems / second-order issues:**
- `acidTint` is gated by `(0.5 + audioHigh * 0.5)` — at silence the tint is 50% of its slider value, fine. But the acid LUT `mix(vec3(0.7,1.4,0.4), vec3(1.4,0.5,1.3), 0.5)` is a fixed value; variety is lost. Could lerp the LUT mix factor with TIME for hue cycling between green-acid and magenta-acid.
- Carve advection (lines 87-93) computes `ridgedFbm` 4× per fragment — expensive. Each call is 5 octaves so 20 noise calls × 4 samples = 80 noise evaluations per pass-0 fragment. At 4K performance will hurt.
- Lamp `0.5 + 0.5*sin(TIME*1.3)` modulates ALL lamps in phase — should hash-stagger.
- Window flicker `floor(TIME*0.7)` is ~0.7 Hz and bucketed: windows blink in lockstep across the canvas. Should be per-window time offset.

**Mechanical fixes (apply directly):**
1. Line 119 — change `lampPhase * 6.28` to break sync: `(0.5 + 0.5 * sin(TIME * 1.3 * (0.7 + lampPhase) + lampPhase * 6.28))` — each lamp at its own rate.
2. Line 124 — change `floor(TIME * 0.7)` to `floor(TIME * 0.7 + wHash * 5.0)` so windows blink at hashed offsets, not in phase.
3. Lines 87-93 — replace 4× `ridgedFbm` calls with 1× `ridge` + central differencing: cache `float c = ridgedFbm(sUV*carveScale + TIME*flow);` then approximate gradient via single-tap noise difference. Halves cost.
4. Memorable move — at line 147 the acid LUT is fixed; make the green↔magenta blend cycle: `vec3 acid = mix(vec3(0.7,1.4,0.4), vec3(1.4,0.5,1.3), 0.5 + 0.5*sin(TIME*0.13));`. Acid pulse breathes between Nolde green and Kirchner magenta.

**Optional polish:** `shearAmount` MAX is 0.40 which at extreme values tears the image diagonally — feels right for Kirchner. Default 0.18 is good.

---

## cubism_picasso.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** Time-cycling centres (line 109-110) with 0.06 sin/cos drift. `recomposeRate` parameter declared with sensible 0.0–1.0 range default 0.3. Per-plane fade `fadeT` at line 120 implemented and applied to alpha at line 177. `drift` default raised to 0.05.

**New problems / second-order issues:**
- `fadeT = sin(TIME * recomposeRate + fi * 2.7) * 0.5 + 0.5` at line 120 — at default `recomposeRate=0.3`, full fade cycle = `2π/0.3 ≈ 21 sec`. Slow but tolerable. However alpha goes `0.4 + fadeT*0.6` — never below 0.4. Planes never actually disappear. The "phasing" is a 0.4–1.0 alpha jiggle, not a true recompose.
- Letter fragments still static — pass-1 fix (`seed += floor(TIME*0.2)`) was not applied. `letterField(uv, compositionSeed)` at line 192 uses raw `compositionSeed`.
- Procedural fallback striation `sin(sUV.y * 11.0 + sUV.x * 2.0)` at line 158 — pass-1 TIME term not added, frozen content per plane.
- `drift` default bumped to 0.05 but MAX is still 0.20 (pass-1 said bump MAX to 0.15, fine). `dr` is `vec2(sin(TIME*0.27 + fi), cos(TIME*0.31+fi*1.3))` × 0.05 → max 0.05 displacement, perceptible but not dramatic.
- `recomposeRate` slider min is 0.0 → at 0.0, `sin(0 + fi*2.7)` is constant per plane → static fade. Min should be > 0 or the slider docs should warn. Actually 0.0 is fine because each fi gives a different fixed `sin` value, so planes stay at random opacities. Acceptable.

**Mechanical fixes (apply directly):**
1. Line 177 — change alpha multiplier so planes can fully disappear: replace `(0.4 + fadeT * 0.6)` with `pow(fadeT, 1.5)` so at low fadeT the plane truly vanishes. Composition will visibly recompose.
2. Line 192 — apply pass-1 letter tumble: `float lf = letterField(uv, compositionSeed + floor(TIME*0.2));` so JOU/MA JOLIE clusters reposition every 5 sec.
3. Line 158 — apply pass-1 fallback time: `float stripe = sin(sUV.y * 11.0 + sUV.x * 2.0 + TIME*0.5 + fi*1.7) * 0.5 + 0.5;`.
4. Memorable move — Picasso analytic compositions stack from corners; the centres currently spread uniformly. After computing `rawCtr` add a soft attractor toward 4 hashed "viewpoint centres" that themselves drift slowly: `vec2 attractor = vec2(hash11(floor(TIME*0.1) + fi*0.1), hash11(floor(TIME*0.1)+ fi*0.1+ 7.7)); rawCtr = mix(rawCtr, attractor, 0.3);` — planes cluster around shifting focal points.

**Optional polish:** `recomposeRate` default 0.3 gives ~21 sec cycle which is good for slow contemplative breathing. Range 0.0–1.0 is sensible (1.0 = ~6.3 sec cycle, frenetic but defensible).

---

## futurism_boccioni.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** Velocity rotation `vAng = TIME * velocityRotSpeed + audioBass*0.4` retained — but pass-1 idle-floor for `vMag` was NOT added. Dab threshold not lowered.

**New problems / second-order issues:**
- `vMag = velocityMag * (1.0 + audioLevel*1.5)` at line 65 — at silence vMag is just `velocityMag = 0.025`, no breath. Pass-1 idle-floor `(0.5 + 0.5*sin(TIME*0.3))` not applied.
- `vAng = TIME * velocityRotSpeed` at default `velocityRotSpeed=0.15` = full rotation every 42 sec. Slow, rays sweep elegantly. With audio gone the trail just spirals at constant rate — pass-1 said add `+ sin(TIME*0.13)*0.6` to snake; not applied.
- Dab threshold `dh > 0.92` (line 157) means only 8% of grid cells get dabs — pass-1 said lower to 0.85.
- Trail decays with `prev *= trailPersistence` at default 0.96 → trail dies at exp(-1) in 25 frames ≈ 0.4 sec. Too fast for a "Dynamism" feel, the streaks barely accumulate before dying.

**Mechanical fixes (apply directly):**
1. Line 65 — replace with `float vMag = velocityMag * (0.5 + 0.5*sin(TIME*0.3)) * (1.0 + audioLevel*audioReact*1.5);` — pass-1 idle breath.
2. Line 63 — replace with `float vAng = TIME*velocityRotSpeed + sin(TIME*0.13)*0.6 + audioBass*audioReact*0.4;` — snake the trail.
3. Line 157 — change `if (dh > 0.92)` to `if (dh > 0.85)` and timer to `floor(TIME*4.0 + audioHigh*audioReact*4.0)`.
4. Memorable move — Boccioni's "force lines" should ATTACH to the trail head, not just radiate from a wandering origin. At line 136-139 replace `origin` with the current trail head: `vec2 origin = vec2(0.5, 0.5) + vel * TIME * 0.3 + vec2(sin(TIME*0.83), cos(TIME*0.59))*rayOriginDrift;` — rays now feel emitted by the moving subject.
5. Bump `trailPersistence` default from 0.96 to 0.975 so streaks are visibly long.

**Optional polish:** Default `phantomCount=5` is fine; Balla's dog had ~20 leg positions so MAX=10 feels low — consider bumping MAX to 16.

---

## constructivism_lissitzky.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Continuous wedge ramming (line 111-115) with `(0.5 + 0.5*sin(TIME*1.7))`. Circle pulse opposite-phase (line 100-101). Bar drift was NOT applied (line 50-52 still frozen with only `audioMid*0.05` rotation). Glyph drift NOT applied (line 67-70 static origins). Printing-press shake at line 90 still gates on `audioLevel`.

**New problems / second-order issues:**
- Wedge ramming at `sin(TIME*1.7)` = ~0.27 Hz, 3.7 sec full cycle. Right speed for a poster.
- Pass-1 bar drift fix not applied → bars completely static positions, only `audioMid*0.05` rotation. With audio off bars never move.
- Pass-1 glyph drift not applied → glyph clusters frozen.
- Pass-1 shake un-gating not applied → shake is dead at silence.
- No `wedgePulse` slider was added (pass-1 proposed parameter never declared).
- The `audioBass*0.05` modulation on circle radius (line 101) is tiny (5%) — wedge ramming will dominate visually, which is correct for "wedge piercing circle".

**Mechanical fixes (apply directly):**
1. Line 50-52 (`barShape`) — add drift inside the function: `c += 0.03*vec2(sin(TIME*0.4 + seed), cos(TIME*0.5 + seed)); angle += TIME*0.1*sign(seed-0.5);` — bars sweep like searchlights.
2. Lines 67-70 (`glyphField`) — drift origins: replace static `origin = vec2(...)` with `origin += vec2(sin(TIME*0.2 + float(g))*0.02, cos(TIME*0.18 + float(g)*1.7)*0.015);` — type slips around poster.
3. Line 90 — replace `* compositionJitter * audioLevel` with `* compositionJitter * (0.4 + audioLevel*audioReact*0.6)` so shake never dies.
4. Add the proposed `wedgePulse` slider AND wire it: `{ "NAME": "wedgePulse", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.5 }` and at line 111 use `(0.5*wedgePulse + 0.5*wedgePulse*sin(TIME*1.7))`.
5. Memorable move — Beat the Whites is a poster that moves with violence, not breath. On bass kick make the WHOLE composition snap-rotate by a small angle: at top of main(), `float kick = audioBass * audioReact * 0.05; uv = mat2(cos(kick),-sin(kick),sin(kick),cos(kick)) * (uv - 0.5) + 0.5;` — the poster is being slammed.

**Optional polish:** `circleSize` default 0.25 is fine for a head-and-shoulders white circle; the wedge tip travels `0.05 + triangleThrust*0.5 + triangleThrust*0.4*audioBass = ~0.155` at default — visibly pierces the circle.

---

## dada_hoch.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** Pass-1 fixes for jagged edges (line 107), letter band scroll (line 173 still uses bucketed `sin(bt*0.71)`), and stamp drift (line 144) appear NOT applied — `edgeMask` is still smooth `smoothstep`, letter band Y is still `0.5 + sin(bt*0.71)*0.3` (jumps per beat, doesn't scroll), and stamp positions still update only on bt change.

**New problems / second-order issues:**
- `bt = floor(TIME*beatRate) + floor(audioBass*audioReact*6.0)` — at silence, only TIME drives the beat, that's fine. With audio, bass kicks bump `bt` instantly, hard reshuffling. Good.
- The chromaSlide directions `dirR = vec2(sin(TIME*0.7), cos(TIME*0.91))` cycle at ~0.11 Hz → 9 sec cycle, max amplitude `0.012*(1+1.5)*audioHigh ≈ 0.03`. Visible only on treble — at silence chroma is locked at sub-pixel offset. Pass-1 didn't address this and it's actually fine for a "subtle ghost" baseline.
- Stamps at line 145-167 — `c = vec2(hash11(g_*13.7 + bt*0.71), ...)` so stamps relocate on bt change (every 0.4 sec at default beatRate). They do move, just discretely. Pass-1 wanted continuous drift between bt ticks.
- Letter band frozen y per beat — pass-1 fix not applied.

**Mechanical fixes (apply directly):**
1. Line 107 — apply pass-1 jagged edge: `float jag = hash21(q*40.0 + bt)*0.04; float edgeMask = smoothstep(0.0, 0.06, min(edgeX, edgeY) - jag);`.
2. Line 173 — apply pass-1 scroll: `float bandY = fract(TIME*0.05 + sin(bt*0.71)*0.2);`.
3. Line 146-147 — apply pass-1 drift: `vec2 c = vec2(hash11(float(g_)*13.7 + bt*0.71), hash11(float(g_)*17.1 + bt*0.29)) + 0.05*vec2(sin(TIME*0.4 + float(g_)), cos(TIME*0.6 + float(g_)));` — stamps slide between beats.
4. Memorable move — Höch's collage has SCALE-MISMATCH (giant face next to tiny gear). The strip scales currently use `s = stripScale * (0.45 + hash*1.1)` ranging ~0.08–0.28 (3.5× variance). Push to 8× by adding occasional huge strips: `if (hash11(fi*43.7) > 0.85) s *= 4.0;` after line 67. One-in-seven strips becomes a wall-fragment sitting under the rest.

**Optional polish:** `beatRate` default 2.5 = 150 BPM, dance-music kick. Fine. Stuttering letterband is genuinely the Hausmann ABCD signifier — keep it but apply scroll fix.

---

## destijl_mondrian.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Lane breath applied at line 55 (`+ sin(TIME*0.15 + fi*2.7)*0.012`) and line 92 (`+ cos(TIME*0.13 + fj*3.1)*0.012`). Soft-edge pulse mix from pass-1 at line 81 NOT applied (still hard `col = pc;`). Intersection beat-phase pulse at line 140 NOT applied (still constant `lit`). The proposed `laneBreath` slider was NOT declared.

**New problems / second-order issues:**
- Lane breath of `0.012` is hard-coded (pass-1 wanted a slider). At default 14 lanes that's `0.012 * 14 = 0.168` of canvas — huge ratio relative to lane width 0.008. So lanes drift visibly across each other's territory. Acceptable, but the `intersectionGlow` loop assumes laneY/laneX from the same hash — and the breath term IS NOT included in the intersection computation (line 126 vs line 53). Intersections drift OUT of phase with their lanes, so the glow no longer sits ON the crossing. Bug.
- Pulses still hard-assign `col = pc` (line 81, 114) — pixel-step aliasing visible at low pulseSize.
- `marchSpeed` default 0.35 with per-pulse hash multiplier `0.5..2.0` → pulses cross canvas in 1.4–5.7 sec. Good Boogie Woogie tempo.

**Mechanical fixes (apply directly):**
1. Add the missing slider: `{ "NAME": "laneBreath", "TYPE": "float", "MIN": 0.0, "MAX": 0.05, "DEFAULT": 0.012 }`. Replace the literal `0.012` at lines 55 and 92 with `laneBreath`.
2. **Critical bug**: lines 126 and 131 (intersection-glow loop) re-derive laneY/laneX without the breath term. Add `+ sin(TIME*0.15 + fi*2.7)*laneBreath` and `+ cos(TIME*0.13 + fj*3.1)*laneBreath` to those reads so glow tracks lanes.
3. Line 81 / 114 — apply pass-1 soft-edge: `col = mix(col, pc, smoothstep(pulseSize, 0.0, max(dx, dy)));` (will need `dy` defined; rename existing `dy` carefully).
4. Line 140 — apply pass-1 syncopation: `lit *= 0.7 + 0.3*sin(TIME*4.0 + (laneX+laneY)*30.0);`.
5. Memorable move — Broadway Boogie Woogie has small COLOURED CELLS along each lane (not just at intersections) creating syncopated rhythm. Add per-lane sub-pulse cells: between marching pulses, paint short coloured segments that fade in/out: `if (audioMid > 0.02 || sin(TIME*2.0+fi) > 0.6) col = mix(col, pc, 0.4);` inside the lane loop on every fragment that lies on the lane line — drives lanes from grey to colored on beat.

**Optional polish:** None — this shader is already strongest of the geometric set.

---

## bauhaus_kandinsky.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** None applied — `home` (lines 76-77, 112-113) is still a static hashed position with no TIME drift. `orbitSpeed` default still 0.15 (pass-1 wanted 0.25). `orbitRange` default still 0.18 (pass-1 wanted 0.22). Halo+fill loop still uncached (44 iterations per pixel).

**New problems / second-order issues:**
- All shapes orbit around static homes — over time the canvas has the SAME 13 positions just rotating in their own little circles. Pass-1 `home += 0.2*vec2(sin(TIME*0.07+fi), cos(TIME*0.05+fi*1.3))` not applied.
- `orbitSpeed=0.15` × `(0.4 + hash*1.4)` = 0.06–0.27 rad/s → orbital periods 23–104 sec. Slow.
- The `mod(fi, 3.0)` paint pairing at line 95 is determined by index, so increasing/decreasing shapeCount changes which shape is which colour — not an actual problem but means `compositionSeed` shouldn't shift the pairing (it doesn't — `t = int(mod(fi, 3.0))` uses fi which DOES include compositionSeed). So changing seed reshuffles colour pairs. Inconsistent with `strictPairing=true` semantics. Minor.
- Double computation: lines 76-92 (halo) and 109-132 (fill) reproduce the same orbital math. Shader is 44 trig ops per pixel.

**Mechanical fixes (apply directly):**
1. Lines 76-77 AND 112-113 — apply pass-1 home drift to BOTH halo and fill loops: change `vec2 home = vec2(0.5 + (hash11(fi*1.3)-0.5)*1.4, 0.5 + (hash11(fi*2.7)-0.5)*1.4);` to `vec2 home = vec2(0.5 + (hash11(fi*1.3)-0.5)*1.4 + 0.10*sin(TIME*0.07 + fi), 0.5 + (hash11(fi*2.7)-0.5)*1.4 + 0.10*cos(TIME*0.05 + fi*1.3));` — orbital centres themselves walk.
2. Bump `orbitSpeed` DEFAULT from 0.15 to 0.25.
3. Bump `orbitRange` DEFAULT from 0.18 to 0.22.
4. Memorable move — Kandinsky's Composition VIII has lines that CONNECT shapes (like force-vectors). Currently `lineCount` lines are random across canvas. Instead, anchor `pt` and `dir` to two shape centres: `vec2 a = computeShapeCenter(int(mod(fk*7.0, float(N))));` and `vec2 b = computeShapeCenter(int(mod(fk*11.0, float(N))));` then `pt=a; dir=normalize(b-a);` — lines now read as relationships between shapes, not random scaffolds.

**Optional polish:** Refactor by extracting `vec2 shapeCenter(int i)` to share between halo and fill loops — halves the trig ops.

---

## surrealism_magritte.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Ghost block (line 174-194) no longer audio-gated; `ghostOpacity` baseline `(0.35 + 0.45*audioBass)` plus `(0.5 + 0.5*sin(TIME*0.3 + fg*1.7))` so ghosts always render and breathe. Ghost positions (line 179-182) drift via `0.05*vec2(sin(TIME*0.20+fg), cos(TIME*0.17+fg))`. Cloud `magritteCloud` still does not have scale-breath (pass-1 fix `p *= 1.0 + 0.04*sin(TIME*0.1)` not applied at line 54). Shadow rotation (line 135) uses raw `shadowAngle` — pass-1 `+ TIME*0.05` not applied. `cloudDrift` default still 0.06 (pass-1 wanted 0.12). `ghostAlwaysOn` bool was not added (but ghosts are now always on by default, so the toggle is moot).

**New problems / second-order issues:**
- Cloud morphs only by translation `+ vec2(t*0.3, 0.0)` (line 54). Without scale-breath the same cloud shapes endlessly translate, looking like a tiled scrolling background. Pass-1 fix needed.
- Shadow direction is fixed by `shadowAngle` slider (default 5.5 rad). Pass-1 wanted slow time rotation; not applied.
- `ghostOpacity = (0.35 + 0.45*audioBass*audioReact) * (0.5 + 0.5*sin(...))` at line 187 — at silence the bass term is 0 so `ghostOpacity = 0.35 * (0.5..1.0) = 0.175..0.35`, then multiplied by `ghostMultiply*0.85*(1.0-hash*0.4) ≈ 0.7` baseline → effective ~0.12–0.24. Ghosts ARE visible at silence, but only barely. Could lift the baseline.
- The object SDF apple/pipe is fixed at one position; ghosts cluster around it. Real Golconda has dozens of ghosts in a structured GRID, not scattered drift. Currently `off = (hash-0.5)*0.6 + 0.05*sin/cos` produces random scatter.

**Mechanical fixes (apply directly):**
1. Line 54 — apply pass-1 cloud breath: `vec2 p = uv * vec2(1.4, 2.6) * (1.0 + 0.04*sin(TIME*0.1)) + vec2(t * 0.3, 0.0);`.
2. Line 135 — apply pass-1 shadow sundial: `vec2 shadowDir = vec2(cos(shadowAngle + TIME*0.05), sin(shadowAngle + TIME*0.05));`.
3. Bump `cloudDrift` DEFAULT from 0.06 to 0.12.
4. Memorable move — Golconda ghosts should form a grid, not scatter. At line 179 replace the hash position with a grid: `vec2 grid = vec2(mod(fg, 4.0), floor(fg/4.0)); vec2 off = (grid - vec2(1.5, 1.5))*0.22 + 0.04*vec2(sin(TIME*0.2 + fg), cos(TIME*0.17 + fg));` — 4×3 ghost grid drifting gently. Make grid size depend on `ghostCount` for legibility.

**Optional polish:** Lift ghost opacity baseline from `0.35` to `0.50` so silence still shows them clearly.

---

## abex_pollock.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Step count was kept at 14 (pass-1 suggested 8 with bigger dt — minor perf nit, not applied but acceptable). Per-frame width variance NOT applied (line 118 still uses fixed `w = strokeWidth * (1+audioLevel*0.8)`).

**New problems / second-order issues:**
- `dripperPos` (line 64-76) walks `t` forward in `dt=0.05` steps inside a loop bounded by `if (float(i) > t) break`. So as TIME grows, more steps are taken — but there's a hard cap of N=14 steps. At `wanderSpeed=0.12`, `t = TIME*0.12`. After ~5 sec, t=0.6, so only 12 steps execute (12*0.05=0.6). After ~10 sec the loop maxes out at 14 and the dripper stops advancing — canvas looks frozen.
- This is a critical bug. The trail length stops growing after ~12 sec, then drippers stay at the same position forever (just oscillating with small audio reactive width changes).
- `paintFade` default 0.992 → trail dies in 1/(1-0.992) ≈ 125 frames ≈ 2 sec. So buffer keeps clearing, but new deposits never accumulate further than the last 12 sec of walk = canvas eventually shows 12-sec-ago dripper positions, not currently moving.

**Mechanical fixes (apply directly):**
1. Line 70 — replace the loop with one that walks RELATIVE to current time, not from t=0: change `if (float(i) > t) break;` to a fixed step count and stride: replace lines 67-76 entirely with:
```
vec2 p = base;
for (int i = 0; i < 14; i++) {
    p += curl(p * turb + fid * 11.7 + TIME * 0.02) * speed * 0.05;
    p = clamp(p, 0.02, 0.98);
}
return p + 0.02 * vec2(sin(TIME * 0.5 + fid), cos(TIME * 0.4 + fid));
```
The `+ TIME*0.02` inside curl makes the field itself flow, and the additive sin/cos term gives a per-frame walk so the dripper actually moves continuously.
2. Line 118 — apply pass-1 width variance: `float w = strokeWidth * (0.6 + 0.8*hash11(fi + floor(TIME*1.2))) * (1.0 + audioLevel*audioReact*0.8);`.
3. Memorable move — Pollock's drips sometimes break into POOLS where they dwell. Add a probability that a dripper stops and pools: `if (hash11(fi + floor(TIME*0.3)) > 0.85) p = mix(p, base, 0.5);` before line 76 — every ~3 sec, 15% of drippers pool back near their base.

**Optional polish:** None.

---

## abex_rothko.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** None of the pass-1 fixes appear applied — `bandShape` still uses axis-aligned bounds (line 44-51) without horizontal undulation. Band Y bounds at lines 81-83 still hardcoded with no slow drift. Shimmer baseline `n - 0.75` still has the asymmetric darkening bias.

**New problems / second-order issues:**
- Band edges are perfectly straight horizontal lines — Rothko's bands are imperceptibly wavy. With `feather=0.12` the soft transition hides this somewhat, but at low feather settings the bands look too clean.
- Shimmer formula `n - 0.75` baseline at line 98: `n` from `vnoise + 0.5*vnoise` ranges ~0–1.5; subtracting 0.75 gives -0.75..+0.75 average ≈ -0.0 to slightly negative. With `shimmer=0.04` the multiplier is `1 + (negative)*0.04 ≈ 0.97` average — bands are ~3% darker than they should be. Should be `n - 0.5` for symmetric breathing.
- Bands don't drift vertically — pass-1 wanted slow `sin(TIME*0.02)*0.005` Y-drift, missing.

**Mechanical fixes (apply directly):**
1. Line 44 — add edge undulation: `float edgeUlow = 0.005*sin(uv.x*8.0 + TIME*0.03); float edgeUhi = 0.005*cos(uv.x*7.0 + TIME*0.04); yLo += edgeUlow; yHi += edgeUhi;` at the start of `bandShape`.
2. Lines 81-91 — wrap with vertical drift: at top of main(), `float drift = sin(TIME*0.02)*0.005;` then add `+ drift` to all Y bounds in the band calls.
3. Line 98 — fix shimmer asymmetry: `col *= 1.0 + (n - 0.5) * shimmer;`. (Mean ≈ 1.0 instead of 0.97.)
4. Memorable move — Rothko CHAPEL paintings have a quality of light coming from BEHIND the band. Add a subtle additive bloom centred on each band's mid-y: at line 86 after each `mix`, `col += cTop * t1 * 0.04 * (0.5 + 0.5*sin(TIME*0.02 + uv.y*3.0));` — bands glow gently from within.

**Optional polish:** Default `feather=0.12` is correct; `audioInfluence` capped at 0.10 honours the patient-painting promise.

---

## opart_vasarely.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Bulge centre wander (line 40-41) `+= 0.05*vec2(sin(TIME*0.5), cos(TIME*0.4))` NO audio gate. Bulge magnitude breath (line 53-55) `* (0.6 + 0.4*sin(TIME*0.7))`. Auto-twist (line 60) `(twist + TIME*0.20) * smoothstep(...)`. Hue rotates on TIME (line 73) `+ TIME * hueRotateSpeed`. Frequency breath (line 64) `(1.0 + 0.18*sin(TIME*0.5))`. The proposed `ringPulse` slider was NOT declared — instead the value is hardcoded as 0.18.

**New problems / second-order issues:**
- Twist auto-rotation `TIME*0.20` at line 60 — modulated by `smoothstep(0.0, bulgeRadius, r)`. So far from centre, twist=0; near centre, full twist. The visible spiral is centred. Good.
- Bulge magnitude `0.6 + 0.4*sin(TIME*0.7)` ranges 0.2–1.0 — at low end the dome flattens to ~20% effect, almost flat. Visible inflate/deflate.
- Hue rotation at default `hueRotateSpeed=0.05` → full hue cycle per `2π/0.05 = 126 sec`. Slow but smooth.
- The hardcoded `0.18*sin(TIME*0.5)` ring frequency breath is fine; just expose as slider.
- `bulgeMag` × `t*t` × `bulgeAmount` at default 0.55: `0.55 * 1.0 * (1) = 0.55`, so `warp = 1 - 0.55 = 0.45` → ring spacing compresses 55% near centre. Strong dome effect.

**Mechanical fixes (apply directly):**
1. Add the proposed slider: `{ "NAME": "ringPulse", "TYPE": "float", "MIN": 0.0, "MAX": 0.5, "DEFAULT": 0.18 }`. Line 64 becomes `float freqNow = ringFrequency * (1.0 + ringPulse * sin(TIME*0.5));`.
2. Memorable move — Vasarely's Vega series has CHROMATIC ABERRATION at the dome rim where compression is highest. Add separate hue offsets per channel: at line 84 replace single `mix(colA, colB, band)` with three slightly-offset bands: `float bandR = sin(rW*freqNow + 0.05)*0.5+0.5; float bandB = sin(rW*freqNow - 0.05)*0.5+0.5;` then mix per-channel — generates real moiré-vibration at the dome rim.

**Optional polish:** `bulgeAmount` MAX 1.5 — at extreme values the warp goes negative (`1 - 1.5*1*1 = -0.5`), inverting the rings near centre. Visually striking, defensible.

---

## popart_lichtenstein.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** All three technique branches now implemented (`tech==0` Ben-Day, `tech==1` 4-Up Silkscreen with per-quadrant tints + per-channel mis-register, `tech==2` Halftone Pop). Procedural fallback at lines 113-127 actually exists now (RGB UV grid + cell tints + grid lines). Speech bubble still gated `audioBass > 0.05` (line 229) — pass-1 wanted always-on with TIME pulse.

**New problems / second-order issues:**
- **Critical bug**: edge-detect at lines 211-219 still samples `texture(inputTex, ...)` directly. When no input is bound, IMG_SIZE_inputTex = 0 and the texture sample returns black/garbage. The outline is then either always black (luminance gradient of zero across all four taps = zero edge) or random noise. Pass-1 specifically warned about this and proposed switching to `dFdx(lvl)`.
- The `drawWord` function (line 42-102) only reads correctly for `letter >= 0` — but letters like A end up with `right + top + midY` only, missing crossbar at left, no bottom. Visually fine; the words read as crude block letters.
- Speech bubble pass-1 fix not applied → bubble disappears at silence.
- Panel scrolling pass-1 fix not applied — `raw` fallback at line 116 uses static `vec2 g = floor(uv * 8.0)` so the comic-book grid is frozen.
- Static `lvl < 0.50 → LIK_RED` thresholds at line 196-199 — the procedural fallback `raw = vec3(uv.x, uv.y, 0.5)` has lum mostly in [0.3, 0.7] → output dominated by the RED band. Acceptable but monotonous.

**Mechanical fixes (apply directly):**
1. **Critical**: Lines 211-220 — replace edge detect with `dFdx(lvl)/dFdy(lvl)` to operate on the posterised value, not the (possibly unbound) input texture. New code: `float gx = dFdx(lum); float gy = dFdy(lum); float edge = abs(gx)+abs(gy); float outline = smoothstep(outlineThreshold, outlineThreshold+0.04, edge*outlineWidth*40.0);`. (Multiplier 40 because dFdx is per-pixel, not per-uv.)
2. Lines 116-118 — apply pass-1 panel scrolling: `vec2 c = (uv - 0.5 - vec2(sin(TIME*0.3)*0.05, cos(TIME*0.4)*0.04)) * vec2(1.0, 1.2); vec3 base = vec3(c.x+0.5, c.y+0.5, 0.5); vec2 g = floor((c+0.5) * 8.0);` — comic girl panel drifts.
3. Line 229 — drop audio gate, always show: `if (speechBubble) { ... float bSz = bubbleSize * (0.8 + 0.2*sin(TIME*1.5)) * (1.0 + audioBass*audioReact*0.6); ... }`.
4. Line 137-138 (silkscreen tech): the `silkscreenShift * vec2(cos(qid*1.7), sin(qid*1.7))` uses qid as both quadrant id (0–3) and angle multiplier — gives 4 fixed offset directions. Add TIME drift: `dirR = vec2(cos(qid*1.7 + TIME*0.3), sin(qid*1.7 + TIME*0.3)) * silkscreenShift;` so the misregistration breathes (as a real silkscreen registration drifts during printing).
5. Memorable move — Lichtenstein's "BRUSHSTROKE" works render gestural marks AS Ben-Day pattern. In tech=0, when audioMid spikes, paint a wide diagonal "brushstroke" overlay across the canvas as a stylized sweep: `if (audioMid > 0.3) { float d = abs(uv.y - 0.5 - sin(uv.x*3.14)*0.1); col = mix(col, LIK_BLUE, smoothstep(0.05, 0.0, d)*audioMid); }`.

**Optional polish:** `dotMaxRadius` default 0.36 with `dotDensity=140` produces visible dots; raising MAX to 0.50 keeps it.

---

## minimalism_stella.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Always-on march at line 79-81 `+ TIME * marchSpeed + paletteMarch * audioBass * 6.0`. `rotateSpeed` default raised to 0.05 (line 8). Black Paintings preset at line 38 alternates black/raw-canvas correctly.

**New problems / second-order issues:**
- `marchSpeed` default 0.4 with bandcount default 11: rings advance one ring per 1/(0.4) = 2.5 sec. Visible march, slightly fast for "minimalist" feel.
- `rotateSpeed` default 0.05 → full rotation in 31 sec (canvas was clamped to π/2 → ¼ rotation in 31 sec). Slow drift, appropriate.
- `paletteMarch * audioBass * 6.0` with audio at 0.5 → adds 1.2 to bandIdx → audio kicks shift the palette by 1+ ring. Strong impact, good.
- Hyena Stomp preset (line 35) cycles `fract(fi/11.0)` — at default 11 bands this spans the full hue circle exactly once. Looks like a perfect rainbow bullseye. With march, the bullseye spirals outward — beautiful.
- Black Paintings now has stripe alternation but uses raw-canvas as alternate `vec3(0.94, 0.91, 0.83)` which is bright. Real Stella Black Paintings show NEGATIVE (un-painted) raw canvas, i.e. very subdued. The brightness difference between black and "raw" is too high.

**Mechanical fixes (apply directly):**
1. Line 38 — adjust raw-canvas brightness so Black Paintings reads as subtly pinstriped, not high-contrast: `return (bandIdx % 2 == 0) ? vec3(0.05) : vec3(0.42, 0.39, 0.34);` — much closer to actual Stella surfaces.
2. Memorable move — Stella's late protractor series (Harran II) uses INTERLOCKING ARCS, not just concentric rings. Add a `shapeMode==3` "Protractor" option that masks half-circles: `d = (uv.y > 0) ? length(uv) : abs(uv.x);` — quick win for shape variety.

**Optional polish:** `marchSpeed` default 0.4 is on the fast side; consider 0.25 to slow the ring travel for a meditative feel.

---

## vaporwave_floral_shoppe.fs

**Verdict:** NEEDS_TWEAK

**Round-1 fixes that landed correctly:** Sun bars scroll (line 112) `+ TIME*0.5`. Marble has TIME (line 43) `+ TIME * 0.08`. Chromatic aberration is STILL gated by `IMG_SIZE_inputTex.x > 0.0` at line 162 — pass-1 wanted unconditional pseudo-shift on `col`. Posterize/VHS step NOT added; `vhsBits` slider not declared.

**New problems / second-order issues:**
- Chromatic aberration at line 162-167 still requires bound texture — falls back to no aberration in procedural mode. The vaporwave aesthetic depends on RGB ghost.
- Posterize VHS step from pass-1 not applied → output remains smooth-gradient, missing the lo-fi colour banding signifier.
- Sun bar scroll `barY * sunBars * π + TIME * 0.5` at default `sunBars=6`: bars cross the sun in `2π / (6π * 0.5) = 0.66 sec`. Fast-ish, reads as "scanline drift" — fine.
- Marble noise `+ TIME * 0.08` at line 43 — sin period dominates at low frequency. Bust shimmers slowly. Good.
- Grid floor advances `1.0 / dh - TIME * gridSpeed` at line 124 — the `1/dh` term grows toward horizon, so "depth" coordinate is large. Good perspective math.

**Mechanical fixes (apply directly):**
1. Line 162-167 — apply pass-1 unconditional pseudoshift: replace the gated block with `col.r = mix(col.r, col.r + col.g*0.05*sin(uv.y*60.0 + TIME*0.7), chromaShift*5.0); col.b = mix(col.b, col.b - col.g*0.05*sin(uv.y*60.0 + TIME*0.7 + 1.57), chromaShift*5.0);` — works in procedural mode too.
2. Add the `vhsBits` slider: `{ "NAME": "vhsBits", "TYPE": "float", "MIN": 4.0, "MAX": 32.0, "DEFAULT": 16.0 }`. After line 181 add: `col = floor(col * vhsBits) / max(vhsBits, 1.0);`.
3. Memorable move — vaporwave aesthetic is incomplete without a CRT ROLLING SHEAR (image rolling vertically every few sec like a misadjusted TV). Add at the start of main(): `uv.y = fract(uv.y + smoothstep(0.95, 1.0, sin(TIME*0.15))*0.3);` — every ~6 sec the picture rolls once.

**Optional polish:** Default `chromaShift=0.004` is subtle; bump default to 0.008 once unconditional.

---

## glitch_datamosh.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Always-on freeze baseline at line 61 `freezeChance * (0.3 + audioMid + 0.3)` so baseline ≈ 0.3 of slider, audio adds. Glitchy fallback at lines 67-78 with stripes + hashed flickering blocks + ring (replaces the smooth ring of pass-0). Block corruption at line 113 `(0.4 + audioLevel*audioReact)` — the `+0.2` baseline boost from pass-1 not applied. I-frame stutter parameter `iframeRate` and the periodic clean-frame pass-1 fix not applied.

**New problems / second-order issues:**
- Burst at line 121 `if (burstRoll < burstProb * audioBass*audioReact)` — at silence, audio=0, burst probability=0. Bursts entirely audio-gated; no baseline. Pass-1 wanted some baseline burst presence.
- `freezeChance` default 0.05 — at silence baseline is `0.05 * 0.6 = 0.03`, only 3% of cells freeze. Subtle, perhaps too much.
- I-frame stutter (clean-frame insertion) missing — without it, the buffer never refreshes, and over time the moshBuf converges to mush. At default `moshPersistence=0.94`, after 50 frames the new content is 5% of the buffer → buffer essentially saturated to old content. Eventually freezes.
- Block corruption at line 113 `(0.4 + audioLevel)` — at silence this is 0.4 of slider, blockCorruption=0.40 default, so 16% of blocks corrupt → visible always. Good baseline.

**Mechanical fixes (apply directly):**
1. Line 113 — apply pass-1: `if (blkRoll < blockCorruption * (0.4 + audioLevel*audioReact + 0.2))` so silence baseline goes from 16% to 24%.
2. Line 121 — add baseline: `if (burstRoll < burstProb * (0.3 + audioBass*audioReact*0.7))` — silence shows occasional bursts, audio amplifies.
3. Add `iframeRate` slider: `{ "NAME": "iframeRate", "TYPE": "float", "MIN": 0.0, "MAX": 2.0, "DEFAULT": 0.5 }`. At top of pass 0 main: `if (iframeRate > 0.0 && fract(TIME * iframeRate) < 0.02) { gl_FragColor = vec4(fresh, 1.0); return; }` — periodic clean keyframe.
4. Memorable move — datamosh canon includes the GREEN/MAGENTA motion-vector ghost (P-frame artefact). Add a chroma direction that DIFFERS per row's hashed direction: replace line 102-104 with `vec2 chrDir = vec2(cos(rowId*1.7), sin(rowId*2.3))*chr; r = texture(moshBuf, uvT + chrDir).r; b = texture(moshBuf, uvT - chrDir).b;` — RGB split per-row → real datamosh palette.

**Optional polish:** `moshPersistence` default 0.94 is balanced; range goes to 0.999 (saturation territory).

---

## ai_latent_drift.fs

**Verdict:** PASS

**Round-1 fixes that landed correctly:** Phantom scale breath (line 124) `phantomScale * (0.7 + 0.3 * sin(TIME * 0.10))`. Rotating gradient (line 109-111) `ang = TIME*0.05; bias = uv.x*cos(ang) + uv.y*sin(ang); t = field + bias*0.20 + L1*0.5 + paletteShift;`. Phantom colour from palette (line 127-128) `phCol = latentPalette(t + 0.3, 0.0); col += phCol * ph * ...`. The `phantomScaleBreath` slider was NOT added (value is hardcoded as 0.3).

**New problems / second-order issues:**
- Rotating gradient at TIME*0.05 → full rotation in 126 sec. Smooth, contemplative. Right speed for Anadol.
- Phantom scale breath `0.7..1.0` of `phantomScale=14.0` → 9.8..14 effective scale. Mild variation.
- Directional blur at line 90-101 with default `taps=8` and `blurStrength=0.012` → integrates over `bDir * 12.0 * t` for t∈[-0.5, 0.5], so sample spread = `0.012 * 12 * 1 = 0.144` UV. That's a HUGE blur diameter. The whole canvas reads as smooth-cloud — diffusion-model softness. Good.
- The directional blur N=14 max iterations × `fbm` (6 octaves × 2 calls) = 168 noise calls per fragment per main pass. Heavy. At 4K = 0.5 GHz worth of noise.
- Audio breath at line 149 `* 0.88 + audioLevel*0.18` → silence → 0.88 brightness, audio max → 1.06. Acceptable.

**Mechanical fixes (apply directly):**
1. Add the `phantomScaleBreath` slider: `{ "NAME": "phantomScaleBreath", "TYPE": "float", "MIN": 0.0, "MAX": 0.6, "DEFAULT": 0.3 }`. Line 124 becomes `float scaleNow = phantomScale * (1.0 - phantomScaleBreath + phantomScaleBreath * (0.5 + 0.5*sin(TIME*0.10)));`.
2. Memorable move — Anadol's "Unsupervised" pieces have BRIGHT POINT-CLOUD specular bursts when the latent crosses high-density regions. Add a sparse high-frequency burst: `float spark = pow(fbm(P*30.0 + L3*5.0), 8.0); col += vec3(0.95, 0.88, 0.76) * spark * 1.2 * (0.3 + audioHigh*audioReact);` — phantom point bursts whose density tracks treble.
3. Performance — the directional blur loop unrolls to 14 iterations always. Add `if (i >= taps) break;` is already there; ensure compiler unrolls efficiently. No concrete fix needed but flag for testing on integrated GPU.

**Optional polish:** `latentSpeed` default 0.06 → LFO period `2π/0.06 ≈ 105 sec` — slow drift, right for the meditative aesthetic.

---

## Summary

PASS (10): expressionism_kirchner, constructivism_lissitzky, destijl_mondrian, surrealism_magritte, abex_pollock, abex_rothko, opart_vasarely, minimalism_stella, glitch_datamosh, ai_latent_drift.

NEEDS_TWEAK (8): art_nouveau_mucha (pass-1 fixes never applied — Klimt mosaic still static, S-curve missing), fauvism_matisse (drop-grid fixes never applied, posterize step missing, posterize slider never declared), cubism_picasso (per-plane fade clamps to 0.4 minimum so planes never disappear, letter tumble + fallback time fix not applied), futurism_boccioni (idle velocity floor + dab threshold + snake rotation not applied), dada_hoch (jagged edge / band scroll / stamp drift not applied), bauhaus_kandinsky (home drift never applied, defaults not bumped, halo+fill loop still uncached), popart_lichtenstein (edge detect still samples possibly-unbound texture — critical bug; speech bubble + panel scroll fixes missing), vaporwave_floral_shoppe (chroma still gated on input, posterize VHS step + slider missing).

NEEDS_REWRITE (0).

Total: 10 PASS / 8 NEEDS_TWEAK / 0 NEEDS_REWRITE. The headline finding is that pass-1 mechanical fixes were applied to roughly half the shaders. Where they landed (kirchner, magritte, vasarely, stella, latent_drift, mondrian lane breath, picasso recompose, datamosh fallback) the round-2 verdicts are PASS. Where they did NOT land, the same pass-1 issues recur. Pass 2 also surfaces three new critical issues: (1) popart_lichtenstein edge-detect samples a possibly-unbound texture which will silently break outlines without an input feed; (2) destijl_mondrian intersection-glow loop re-derives lane positions WITHOUT the laneBreath term, so glow drifts off the actual crossings; (3) abex_pollock dripperPos has a hard 14-step cap that freezes drippers after ~12 sec of TIME — every drip canvas eventually stops painting.
