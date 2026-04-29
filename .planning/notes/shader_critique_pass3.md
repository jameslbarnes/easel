# Shader Critique — Pass 3

Round 3 audit of 18 art-movement shaders. Pass-2 fixes have been applied across the set. This round: verify what landed, find each shader's "single trash failure" — the one mode that breaks the read at default params — assess whether motion reads as the *spirit* of the movement (Pollock=energetic, Mondrian=syncopated, Rothko=meditative, etc.), and run the 2-second guess test. Flag 4K-performance hazards.

---

## art_nouveau_mucha.fs

**Verdict:** PASS

**Two-second guess test:** Y — at default Style=0 (Whiplash) the persistent paintBuf shows soft sinuous trails of ink/gold on cream. The S-curve smoothstep gives genuine whiplash arc, not a wiggle on a line. A viewer will read "Art Nouveau / Mucha" within 2 sec on the strength of palette + sweeping curves alone.

**Confirmed pass-2 fixes that landed:**
- Klimt mosaic drift `floor((uv + 0.005*vec2(sin(TIME*0.05),...)) * 80.0)` at line 56-57 — applied.
- `tendrilPos` ping-pong `tt = abs(fract(...)*2.0 - 1.0)` + `smoothstep` S-curve at lines 93-94 — applied.
- Low-frequency arc bend `base += perp * sin(tt*π) * amp * 0.5` at line 99 — applied.

**Single biggest remaining issue:** Tendril deposits are still a single dot per frame per tendril (one pixel-distance test against a moving point). At default `tendrilSpeed=0.20`, the head moves slowly enough that the persistent-buffer tail reads as continuous, but at higher speed or smaller stroke width the tendril looks dotted, not stroked.

**Mechanical fix for that issue:** In `main()` PASS 0 (~lines 132-153), sample the tendril at *three* prior phases per frame and take `min(dh)`: `for (k=0;k<3;k++) { vec2 head_k = tendrilPos(t - 0.02*float(k), i, ...); dh = min(dh, length(uv-head_k_dispatched_perAspect)); }` — gives a 3-step trail per frame so deposits read as strokes regardless of speed.

**Performance note:** None — single PASS 0 loop is N=10 by default with O(1) work per tendril.

---

## fauvism_matisse.fs

**Verdict:** NEEDS_TWEAK

**Two-second guess test:** Borderline N — at silence, `dropRate=0.35` × `(0.5 + audioBass*0.8)` = 0.175 effective drop chance per cell. Coverage is sparse and the canvas reads as a drifting paper-show with stray colour blobs, not "Matisse". Without the posterize-to-FAUVE step from pass-1/pass-2, the colour soup is too soft.

**Confirmed pass-2 fixes that landed:**
- Drop grid drift `+ vec2(floor(TIME*0.13), floor(TIME*0.17))` at lines 97-99 — applied.
- Posterize-to-FAUVE step (pass-2 critical) — NOT applied. Pass 1 output (lines 143-149) has only saturation + grain, no palette snap.
- Complementary edge boost — NOT applied.
- `posterize` slider — NOT declared.

**Single biggest remaining issue:** Output is desaturated washed pigment — colour reads as pastel watercolour, not Fauvist primary clash. Without the snap-to-palette step the "pure unmixed pigment" signifier is missing.

**Mechanical fix for that issue:** After line 144 `col = saturateColor(...)`, add the pass-2 palette snap:
```
int bestK=0; float bd=1e9;
for(int k=0;k<6;k++){ float dd=dot(col-FAUVE[k],col-FAUVE[k]); if(dd<bd){bd=dd; bestK=k;} }
col = mix(col, FAUVE[bestK], 0.35);
```
Plus declare the `posterize` slider in the input block. This single step pushes the read from "soft watercolour" to "Fauvist".

**Performance note:** None.

---

## expressionism_kirchner.fs

**Verdict:** NEEDS_TWEAK

