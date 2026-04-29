# Shader Critique — Pass 4

Round 4 of 18 art-movement shaders. Focus: (1) concrete performance optimizations with line ranges + quantified noise-call deltas for the three flagged shaders; (2) audio-silence test — does the shader's character live in the audio multiplier; (3) parameter sanity at default values; (4) cross-shader memorability.

---

## art_nouveau_mucha.fs

**Verdict:** PASS

**Audio-silent test:** Y — `tendrilSpeed=0.20` × `(1.0 + audioMid*audioReact*0.6)` → at silence the multiplier is 1.0, full default speed. Strokes deposit continuously. `petalBloom` and `paintFade` independent of audio.

**Parameter audit:** `tendrilCount` MAX 24 is overkill — at 10 the canvas is already dense; 24 looks identical to 16. `harmonicMix` at default 0.5 produces visible sub-harmonic; OK. `petalBloom` only matters in style 0/1; should be hidden in Beardsley.

**Performance:** None.

**Single best round-4 fix:** Apply the still-pending pass-3 multi-tap deposit (`for k=0..2` sample tendrilPos at `t - 0.02*k` and take min) at lines 132-153 — eliminates dotted look at high tendrilSpeed.

---

## fauvism_matisse.fs

**Verdict:** NEEDS_TWEAK

**Audio-silent test:** Borderline N — `dropRate=0.35 * (0.5 + audioBass*0.8)` collapses to `0.35 * 0.5 = 0.175` at silence. Half the drop coverage. Bucket cadence `(1.0 + audioMid*2.0) * 1.5` collapses to 1.5 Hz. Drops appear but sparsely. Pass-1 palette snap at line 156 IS now applied (good — disagrees with pass-3 finding).

**Parameter audit:** `dropRate` default 0.35 is too low at silence — recommend default 0.55 so silence floor is 0.275. `inputBleed` default 0 means a feature is invisible until the user discovers it. `swirlStrength` MAX 0.05 is fine; default 0.012 is gentle but the curl is visible.

**Performance:** None.

**Single best round-4 fix:** Line 104 — change `dropRate * (0.5 + audioBass * 0.8)` to `dropRate * (0.7 + audioBass * 0.6)` so silence floor is 70% of slider, not 50%. Pairs with bumping `dropRate` default to 0.55.

---

## expressionism_kirchner.fs

**Verdict:** NEEDS_TWEAK

**Audio-silent test:** Y — shear `sin(TIME*shearSpeed)*shearAmount` and `ridgedFbm(... + TIME*flow)` are unconditional. The carved-wood texture moves visibly at silence. Lamp blink `0.5 + 0.5*sin(TIME*1.3 + lampPhase*6.28)` (line 119) is unconditional. Window blink `floor(TIME*0.7)` (line 124) is unconditional but synchronized.

**Parameter audit:** `acidTint` default 0.42 is good; clamped to 0.7 inside (line 149) so MAX 0.80 in slider is misleading — slider goes to 0.80 but is silently capped. Cap the slider at 0.70. `posterize` default 4.0 is correct; `inkLines` default 0.55 visible.

**Performance:** YES — flagged in pass-3. **Concrete fix at lines 87-92:** the 4-tap gradient calls `ridgedFbm` × 4 (4 × 5 octaves = 20 noise calls per frag for the gradient alone), plus another `ridgedFbm` at line 131 (5 calls), plus the source-content `ridgedFbm` is already at line 131 — total `ridgedFbm` calls per fragment in PASS 0 = 5 (4 gradient + 1 carve) → **25 vnoise calls × 4 hash21 calls each = 100 hash calls** per fragment.
  - **Optimization:** Replace lines 87-92 with a single `ridgedFbm` call + cheap analytic gradient via finite difference of stored value, OR drop the gradient entirely and keep `prev *= paintFade` advection-free. **Mechanical change:**
    ```
    vec2 advUV = uv;  // skip gradient warp at lines 87-93
    vec3 prev = texture(carveBuf, advUV).rgb;
    ```
  - **Result:** 4× `ridgedFbm` removed → from 5 calls/frag to 1 call/frag = **80% reduction in PASS 0 noise calls** (from ~25 vnoise to ~5 vnoise). At 4K (8.3M frags), reduces 207M vnoise/frame to 41M vnoise/frame. The carve overlay at line 131 still gives the woodcut texture; the gradient warp was a barely-visible refinement.