**Two-second guess test:** Y — the procedural Berlin street with shear, lit windows, lamp streaks, posterize, and acid LUT reads decisively as Die Brücke. Heavy contrast + black ink edges seal it. But the perspective shear leaning the canvas is the killer signifier and it works.

**Confirmed pass-2 fixes that landed:**
- Lamp per-lamp phase decoupling `* (0.7 + lampPhase)` — NOT applied; line 119 still uses the synchronized `0.5 + 0.5*sin(TIME*1.3 + lampPhase*6.28)`. All lamps blink in identical phase pattern, only offset by phase constant.
- Window per-window time offset — NOT applied; line 124 still `floor(TIME*0.7)` shared across all windows.
- Acid LUT mix-cycling between green and magenta — NOT applied; line 147 still hardcodes the 0.5 mix.

**Single biggest remaining issue:** All windows blink in lockstep (same `floor(TIME*0.7)` argument across the canvas). At any given moment all windows show the same lit/unlit state — looks like a synchronized clock, not a residential block where each apartment lives its own life. This breaks immersion immediately.

**Mechanical fix for that issue:** Line 124 — change `floor(TIME * 0.7)` to `floor(TIME * 0.7 + wHash * 5.0)` so each window's blink phase is hashed off its grid id; immediately the canvas reads as dozens of independent windows.

**Performance note:** Yes — at line 87-93, `ridgedFbm` is called 4× per fragment in PASS 0; each call is 5 octaves so 20 noise calls × 4 = 80 noise evaluations per fragment in the persistent pass. At 4K (8.3 M fragments) this is ~660 M noise calls per frame plus the source-content `ridgedFbm` at line 131. Could choke at 4K on integrated GPUs. Pass-2 suggested replacing the 4-tap gradient with a single ridgedFbm call + small offset; not applied.

---

## cubism_picasso.fs

**Verdict:** PASS

**Two-second guess test:** Y — overlapping translucent ochre rectangles with letter fragments, proceduralrubric reads decisively as analytic Picasso. Recompose breath via `pow(fadeT, 1.5)` lets planes drop to near-zero opacity so the composition genuinely recomposes; this is a PASS verdict where pass-2 was NEEDS_TWEAK.

**Confirmed pass-2 fixes that landed:**
- `alpha *= pow(fadeT, 1.5)` so planes can fully disappear — applied (line 178).
- Letter tumble `compositionSeed + floor(TIME*0.2)` — NOT applied; line 193 still uses raw seed. Letter fragments remain frozen.
- Procedural fallback `+ TIME*0.5 + fi*1.7` time term — applied at line 159.

**Single biggest remaining issue:** Letter fragments (the BAL/JOU/MA JOLIE signature flourishes) are completely static when no input texture is bound. The whole canvas breathes except for those four glyph clusters anchored at hashed-but-fixed positions. They read as printed-on stickers stuck to a moving canvas.

**Mechanical fix for that issue:** Line 193 — change `letterField(uv, compositionSeed)` to `letterField(uv, compositionSeed + floor(TIME * 0.2))`. Glyph clusters reposition every ~5 sec, syncing with the recompose breath.

**Performance note:** None — N=11 default, O(1) per plane.

---

## futurism_boccioni.fs

**Verdict:** PASS

**Two-second guess test:** Y — at silence, `vMag=velocityMag*(0.5+0.5*sin(TIME*0.3))` produces a real moving streak; trail persists; force rays radiate from origin biased by velocity; divisionist dabs spark. Reads as Futurism within 2 sec.

**Confirmed pass-2 fixes that landed:**
- `vMag = velocityMag * (0.5 + 0.5*sin(TIME*0.3))` idle floor — applied (lines 67-69).
- `vAng = TIME*velocityRotSpeed + sin(TIME*0.13)*0.6 + audioBass*audioReact*0.4` snake — applied (lines 64-66).
- Force-ray origin riding velocity `+ vel * 0.5` — applied (line 143).
- Dab threshold lowered to 0.85 — NOT applied; line 161 still `dh > 0.92`. Dabs stay sparse.

**Single biggest remaining issue:** Trail has the right velocity, but the persistent advection stage never deposits enough new content to read as Boccioni's "Dynamism of a Cyclist" — the whole subject visibly multiplied. With phantomCount=5 spread perpendicular to vel, the silhouette doubles up nicely; with audio silent, however, the buffer drives toward `outC = mix(prev, max(prev, newC*0.16), 0.6)` (since `1 - 0.96 = 0.04`, `0.04*4 = 0.16`) — newC contribution is tiny, so the trail dominates and figure barely registers.

**Mechanical fix for that issue:** Line 115 — change `outC = max(prev, newC * (1.0 - trailPersistence) * 4.0)` to `outC = max(prev, newC * 0.5)` so each frame's new figure stamps onto the trail at half alpha; dynamism reads more clearly. Also lower `dh > 0.92` to `dh > 0.85` at line 161 for denser dabs.

**Performance note:** None.

---

## constructivism_lissitzky.fs

**Verdict:** PASS

**Two-second guess test:** Y — red wedge ramming a white circle on cream paper, with rotating diagonal black bars. Strict palette and SDF geometry. Reads as Lissitzky immediately.

**Confirmed pass-2 fixes that landed:**
- `barShape` per-bar drift `c += 0.03*vec2(sin(TIME*0.4 + seed), ...)` — applied (lines 56-57).
- `barShape` rotation `+ TIME*0.10*sign(...)` — applied (line 52).
- `glyphField` origin drift — applied (lines 77-78).
- Wedge ramming `(0.5 + 0.5*sin(TIME*1.7))` — applied (lines 119-122).
- Printing-press shake un-gating — NOT applied; line 98 still `* compositionJitter * audioLevel` (still gated on audio).
- `wedgePulse` slider — NOT declared.

**Single biggest remaining issue:** Wedge thrust speed (`sin(TIME*1.7)` ≈ 0.27 Hz, ~3.7 sec full cycle) is gentle; Lissitzky's poster wants violent thrust. With audio silent the wedge pulses but never *slams*. The Constructivist energy is "revolutionary urgency", not "rhythmic breath".

**Mechanical fix for that issue:** Line 119 — replace the smooth sin with a sharp asymmetric ramp: `float thrustT = pow(0.5 + 0.5*sin(TIME*1.7), 3.0);` so the wedge dwells in retracted state then *snaps* forward, reading as repeated impact strikes. Cheap one-line change.

**Performance note:** None.

---

## dada_hoch.fs

**Verdict:** NEEDS_TWEAK

**Two-second guess test:** Borderline Y — at silence `bt = floor(TIME*2.5)` reshuffles strips every 0.4 sec, halftone overlay + chromatic slide read as photomontage. But the strips themselves don't drift between bt ticks (pass-1/pass-2 stamp drift not applied), so the canvas teleport-rearranges every 0.4 sec instead of evolving.