**Single best round-4 fix:** Drop the 4-tap gradient at lines 87-93; keep `advUV = uv`. 80% noise reduction; visual impact minimal because `paintFade` already gives the stacked-passes feel.

---

## cubism_picasso.fs

**Verdict:** PASS

**Audio-silent test:** Y — `sin(TIME*0.4 + fi)`, `sin(TIME*0.27)`, `sin(TIME*recomposeRate + fi*2.7)` all run free. Plane fade (`pow(fadeT, 1.5)`, default `recomposeRate=0.3` → period ~21 sec) breathes without audio. Drift unconditional at line 109-110.

**Parameter audit:** `compositionSeed` MAX 80.0 is huge; 0–10 is enough. `letterFragments` default 0.35 visible. `viewpointSpread` only matters when texture bound — 0.22 default is fine but invisible in fallback (since fallback doesn't use sUV.vp meaningfully). `centerBias` default 0.65 is correct. `recomposeRate` default 0.3 → 21 sec cycle is good for breath.

**Performance:** None — N=11 default, planes are O(1) work each.

**Single best round-4 fix:** Apply the still-pending pass-3 letter-tumble at line 193 — `letterField(uv, compositionSeed + floor(TIME * 0.2))` — currently the letter clusters are still frozen.

---

## futurism_boccioni.fs

**Verdict:** PASS

**Audio-silent test:** Y — `vMag = velocityMag * (0.5 + 0.5*sin(TIME*0.3)) * (1.0 + audioLevel*1.5)` floors at `velocityMag * 0.5 * 1.0 = 0.0125` at silence. Trail moves. Force-rays pulse with `sin(TIME*0.83)`. Divisionist dabs: line 161 `if (dh > 0.92)` always allows ~8% of grid cells; not gated on audio.

**Parameter audit:** `velocityMag` default 0.025 — at silence `vMag` ranges 0–0.0125; trail visible but slow. Bump default to 0.04. `phantomCount` default 5 visible. `forceRays` default 16 fine. `divisionistDots` default 0.45 visible only at audio peaks because `(0.5 + audioHigh*audioReact*0.8)` floors at 0.5 — OK.

**Performance:** None.

**Single best round-4 fix:** Line 6 — bump `velocityMag` DEFAULT from 0.025 to 0.04 so silent-state trail energy is more visible.

---

## constructivism_lissitzky.fs

**Verdict:** PASS

**Audio-silent test:** Y — wedge thrust `(0.5 + 0.5*sin(TIME*1.7))` and bar drift `0.03*vec2(sin(TIME*0.4))`, glyph drift `sin(TIME*0.20)` all unconditional. Circle pulse `(0.85 + 0.15*sin(TIME*1.7 + π))` independent of audio. Bar rotation `+ TIME*0.10` unconditional.

**Parameter audit:** `compositionJitter` default 0.005 × audio gate — line 98 still multiplies by `(0.4 + audioLevel*0.6)`, so silence floor is 40% of slider. Acceptable. `triangleThrust` default 0.1, MAX 0.3 — fine. `circleSize` default 0.25 visible. `glyphIntensity` 0.6 visible. `barCount` default 2 — visible.

**Performance:** None.

**Single best round-4 fix:** Apply pass-3 `pow(0.5+0.5*sin(TIME*1.7), 3.0)` at line 119 to make the wedge slam instead of breathe — gives the Constructivist urgency the canvas currently lacks.

---

## dada_hoch.fs

**Verdict:** PASS

**Audio-silent test:** Y — `bt = floor(TIME*beatRate) + floor(audioBass*audioReact*6.0)` — the `floor(TIME*2.5)` term ticks every 0.4 sec without audio. Stamp drift at line 153-154 (`+ 0.05*vec2(sin(TIME*0.4)`) is unconditional. Letter band `fract(TIME*0.05 + sin(bt*0.71)*0.2)` (line 181) scrolls continuously. **Pass-3 fixes for stamp drift and letter scroll DID land** (revising pass-3 finding).

**Parameter audit:** `beatRate` default 2.5 → reshuffle every 0.4 sec is correct cadence. `stripsPerBeat` default 8 visible. `chromaSlide` default 0.012 subtle but visible. `paperFade` default 0.992 — slow accumulation (good). `letterStutter` default 0.4 visible. `halftoneScale` default 180 produces visible dots.

**Performance:** None — but PASS 0 strip loop is `int N = int(clamp(stripsPerBeat, 1.0, 20.0))` with rect-test-then-skip — at default 8 strips × 8.3M frags this is fine.

**Single best round-4 fix:** Apply the still-pending pass-3 jagged edge at line 110-111 — `smoothstep(0.0, 0.06, min(edgeX, edgeY) - jag)` IS already there. Looking at line 107-111, jag IS applied. **Confirmed all three pass-2 fixes landed**; this is now PASS-quality. Net new round-4 issue: `stripScale` MAX 0.4 with default 0.18 — at high values strips dominate; tighten MAX to 0.3.

---

## destijl_mondrian.fs

**Verdict:** PASS

**Audio-silent test:** Y — lane breath `sin(TIME*0.15 + fi*2.7)*0.012` unconditional (lines 55, 92, 130, 136). Pulse march `t = TIME * speed * dir` always running, audio is `(1.0 + audioMid*0.6)` multiplier. Intersection box size `(1.0 + audioBass*audioReact*0.6)` floors at 1.0. Without audio: pulses march, intersections light at full size, lanes breathe.

**Parameter audit:** `pulseDensity` default 4 produces 4 pulses per lane — visible. `marchSpeed` default 0.35; pulses cross the canvas in 1/0.35 ≈ 2.9 sec — acceptable. Probabilities (`redArea=0.22`, `blueArea=0.18`, `yellowArea=0.30`, `greyMix=0.18`) sum to 0.88, leaves 0.12 for black — OK distribution.

**Performance:** None — but the intersection-glow loop is NH×NV = 36 iterations × 8.3M frags at default; manageable but borderline. At MAX (14×14 = 196) starts to bite.

**Single best round-4 fix:** Apply still-pending pass-3 swing offset `+ 0.15*sin(TIME*1.7 + fi*5.3)` at line 73, 107 to break the metronomic pulse march into syncopation — this is the Boogie-Woogie signifier missing from the current build.

---

## bauhaus_kandinsky.fs

**Verdict:** PASS

**Audio-silent test:** Borderline Y — Lissajous orbit `a = TIME * orbitSpeed * (0.4 + hash11(fi*3.1)*1.4)`. With `orbitSpeed=0.25` (current default at line 7) × hash range (0.4–1.8) → orbital periods 14–63 sec. Visible movement over a 5-sec glance. Home drift `0.10*sin(TIME*0.07 + fi)` adds very slow translation. **Pass-3 said default still 0.15; it IS now 0.25** (revised).

**Parameter audit:** `orbitRange` default 0.22 (also bumped per pass-3). `springReact` default 0.12 — useless at silence (multiplied by `audioBass*audioReact`); shows nothing without audio. `useTexPalette` is bool with default false — invisible feature.

**Performance:** Yes — N=13 shapes × TWO loops (halo + fill) at lines 72 and 113 with full Lissajous + hash math = ~26 trig ops per pixel. At 4K manageable. **Optimization:** extract `vec2 shapeCenter(int i)` helper and precompute centers once into a vec2[22] array at start of `main()`, reuse for both loops. Reduces ~13 redundant Lissajous evaluations per fragment to zero. Hard to express in GLSL because no array assignment from a function — easier to inline a single loop that handles both halo AND fill: merge lines 72-103 with 109-153 into one walk that accumulates haloField AND tracks bestSD/bestCol. **~50% reduction in trig ops** for the shape system.

**Single best round-4 fix:** Merge the halo loop (72-103) and fill loop (113-153) into one walk over `i in [0..N)`. Each iteration computes ctr once, contributes to haloField AND tests bestSD. Halves the trig and hash evaluations for shape geometry.

---

## surrealism_magritte.fs

**Verdict:** PASS

**Audio-silent test:** Y — clouds drift `t * 0.3 + cloudDrift*TIME` unconditional (line 54 + 114). Hover `sin(TIME*0.5)*hoverAmp*(1.0 + audioLevel)` floors at full hover at silence. Shadow rotation `TIME*0.05` unconditional. Ghost grid drift unconditional.

**Parameter audit:** `cloudDrift` default 0.06 — clouds creep slowly (acceptable for Magritte's still-life pacing). `hoverAmp` default 0.025 visible. `ghostMultiply` default 0.35 visible. `shadowAngle` default 5.5 rad — odd default; better at 0.0 or π/4. `objectChoice` default 0 (Apple) is the canonical Son of Man. `ghostCount` default 5 — but the grid logic uses 4×3 = 12 positions; 5 ghosts in a 4×3 grid leaves 7 empty slots — looks sparse. Bump default to 10 to fill most of the lattice.

**Performance:** None.

**Single best round-4 fix:** Line 19 — bump `ghostCount` DEFAULT from 5 to 10 so the Golconda lattice reads as a populated grid, not a half-empty grid.

---

## abex_pollock.fs

**Verdict:** NEEDS_TWEAK (performance)

**Audio-silent test:** Y — `t = TIME * wanderSpeed * (0.5 + audioMid*audioReact*1.5)` floors at `0.5*wanderSpeed = 0.06` at silence. Drippers walk. Per-frame vec at line 76-77 unconditional. Pass-3 said `wHash` is unused (still true at line 126); `splatterDensity` audio-gated.

**Parameter audit:** `drippers` default 12 — acceptable density. `wanderSpeed` default 0.12 → at silence effective 0.06; drippers progress about 0.06 units/sec — slow. Bump default to 0.18. `paintFade` default 0.992 → buffer half-life ~85 sec — too long; canvas saturates within a minute. Drop default to 0.985 (half-life ~46 sec).

**Performance:** YES — flagged in pass-3 as 336 noise calls/frag. **Concrete fix at lines 64-82 (`dripperPos`):** the inner loop is 14 iterations × 1 `curl()` call (= 4 vnoise) = **56 vnoise per dripper × 24 max drippers = 1344 vnoise/frag** in PASS 0. The integration is wasted because the per-frame vec at line 76-77 dominates the position anyway. **Mechanical change at lines 70-72:** reduce loop count from `i < 14` to `i < 6` and stride from `0.05` to `0.08` (covers same path length). Quantified delta:
  - Before: 14 × 4 vnoise = 56 vnoise/dripper
  - After: 6 × 4 vnoise = 24 vnoise/dripper
  - **57% reduction in dripper noise.** At 24 drippers × 4K fragments: drops from 11.2 G vnoise/frame to 4.8 G vnoise/frame.

**Single best round-4 fix:** Lines 70-72 — change `for (int i = 0; i < 14; i++)` to `for (int i = 0; i < 6; i++)` and `* 0.05` to `* 0.08`. 57% fewer noise calls in PASS 0, near-identical visual.

---

## abex_rothko.fs

**Verdict:** PASS

**Audio-silent test:** Y — band edge undulation `0.005*sin(uv.x*8.0 + TIME*0.03)` unconditional (lines 47-48). Shimmer `vnoise(uv*2.6 + TIME*shimmerSpeed)` unconditional. Audio influence is hard-capped at 0.04 per spec — the painting refuses to be hurried, by design.

**Parameter audit:** `shimmer` default 0.04 subtle but present. `shimmerSpeed` 0.04 → 25-sec cycle; correct for meditative pacing. `feather` default 0.12 → bands feathered. `audioInfluence` default 0.04 (capped to 0.10 MAX) — intentional. `bandCount` MIN 2 / MAX 4 — but the if-branch only handles `>= 3` and `< 3`; `bandCount = 4` falls into the `>= 3` branch and only renders 3 bands. Bug, but minor — extend the layout to handle 4.

**Performance:** None.

**Single best round-4 fix:** Either extend `bandCount` MAX to 3 (matching what's actually drawable) or add a fourth band at lines 87-92. Currently MAX=4 advertises a feature that doesn't exist.

---

## opart_vasarely.fs

**Verdict:** PASS

**Audio-silent test:** Y — bulge centre wander `0.05*vec2(sin(TIME*0.5))` unconditional. Bulge magnitude `(0.6 + 0.4*sin(TIME*0.7))` floors at 0.2 unconditional. Auto-twist `+ TIME*0.20` unconditional. Hue rotation `+ TIME*hueRotateSpeed` unconditional.

**Parameter audit:** `twist` default 0.0, slider 0..2π — but auto-twist `+ TIME*0.20` is added regardless, so user-set twist is additive on top. Slider behaviour confusing. `hueRotateSpeed` default 0.05 → 20-sec cycle; visible. `ringFrequency` default 28 visible. `ribSharpness` default 0.35 — produces medium-soft band edges.

**Performance:** None — single fragment, all closed-form math.

**Single best round-4 fix:** Apply the still-pending pass-3 chromatic-vibration at line 84 — `vec3 col = vec3(mix(colA.r, colB.r, bandR), mix(colA.g, colB.g, bandG), mix(colA.b, colB.b, bandB))` with offset bands per channel. Op Art's signature retinal vibration is missing.

---

## popart_lichtenstein.fs

**Verdict:** PASS

**Audio-silent test:** Y — speech bubble `(0.85 + 0.15*sin(TIME*1.5)) * (1.0 + audioBass*0.6)` floors at 1.0 at silence. Procedural fallback (lines 113-127) is **static** — no TIME term in `vec2 g = floor(uv*8.0)`. Without input texture and without audio, only the bubble pulses; canvas is otherwise frozen. Pass-3 flagged this; still not fixed.

**Parameter audit:** `outlineWidth` default 2.5 — `outline = smoothstep(threshold, threshold+0.04, edge*outlineWidth*40.0)` so at default 2.5 × 40 = 100, edge needs to be > threshold/100 = 0.001 — almost any fragment edges in. Result: outlines blanket the whole image at default. Drop `outlineWidth` default to 1.0. `silkscreenShift` default 0.012 visible only in tech=1. `posterizeLevels` default 4 correct. `dotDensity` default 140 — Ben-Day dots visible.

**Performance:** None.

**Single best round-4 fix:** Lines 116-118 — apply pass-3 panel scroll `c = (uv - 0.5 - vec2(sin(TIME*0.3)*0.05, cos(TIME*0.4)*0.04)) * vec2(1.0, 1.2)` so the substrate moves under the Ben-Day filter at silence.

---

## minimalism_stella.fs

**Verdict:** PASS

**Audio-silent test:** Y — `bandIdx = floor(bf + TIME*marchSpeed + paletteMarch*audioBass*6.0)`. At silence the audio term is 0, so `floor(bf + TIME*0.4)` ticks rings outward at 0.4 per second unconditionally. Rotation `TIME*rotateSpeed=0.05` unconditional.

**Parameter audit:** `marchSpeed` default 0.4 visible (a band rotates outward every 2.5 sec). `rotateSpeed` default 0.05 → 126-sec full rotation; subtle but unconditional. `bandCount` default 11 — band thickness `0.5/bandCount = 0.045` at the centre. `paletteMarch` default 0.4 — useless at silence (multiplied by `audioBass*audioReact`); shows nothing without audio.

**Performance:** None — single closed-form per-fragment.

**Single best round-4 fix:** Line 9 — bump `marchSpeed` DEFAULT from 0.4 to 0.7 (per pass-3) for crisper Stella declarative motion.

---

## vaporwave_floral_shoppe.fs

**Verdict:** PASS

**Audio-silent test:** Y — sun bars `sin(barY*sunBars*π + 0.4 + TIME*0.5)` unconditional. Grid `1.0/dh - TIME*gridSpeed` unconditional. Bust hover `sin(TIME*0.4)*bustHover*(1.0 + audioLevel)` floors at 1.0. Marble noise `+ TIME*0.08`. Chromatic aberration unconditional. Posterize at line 189 unconditional.

**Parameter audit:** `vhsBits` parameter does not exist (still hardcoded to 16 at line 189). `chromaShift` default 0.004 — barely visible; bump to 0.008. `crtBloom` default 0.4 visible. `katakanaIntensity` default 0.6 visible.

**Performance:** None.

**Single best round-4 fix:** Top of `main()` — apply pass-3 vertical-roll `uv.y = fract(uv.y + smoothstep(0.95, 1.0, sin(TIME*0.15))*0.3)` for the canonical VHS tracking-error sweep.

---

## glitch_datamosh.fs

**Verdict:** PASS

**Audio-silent test:** Y — fallback fresh content (lines 87-95) animates with TIME (`fract(uv.x*5.0 + TIME*0.30)` etc). I-frame stutter `if (fract(TIME*0.5) < 0.02)` ticks every 2 sec. Burst at line 138 `< burstProb * audioBass * audioReact` — **gates entirely on audio** (still not fixed per pass-3).

**Parameter audit:** `freezeChance` default 0.05 × `(0.3 + audioMid + 0.3)` = silence floor 0.05 × 0.6 = 0.03; visible. `blockCorruption` default 0.40 × `(0.4 + audioLevel)` floor 0.16; visible. `tearAmp` default 0.06 visible. `burstProb` default 0.10 — but `* audioBass * audioReact` zeros out at silence. **Only audio-gated parameter that's actually invisible at silence.**

**Performance:** None.

**Single best round-4 fix:** Line 138 — apply pass-3 `(0.3 + audioBass * audioReact * 0.7)` so silence baseline gives 30% of slider, not 0%.

---

## ai_latent_drift.fs

**Verdict:** NEEDS_TWEAK (performance)

**Audio-silent test:** Y — three LFOs `L1, L2, L3 = TIME * ws * ...` unconditional. Curl warp unconditional. Phantom `fbm(P*scaleNow + L2*1.7)` unconditional with `(0.4 + audioHigh*1.3)` floor 0.4.

**Parameter audit:** `latentSpeed` default 0.06 — slow; LFOs cycle every ~17 sec. Acceptable for "latent walk" pacing. `blurTaps` default 8 — directly drives the perf hazard. `blurStrength` default 0.012 visible. `phantomScale` default 14 visible.

**Performance:** YES — flagged in pass-3 as 168 noise calls/frag in directional blur. **Concrete fix at lines 93-101 (the directional blur loop):** loop runs `min(blurTaps, 14)` iterations × **2 `fbm` calls × 6 octaves = 12 vnoise per tap**. At default `blurTaps=8`: 8 × 12 = 96 vnoise/frag. Plus phantom `fbm` (6 vnoise) and palette. **Mechanical change:**
  1. Drop `blurTaps` default from 8 to 5 (line 10): 5 × 12 = 60 vnoise → **38% reduction**
  2. Reduce `fbm` octaves from 6 to 4 (line 38): each fbm now costs 4 vnoise instead of 6 → 5 × 8 = 40 vnoise → **58% reduction overall** vs original
  - At 4K (8.3M frags): drops from 800M vnoise/frame in blur to 332M.

**Single best round-4 fix:** Two-line change: line 38 `for (int i = 0; i < 4; i++)` (was 6) and line 10 default `8.0` → `5.0`. Combined 58% reduction in directional-blur noise; visual softness preserved because the curl-domain warp does most of the smoothness.

---

## Cross-shader analysis

**Risk of looking too similar:**

- **fauvism_matisse + abex_pollock** — both are "persistent paint buffer + curl-noise advection + colour drops on a paper substrate". Differentiation: Pollock should ditch the curl-self-advection at line 116-118 (paint flows after deposit) and rely purely on dripper position deposits — Pollock's surface DOESN'T flow, it stays where the gesture put it. Matisse keeps the flow. Currently they read as siblings.

- **ai_latent_drift + abex_rothko** — both are "low-frequency soft colour fields with shimmer breath". Differentiation: Rothko's bands have **horizontal stratification** (3 stacked bands); ai_latent_drift has **rotating gradient bias** (line 110). The visual identity is preserved IF Rothko keeps its hard band layout, but at low contrast both can read as "slow chromatic ambient". Push ai_latent_drift toward more visible phantom forms (line 124-130 phantomPulse) — currently the phantom contribution is subtractive-additive at low amplitude.

- **art_nouveau_mucha + abex_pollock + futurism_boccioni** — all three deposit moving subjects into a persistent buffer with fade. The differentiator is the deposit shape: Mucha is a single-pixel head trailing a smoothstep S-curve; Pollock is N independent drippers; Boccioni is a phantom-fan along velocity. Currently the visual reads diverge because of palette + decay rate, but on a B&W comparison they look interchangeable. Mucha's S-curve harmonics (line 102-103) should be more aggressive — pump `harmonicMix` default to 0.75 so the strokes wave more strongly than Pollock's curl-walkers.

- **expressionism_kirchner + dada_hoch** both use halftone + chromatic-slide-style passes over a noisy substrate. Differentiation is OK: Kirchner has shear and ridged-noise carving; Dada has axis-aligned strip rectangles. But on a quick glance they share "high-contrast noisy print look". Kirchner's shear is the strongest differentiator — make sure `shearAmount=0.18` default reads decisively (it does).

**Coverage gaps:**

- **Conceptual / Land Art (Smithson, LeWitt, Andre)** — no shader represents grid/system/land work. A LeWitt wall-drawing-style shader (procedural pencil grid that grows according to algorithmic instructions) would be a clean addition.
- **Photo-realism / Hyperrealism (Chuck Close, Richter)** — gridded portrait reconstructions; nothing in the canon shader-set targets this.
- **Romanticism / Sublime (Turner, Friedrich)** — atmospheric storm paintings with luminous mist; very different from the AI-latent soft cloud since Turner's light has *direction*.
- **Japanese Ukiyo-e (Hokusai, Hiroshige)** — flat-area woodblock prints with gradients (bokashi); the technique (mokuhanga colour separation + signature wave forms) is nowhere in the set.
- **Bauhaus weaving / Anni Albers** — the canon includes Bauhaus painting (Kandinsky) but not the textile / pattern lineage (vertical thread-grid with rotating colour modules) which would differentiate visually from Mondrian's grid.

The current 18 lean heavily on painting (15 of 18); only vaporwave / glitch / AI represent post-2000 net-aesthetic. A **photographic / printmaking** axis is the biggest gap.

---

## Performance summary

Three quantified optimizations land in this round:

| Shader | Change | Lines | Noise reduction |
|---|---|---|---|
| expressionism_kirchner | Drop 4-tap gradient warp | 87-93 | ~80% (from ~25 vnoise/frag to ~5 in PASS 0) |
| abex_pollock | Reduce `dripperPos` loop 14→6, stride 0.05→0.08 | 70-72 | ~57% (from 56 to 24 vnoise/dripper) |
| ai_latent_drift | `blurTaps` default 8→5, `fbm` octaves 6→4 | 10, 38 | ~58% (from 96 to 40 vnoise/frag in blur) |

All three changes preserve the visual signature (gradient warp was barely visible; dripper integration is dominated by the per-frame vec; latent blur is dominated by curl warp not octave depth). At 4K these together drop the worst-case noise budget for the three flagged shaders by approximately 60-80%.

---

## Summary

Three NEEDS_TWEAK in round 4: **fauvism_matisse** (silence-floor dropRate too low — line 104 fix), **expressionism_kirchner** (perf — drop the 4-tap gradient at lines 87-93), **abex_pollock** (perf — reduce dripperPos loop 14→6). All other 15 PASS the round-4 audit. Audio-silence test catches three shaders where audio gating still hides features at silence: glitch_datamosh's bursts (line 138), bauhaus_kandinsky's springReact (only nudges on bass), popart_lichtenstein's static fallback substrate. Cross-shader memorability flags two adjacent pairs (Pollock/Matisse paint-buffer twins; ai_latent/Rothko slow-colour-field twins) that need stronger visual differentiation. Coverage gap: the canon under-represents printmaking/photographic movements (Ukiyo-e, Hyperrealism, Photo-realism). Output: `/Users/lu/easel/.planning/notes/shader_critique_pass4.md`.