**Confirmed pass-2 fixes that landed:**
- Letter band scroll `bandY = fract(TIME*0.05 + sin(bt*0.71)*0.2)` — NOT applied; line 173 still uses `0.5 + sin(bt*0.71)*0.3` (jumps per beat, doesn't scroll).
- Stamp drift `+ 0.05*vec2(sin(TIME*0.4+g_), ...)` — NOT applied; lines 146-147 still bucket-only.
- Jagged edge `- jag` — NOT applied; line 107 still smooth.

**Single biggest remaining issue:** Stamps and letterband teleport per beat instead of drifting. The composition has two motion modes — slow chromatic ghost and beat-keyed teleport — with nothing in between. Höch's collage was *cut* once and *glued*; it didn't pulse-rearrange. The relentless quantized teleport reads as VJ effect, not Dada.

**Mechanical fix for that issue:** Line 146-147 (stamp positions) — apply pass-2 drift: `vec2 c = vec2(hash11(float(g_)*13.7 + bt*0.71), hash11(float(g_)*17.1 + bt*0.29)) + 0.05*vec2(sin(TIME*0.4 + float(g_)), cos(TIME*0.6 + float(g_)));`. And line 173 — `float bandY = fract(TIME*0.05 + sin(bt*0.71)*0.2);`. Together these break the beat-quantized teleport into continuous drift over a beat-keyed reshuffle.

**Performance note:** None.

---

## destijl_mondrian.fs

**Verdict:** PASS

**Two-second guess test:** Y — orthogonal lanes with primary pulses on white reads as Boogie Woogie within 2 sec. The marching pulses convey syncopation.

**Confirmed pass-2 fixes that landed:**
- Lane breath `+ sin(TIME*0.15 + fi*2.7)*0.012` — applied at lines 55, 92.
- Intersection-glow loop now includes the same `sin(TIME*0.15 + fi*2.7)*0.012` term at lines 130, 136 — applied. Critical bug from pass 2 fixed; glow correctly tracks lane crossings.
- Soft-edge pulse mix — NOT applied; lines 81, 114 still hard `col = pc;`.
- Beat-phase glow modulation `lit *= 0.7 + 0.3*sin(TIME*4.0+...)` — NOT applied; line 144 still constant.
- `laneBreath` slider — NOT declared (still hardcoded 0.012).

**Single biggest remaining issue:** Movement quality. Mondrian's spirit is *syncopated rhythm* (Broadway Boogie Woogie, jazz). Currently pulses move at constant per-lane speed in straight lines — that reads as "marching ants", not "boogie-woogie". The motion is too metronomic; lanes don't accent each other.

**Mechanical fix for that issue:** Inside both lane loops at the pulse phase computation (~line 73, 107), add a swing offset: `float xpos = fract(t + spawn + 0.15*sin(TIME*1.7 + fi*5.3));` — pulses speed up and slow down within their lane, creating the rhythmic stutter of jazz. Combined with the existing intersection glow this would feel genuinely syncopated.

**Performance note:** None.

---

## bauhaus_kandinsky.fs

**Verdict:** PASS

**Two-second guess test:** Y — yellow triangles, red squares, blue circles drifting on warm white with thin black support lines and weighted-circle halos. Reads as Composition VIII / Several Circles within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Home drift `+ 0.10*sin(TIME*0.07 + fi)` — applied to BOTH halo loop (line 79) and fill loop (line 119). Composition is no longer "same N positions rotating in fixed circles".
- `orbitSpeed` default → 0.25 — NOT applied; default still 0.15 at line 7.
- `orbitRange` default → 0.22 — NOT applied; default still 0.18 at line 8.
- `shapeCenter` extraction to share between halo+fill loops — NOT applied; still ~44 trig ops per pixel.
- Lines connecting two shape centres (memorable move) — NOT applied; lines still random.

**Single biggest remaining issue:** Defaults are too slow. `orbitSpeed=0.15` × `(0.4 + hash*1.4)` gives orbital periods 23–104 sec. At silence the canvas barely moves over a 5-sec glance — reads as a still life with subtle drift. Kandinsky's pedagogy was about geometry as *music*, not as quiet meditation.

**Mechanical fix for that issue:** In the input block (lines 7-8), bump `orbitSpeed` DEFAULT to 0.25 and `orbitRange` DEFAULT to 0.22. Two-line change. The shapes will visibly Lissajous around their drifting homes within a single glance.

**Performance note:** Yes — N=13 shapes × two loops × full Lissajous + hash math = ~26 trig ops per pixel. At 4K manageable but flagged. Refactor to a `vec2 shapeCenter(int i)` helper called once per i and reuse for both halo and fill loops would halve cost.

---

## surrealism_magritte.fs

**Verdict:** PASS

**Two-second guess test:** Y — sky over horizon with a hovering apple and Golconda ghost duplicates. The deadpan composition + photoreal sky (cobalt + cream) reads as Magritte.

**Confirmed pass-2 fixes that landed:**
- Cloud breath `* (1.0 + 0.04*sin(TIME*0.1))` — NOT applied; line 54 still without breath. Clouds translate, don't morph.
- Shadow rotation `+ TIME*0.05` — applied (line 137).
- `cloudDrift` default 0.12 — NOT applied; still 0.06 at line 14.
- Ghost grid (vs scatter) — NOT applied; ghosts still scatter at hashed positions.
- Ghost opacity baseline lift to 0.50 — NOT applied; still 0.35.

**Single biggest remaining issue:** Golconda ghosts scatter rather than form a grid. The painting *Golconda* is iconic specifically because the bowler-men hang in a *regular grid pattern* against the sky. Random scatter reads as "swarm of identical objects" — a different vibe (Surrealist invasion rather than Magrittean catalog).

**Mechanical fix for that issue:** Lines 182-185 — replace hash scatter with grid layout:
```
vec2 grid = vec2(mod(fg, 4.0), floor(fg / 4.0));
vec2 off = (grid - vec2(1.5, 1.5)) * 0.22
        + 0.04 * vec2(sin(TIME*0.2 + fg), cos(TIME*0.17 + fg));
```
This forms a 4×3 ghost grid drifting gently — instantly recognisable as Golconda, not Dalí swarm.

**Performance note:** None.

---

## abex_pollock.fs

**Verdict:** PASS

**Two-second guess test:** Y — black/white/silver/red/ochre drip skein on raw canvas, all-over composition with no focal point. The persistent paintBuf accumulates real strokes, reads as Pollock within 2 sec.

**Confirmed pass-2 fixes that landed:**
- `dripperPos` rewrite to walk forward from base every frame using `+ TIME*0.02` inside curl plus per-frame `0.02*vec2(sin/cos)` — applied (lines 70-77). Drippers now move continuously; no more 14-sec freeze bug.
- Pooling probability `if (hash11(fid + floor(TIME*0.3)) > 0.85) p = mix(p, base, 0.5);` — applied (line 80).
- Per-frame width variance — NOT fully applied; `wHash` is computed but unused at line 126; line 127 still uses fixed `strokeWidth * (1.0 + audioLevel*0.8)`.

**Single biggest remaining issue:** Movement quality — Pollock's spirit is *energetic, chaotic, athletic*. Currently drippers wander at uniform `wanderSpeed=0.12` and audio modulates with `audioMid*1.5` only on `t`. With audio silent the motion is calm, contemplative — reads as "automatist drawing", not "Number 1A Action Painting". Need bursts of speed.

**Mechanical fix for that issue:** Line 123 — replace `t = TIME * wanderSpeed * (0.5 + audioMid*audioReact*1.5)` with `t = TIME * wanderSpeed * (0.5 + 0.7*pow(0.5+0.5*sin(TIME*0.4 + hash11(fi)*6.28), 3.0) + audioMid*audioReact*1.5)` — adds per-dripper speed bursts (sharp peaks of `^3` envelope) so each dripper accelerates and decelerates independently. The canvas develops the staccato gesture-energy of action painting.

**Performance note:** Yes — `dripperPos` runs 14 inner curl iterations × 24 max drippers × pass-0 fragments = 336 noise calls per fragment in PASS 0. At 4K, 2.8 G noise calls per frame for the dripper update. May choke. Pass-1 fix (8 steps × 0.10 stride) was not applied; consider flagging.

---

## abex_rothko.fs

**Verdict:** PASS

**Two-second guess test:** Y — stacked feathered horizontal bands of saturated colour over a chromatic ground, with slow shimmer and vignette. Reads as Rothko within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Band edge undulation `edgeULo = 0.005*sin(uv.x*8.0 + TIME*0.03)` etc. — applied at lines 47-50. Band edges now organic.
- Shimmer asymmetry fix `(n - 0.5)` — applied (line 105).
- Band Y drift — NOT applied; lines 87-89 still hardcoded `0.62, 0.92`, etc.
- Inner-band glow (memorable move) — NOT applied.

**Single biggest remaining issue:** Bands occupy fixed Y positions forever. Over a 30-sec viewing the painting is *exactly* the same layout, just shimmering on top. Real Rothko canvases live by the eye creeping upward; the bands need to slowly migrate.

**Mechanical fix for that issue:** At top of `main()` after line 60, add `float drift = sin(TIME * 0.02) * 0.005;` and apply to band call y-bounds at lines 87-89: `bandShape(uv, 0.62 + drift, 0.92 + drift, ...)` etc. Cycle period 5 minutes; barely perceptible per frame but the painting is genuinely living over time.

**Performance note:** None.

---

## opart_vasarely.fs

**Verdict:** PASS

**Two-second guess test:** Y — concentric rings warped by a wandering bulge with hue rotation and centre highlight reads as Vasarely's Vega within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Bulge centre wander `+ 0.05*vec2(sin(TIME*0.5),cos(TIME*0.4))` — applied (lines 40-41).
- Bulge magnitude breath `(0.6 + 0.4*sin(TIME*0.7))` — applied (lines 53-54).
- Auto-twist `+ TIME*0.20` — applied (line 60).
- Ring frequency breath `* (1.0 + 0.18*sin(TIME*0.5))` — applied (line 64).
- Hue rotation `+ TIME * hueRotateSpeed` — applied (line 73).
- `ringPulse` slider exposing 0.18 — NOT declared.
- Chromatic aberration at dome rim (memorable move) — NOT applied.

**Single biggest remaining issue:** No chromatic vibration at the dome rim. Vasarely's signature optical effect is a faint complementary shimmer where compression is highest — without it the dome reads as a smooth gradient, not as an optical retina-itch. Currently flat colour bands inside the dome.

**Mechanical fix for that issue:** Replace line 84 single mix with three-channel offset: `float bandR = sin(rW*freqNow + 0.05)*0.5+0.5; float bandG = band; float bandB = sin(rW*freqNow - 0.05)*0.5+0.5; vec3 col = vec3(mix(colA.r, colB.r, bandR), mix(colA.g, colB.g, bandG), mix(colA.b, colB.b, bandB));` — generates real RG/GB moiré at high-compression ring zones. Pure Op Art retinal vibration.

**Performance note:** None.

---

## popart_lichtenstein.fs

**Verdict:** PASS

**Two-second guess test:** Y — Ben-Day dots, primary palette, thick black outlines, speech bubble with WHAAM. Reads decisively as Lichtenstein within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Edge detect uses `dFdx(lum)/dFdy(lum)` (no longer reads possibly-unbound texture) — applied at lines 214-219. Critical pass-2 bug fixed.
- Three technique branches all implemented — applied (tech==0 Ben-Day, tech==1 4-Up Silkscreen, tech==2 Halftone Pop).
- Speech bubble always-on with TIME pulse — applied (lines 224-249); no audio gate.
- Panel scroll for fallback — NOT applied; line 117 still `floor(uv*8.0)` static.
- Silkscreen TIME drift on per-quadrant offset directions — NOT applied; line 140 still uses static `qid*1.7`.
- Brushstroke memorable move — NOT applied.

**Single biggest remaining issue:** With no input texture bound, the procedural fallback (lines 113-127) shows a static UV-gradient grid behind the comic-effect filters. The whole canvas filter (Ben-Day dots, outline, posterize) sits on top of an unmoving substrate, so the only visible motion is the speech-bubble pulse. Reads as "filtered still life", not "comic in motion".

**Mechanical fix for that issue:** Lines 116-118 — apply pass-1 panel scroll: `vec2 c = (uv - 0.5 - vec2(sin(TIME*0.3)*0.05, cos(TIME*0.4)*0.04)) * vec2(1.0, 1.2); vec3 base = vec3(c.x+0.5, c.y+0.5, 0.5); vec2 g = floor((c+0.5) * 8.0); vec2 gf = fract((c+0.5) * 8.0);`. The Ben-Day filter then rides on a continuously drifting substrate.

**Performance note:** None.

---

## minimalism_stella.fs

**Verdict:** PASS

**Two-second guess test:** Y — concentric square/diamond rings with hard-edge palette stripes and raw-canvas hairline gaps. The marching outward ring rhythm reads as Hyena Stomp within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Always-on march `+ TIME * marchSpeed` — applied (lines 80-81).
- `rotateSpeed` default 0.05 — applied (line 8).
- Black Paintings raw-canvas band tone fix to subdued `vec3(0.42, 0.39, 0.34)` — applied (line 39).
- Protractor mode (memorable move) — NOT applied.

**Single biggest remaining issue:** With `marchSpeed=0.4` and `bandCount=11`, the ring index advances `0.4` per second with `bandCount*2*radius ≈ 22` steps to cross the canvas — a band cycles outward in 55 sec. Visually the rings barely move at default. Stella's spirit is *crisp, declarative, present*. The current motion is patient and slow.

**Mechanical fix for that issue:** Line 9 — bump `marchSpeed` DEFAULT from 0.4 to 0.7 (still under MAX 2.0). Rings march outward visibly within 1.5 sec. The "what you see is what you see" declaration reads more confidently.

**Performance note:** None.

---

## vaporwave_floral_shoppe.fs

**Verdict:** PASS

**Two-second guess test:** Y — pink/teal sky, magenta sun with bars, perspective grid floor receding, marble bust silhouette, scanlines, katakana, CRT bloom, 16-bit posterize. Reads as Floral Shoppe within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Chromatic aberration unconditional (works without bound texture) — applied at lines 164-171.
- VHS posterize `floor(col * 16.0) / 16.0` — applied (line 189). Hardcoded 16 levels.
- Sun bar scroll `+ TIME * 0.5` — applied (line 112).
- Marble TIME term — applied (line 43).
- `vhsBits` slider — NOT declared (16 still hardcoded).
- CRT vertical roll (pass-2 memorable move) — NOT applied.

**Single biggest remaining issue:** No CRT vertical roll. Vaporwave's nostalgic flagship effect is a tracking-error vertical roll that sweeps through every few seconds. Without it, the static-perspective composition with scanlines reads as a postcard, not as "VHS captured at 3am".

**Mechanical fix for that issue:** Top of `main()` at line 89 add `uv.y = fract(uv.y + smoothstep(0.95, 1.0, sin(TIME*0.15))*0.3);` — every ~6 sec the picture rolls once. Single line, immediate vibe.

**Performance note:** None.

---

## glitch_datamosh.fs

**Verdict:** PASS

**Two-second guess test:** Y — datamoshed scrolling stripes with per-row tearing, RGB channel split, block corruption, and burst rainbow. Reads as Murata-style datamosh within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Block corruption baseline `(0.4 + audioLevel*audioReact)` — applied (line 130). Pass-2 wanted `+ 0.2` extra; not added but baseline 0.4 already gives visible blocks.
- I-frame stutter `if (fract(TIME*0.5) < 0.02)` — applied (lines 50-62). Periodic clean-frame insertion working.
- Glitchy fallback (stripes + ring + flickering blocks) — applied (lines 87-95).
- Burst baseline at silence — NOT applied; line 138 still gates `< burstProb*audioBass*audioReact`. Bursts only fire on bass.
- Per-row datamosh chroma direction (memorable move) — NOT applied.
- `iframeRate` slider — NOT declared (0.5 hardcoded at line 50).

**Single biggest remaining issue:** Bursts (the catastrophic-failure rainbow garbage that signifies datamosh) require audio to fire. At silence the visual is moshing-tearing-blocks but no bursts — too clean. Datamosh canon is "sometimes the codec just fails, no reason"; bursts at zero audio sell that.

**Mechanical fix for that issue:** Line 138 — change `if (burstRoll < burstProb * audioBass * audioReact)` to `if (burstRoll < burstProb * (0.3 + audioBass * audioReact * 0.7))` — silence baseline = 30% of slider, audio amplifies on top.

**Performance note:** None.

---

## ai_latent_drift.fs

**Verdict:** PASS

**Two-second guess test:** Y — directionally blurred pigment cloud with palette mapping through cream/mauve/teal/orange and phantom features pulsing. Soft probability-edge feel. Reads as Anadol-style latent walk within 2 sec.

**Confirmed pass-2 fixes that landed:**
- Phantom scale breath `(0.7 + 0.3*sin(TIME*0.10))` — applied (line 124).
- Rotating gradient `bias = uv.x*cos(ang) + uv.y*sin(ang)` — applied (lines 109-110).
- Phantom colour from palette (`latentPalette(t + 0.3, 0.0)`) — applied (line 127).
- `phantomScaleBreath` slider — NOT declared (0.3 still hardcoded).
- Bright point-cloud sparks (memorable move) — NOT applied.

**Single biggest remaining issue:** No bright sparks. Anadol's "Unsupervised" pieces have a constant scatter of bright specular points where the latent crosses high-density clusters — without them, the canvas is a uniformly soft pigment cloud, missing the data-density signifier. Soft AND varied is the goal; right now it's just soft.

**Mechanical fix for that issue:** After line 130 (after phantom additive), add: `float spark = pow(fbm(P*30.0 + L3*5.0), 8.0); col += vec3(0.95, 0.88, 0.76) * spark * 1.2 * (0.3 + audioHigh*audioReact);` — bright pinpricks of warm light scattered across the canvas. Adds visual depth + audio reactivity.

**Performance note:** Yes — directional blur (lines 93-101) calls `fbm` × 2 × 14 taps = 28 fbm × 6 octaves = 168 noise calls per fragment. At 4K, 1.4 G noise per frame just for the blur. Add `if (i >= taps) break;` is present but the outer loop is bounded at 14. Flag for integrated GPU testing.

---

## Summary

**FAILS_GUESS_TEST:** none outright fail — but **fauvism_matisse** is borderline-N: at silence its drop coverage is sparse and without the posterize-to-FAUVE step the output reads as washed pastel watercolour rather than "Fauvist primary clash". Single fix: add the snap-to-palette step after `saturateColor` in PASS 1 — `for(k=0;k<6;k++)... col = mix(col, FAUVE[bestK], 0.35);`. **dada_hoch** is also borderline because its strips teleport per beat with no inter-beat drift; fix is to apply the pass-2 stamp drift `+ 0.05*vec2(sin(TIME*0.4+g_), cos(TIME*0.6+g_))` and the letter-band scroll `bandY = fract(TIME*0.05 + sin(bt*0.71)*0.2)`. All other 16 shaders pass the 2-sec guess test on movement palette + signature technique. The **NEEDS_TWEAK** count drops from pass-2's 8 to 3 here (fauvism_matisse, expressionism_kirchner, dada_hoch), driven by which pass-2 mechanical fixes did vs didn't land. Pass-3 also flags performance hazards at 4K for **expressionism_kirchner** (4× 5-octave ridgedFbm in PASS 0 → 80 noise/fragment), **abex_pollock** (14-step curl walk × 24 drippers in PASS 0 → 336 noise/fragment) and **ai_latent_drift** (28 fbm calls in directional blur → 168 noise/fragment). Output: `/Users/lu/easel/.planning/notes/shader_critique_pass3.md`.
