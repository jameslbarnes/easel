# State of Real-Time Graphics: 2024–2026

A field-survey for Easel, written for a senior C++/GLSL implementer who needs to know
what is actually new, why it matters for live audio-reactive projection, and what is
realistic to integrate into a small OpenGL host. Date: April 2026.

---

## 1. Niagara / Unreal particle innovations (UE 5.4 → 5.7)

What is genuinely new since UE 5.0 (when Niagara was already shipped) is not a single
feature but a *re-architecting* of the particle pipeline around three pillars:
GPU-side simulation at population scales nobody attempted in 5.0, hybrid
ray-traced lighting of particle media, and tight bidirectional coupling with
non-particle systems (fluids, PCG, Chaos, Lumen).

**Niagara Fluids** moved out of experimental in UE 5.5 and is now first-class in
5.7 documentation. It runs real-time grid-based fluids (FLIP for liquids, MAC-grid
gas) inside the Niagara graph, so the same emitter that spawns particles can
*be* the fluid solver. Simulation Stages — introduced in 5.0 but rebuilt in
5.4 — let you run multiple compute passes per tick with different DI bindings,
which is what makes per-frame pressure-projection or vorticity-confinement viable
inside a stock emitter ([UE 5.7 Niagara Fluids docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-fluids-in-unreal-engine), [Unreal Fest Gold Coast 2024 – Advanced Niagara VFX talk](https://dev.epicgames.com/community/learning/talks-and-demos/pLpM/unreal-engine-advanced-niagara-vfx-fluids-simulation-stages-and-more-unreal-fest-gold-coast-2024)).

**Lightweight Emitters** (5.4 experimental → 5.6 stable) are the other end of
the curve: stripped emitters with no per-particle scripts, designed for sparks,
dust, debris where you need 100k+ instances at near-zero memory cost. Epic's
internal benchmarks on the GDC '24 State of Unreal showed orders-of-magnitude
particle-count increases on the same GPU budget when migrating from full
emitters to lightweight ones ([CG Channel UE 5.6 preview](https://www.cgchannel.com/2025/01/see-the-new-features-due-in-unreal-engine-5-6-and-beyond/), [iRender Lightweight Emitters intro](https://irendering.net/introduction-to-lightweight-emitters-for-niagara-in-unreal-engine/)).

**Lumen-lit particles**: 5.4 added correct Lumen GI on translucent and lit
particles; 5.6 introduced ray-traced translucency with hit-lighting integrated
with depth-of-field. This finally makes haze, smoke, embers and god-rays receive
indirect bounce and reflect emissive transients without per-particle light
hacks ([UE 5.6 Lumen RT translucency demo, 80.lv](https://80.lv/articles/check-out-unreal-engine-5-6-s-lumen-with-ray-traced-refraction-demo)).

**Cross-system coupling**: 5.6/5.7 makes Niagara and PCG interoperable so
foliage-spawning systems become emitters and emitters can read PCG point clouds.
Combined with Chaos soft-body and cloth (Chaos Cloth 2 in 5.5), particles can be
spawned on a deforming cloth's surface or inherit velocity from a Chaos
soft-body collision — the single biggest "looks like CGI in real time" upgrade
since Nanite.

**Implication for Easel**: We will not re-implement Niagara, but the *patterns*
matter. (a) Multi-pass GPU simulation (ping-pong + simulation-stage analogue)
buys us large particle counts cheaply. (b) Treating shaders as stages of a
graph rather than monolithic frags lets us compose effects, which directly
informs how we should layer ISF passes. (c) Lit particles in Lumen are out of
reach, but a fake "GI tint" sampling a low-res scene-color buffer can mimic the
look at 1% the cost.

---

## 2. GLSL / shader trends (2024–2026)

The trend line is **hybrid pipelines**. Pure raster is rare; pure path-traced is
expensive; the interesting work fuses raster G-buffers with screen-space ray
marching, signed-distance fields and short-range ray tracing.

**Ray-marched SDFs going mainstream**: SDFs are no longer a Shadertoy curiosity.
Unreal exposes Distance Fields as a first-class scene resource; godot 4.x
ships SDF-based GI; libraries like *hg_sdf* and Inigo Quilez's distance-function
catalogue are now standard. The signed-distance combinator vocabulary
(`opSmoothUnion`, `opTwist`, `opRevolution`, `opOnion`) is what lets a single
fragment shader sculpt a scene that would have needed a mesh pipeline.

**Flow-noise / curl-noise for organic motion**: The default for any vector
field that needs to look fluid. Curl noise is divergence-free, so particles
following it stay area-preserving — they don't pile up or thin out
unphysically. atyuwen's *bitangent_noise* (2024) gives GLSL a fast 3D/4D
divergence-free primitive in a single shader without dependencies, which is the
go-to today ([Visualizing Curl Noise – Shadertoy](https://www.shadertoy.com/view/mlsSWH), [Bitangent Noise GitHub](https://github.com/atyuwen/bitangent_noise), [Dissecting Curl Noise – Emil Dziewanowski](https://emildziewanowski.com/curl-noise/)).

**Screen-space curvature** (Mertens 2008-style, but now ubiquitous as a stylistic
edge term): differentiate normals across neighbouring fragments to extract
convex/concave bands; multiply into AO or stylise as ink lines. It's the
technique behind much of what reads as "Pixar-quality edges" in indie
shaders ([Curvalicious shader doc](https://github.com/p-groarke/fea_osl/blob/main/doc/curvalicious.md), [Screen Space Reflection – Lettier](https://lettier.github.io/3d-game-shaders-for-beginners/screen-space-reflection.html)).

**Custom tone mapping**: 2024–2026 is the AGX/Tony McMapface era, replacing
ACES as the reference real-time DRT. AGX (Troy Sobotka) is Blender 4.0's
default; Tony McMapface (Tomasz Stachowiak) is Bevy's default and prizes
neutrality — it is intentionally boring, doesn't increase contrast or
saturation, and ships as a small LUT + shader. Both behave dramatically better
than Reinhard/ACES on saturated highlights, which is exactly the regime
audio-reactive shaders push hardest ([Reframing Tonemapping in Games – Alex Tardif](https://alextardif.com/Tonemapping.html), [AgX Unreal port (gist)](https://gist.github.com/nxrighthere/eb208dae8b66dbe452af223f276e46cc), [three.js AGX discussion](https://discourse.threejs.org/t/is-agx-tonemapping-implemented-correctly/60609)).

**Neural denoisers in real-time**: NVIDIA's Ray Reconstruction (DLSS 3.5+)
and Intel OIDN's lightweight networks now run inside frame budgets. Useful for
reducing samples needed in ray-traced effects; less directly applicable to
Easel, but the *aesthetic* of "denoised, smoothed, AI-assisted" image is
infiltrating non-RT shaders too via temporal filters.

**Implication for Easel**: replacing our current Reinhard-flavoured tonemap
with AGX or Tony McMapface is a 30-line change with outsized perceptual gain —
worth doing as a one-shot phase. SDF combinators belong in a shared `_lib.glsl`.
Curl noise is the single most leveraged primitive we don't yet have; ship
`bitangent_noise` as a helper.

---

## 3. TouchDesigner installation patterns

TouchDesigner is, for our purposes, the reference vocabulary for what a
"node-graph projection installation" looks like. Six recurring patterns dominate
2023–2026 work:

**Feedback TOPs (TOP→TOP wraparound)**: a `Feedback TOP` reads the previous
frame, a `Composite TOP` mixes new content over it, output goes back. Even with
a low feedback gain (0.95) plus a tiny zoom or rotation per frame, you build up
hypnotic trails that feel infinitely deep ([TouchDesigner GPU Particles + Feedback – AllTouchDesigner](https://alltd.org/touchdesignerparticles-gpu-feedback-effects/), [Feedback tag, AllTouchDesigner](https://alltd.org/tag/feedback/)).

**GPU compute particles via ping-pong textures**: positions and velocities
encoded into RG16F/RGBA32F textures, simulated by a fragment or compute shader
that reads texture-N and writes texture-N+1, then they swap. Particle counts
in the millions are routine; complex behaviour (curl-noise, depth-aware
collision against a scene-Z buffer) costs the same per-particle as simple
behaviour ([Introduction to TouchDesigner – GPU Particle Systems](https://nvoid.gitbooks.io/introduction-to-touchdesigner/content/GLSL/12-7-GPU-Particle-Systems.html), [Palette:particlesGpu – Derivative](https://derivative.ca/UserGuide/Palette:particlesGpu)).

**Instancing geometry against textures**: a `Geometry COMP` instances N copies
of a mesh; each instance reads its transform from a texture (R = posX, G = posY,
B = posZ, A = scale). The texture itself is the output of a feedback or compute
chain. This is how you get audio-reactive grids of 100k cubes for free.

**Audio-FFT through CHOPs into shader uniforms**: `Audio Spectrum CHOP` →
`Convert CHOP to TOP` so the spectrum becomes a 256×1 row of pixels →
sampled inside any shader. The bass column drives one uniform, treble another,
in a way that scales to many shaders without recomputing FFT.

**Scene depth as collision**: rendering a depth-only camera pass and treating it
as a heightfield that particles collide against, or as a soft-particle mask.
Combined with **Kinect/Azure depth** sources, this becomes "people in front of
the wall push the particles" with no skeleton tracking required.

**Pixel-sorting / displacement feedback**: feedback loop where each frame a tiny
horizontal swap is applied based on luminance; over many frames this organises
into glitch-art horizontal bars of sorted colour.

**Point-cloud sources**: increasingly common in 2025 — LiDAR scans
(Polycam, iPhone) loaded as positional textures, instanced as splats or used to
seed particle emitters.

**What makes a "TouchDesigner-feeling" output**: usually six or more layered
feedback passes of decreasing opacity, audio-coupled at multiple stages
(once on input intensity, once on feedback gain, once on hue rotation), with at
least one slow modulation oscillator (LFO at 0.05–0.5 Hz) drifting some
parameter so the image is never quite the same twice. Easel's compositing
pipeline already mirrors this structure; the things we're missing are: persistent
ping-pong FBOs exposed to shaders, scene-depth-as-collision, and an audio LFO
bus.

---

## 4. Gaussian splatting

**What it is, in 30 lines**: 3D Gaussian Splatting (3DGS), introduced in
SIGGRAPH 2023 (Kerbl, Kopanas et al, INRIA), represents a scene as millions of
3D Gaussian primitives — each with position, anisotropic covariance (so it's
an ellipsoid, not a sphere), opacity, and view-dependent colour encoded as
spherical harmonics. To render: project each Gaussian to screen space as a 2D
Gaussian, depth-sort, then alpha-blend back-to-front. There is no triangle, no
marching cube, no implicit surface — just a quasi-volumetric particle cloud
trained from photos. The result is photorealistic novel-view synthesis at 100+
fps at 1080p on a laptop GPU, from inputs that previously required hours of
NeRF training and sub-frame-rate playback ([Original 3DGS paper – arXiv 2308.04079](https://arxiv.org/abs/2308.04079), [graphdeco-inria/gaussian-splatting](https://github.com/graphdeco-inria/gaussian-splatting)).

**Why it's hot in 2025**: it eats NeRF's lunch (faster, more editable, no MLP
inference at render time), the input pipeline (a phone video) is consumer-grade,
and the data format compresses well — splats are starting to ship in glTF via
the Khronos *KHR_gaussian_splatting* extension drafted Feb 2026. Mario Lee
calls 2025 "the breakout year, splats becoming the JPEG of 3D" ([Khronos KHR_gaussian_splatting](https://www.thefuture3d.com/blog/state-of-gaussian-splatting-2026/), [Gaussian Splats Are Becoming the JPEG of 3D – Mario Lee, Medium](https://medium.com/@qsibmini123/gaussian-splats-are-becoming-the-jpeg-of-3d-why-2025-is-the-breakout-year-ac841ed39440)).

**OpenGL implementations**: *Splatapult* by hyperlogic is a C++/OpenGL
3DGS renderer; Open3D's filament path is implementing 3dgs vertex/fragment
shaders in ESSL 3.0; *FlashGS* (CVPR 2025) shows large-scale,
high-resolution rendering optimisations that fit a vanilla GPU. Aras Pranckevičius's
UnityGaussianSplatting (the popular Unity reference) is a useful porting model
because it stays at the OpenGL/DX feature level rather than relying on
Vulkan-specific tricks ([Splatapult GitHub](https://github.com/hyperlogic/splatapult), [FlashGS CVPR 2025 paper](https://openaccess.thecvf.com/content/CVPR2025/papers/Feng_FlashGS_Efficient_3D_Gaussian_Splatting_for_Large-scale_and_High-resolution_Rendering_CVPR_2025_paper.pdf), [Unigine 2.20 adds 3DGS – CG Channel](https://www.cgchannel.com/2025/07/unigine-2-20-adds-support-for-3d-gaussian-splatting/)).

**For real-time installations**: 3DGS turns a *space* (a room scanned in 5
minutes on an iPhone) into a navigable, photorealistic backdrop you can fly the
camera through and project onto a wall. The killer use case for Easel is
loading a splat of the venue itself and having it react to audio (each
splat's opacity multiplied by a per-frame audio uniform; the camera path
modulated by bass). For a small C++/OpenGL app, integration is feasible but
non-trivial: you need (a) a `.ply` parser for the splat format, (b) GPU sorting
each frame (radix sort in a compute shader is the standard), (c) a tile-based
rasteriser shader. Splatapult is ~5kloc and is the reference to read first; a
minimum-viable Easel integration is one ISF-shaped layer that ingests a static
splat and renders one camera path — call it 2–3 weeks of focused work.

---

## 5. Feedback-loop systems

The cellular-automaton aesthetic — patterns that emerge from per-pixel rules
iterated over time — is having a renaissance because it composes naturally with
audio reactivity and because it is *cheap*. A 1024×1024 reaction-diffusion
runs at hundreds of fps on integrated graphics.

**Reaction-Diffusion (Gray-Scott)**: two scalar fields `U, V` (chemical
concentrations) updated each frame: `U += Du*Δ²U − UV² + F(1−U)`; `V += Dv*Δ²V
+ UV² − (F+k)V`. The Laplacian Δ² is 4 texture taps. By varying F, k spatially
or temporally, you get spots, stripes, holes, mitosis-like splitting. It looks
*organic* in a way no closed-form shader can fake ([Reaction-Diffusion Tutorial – Karl Sims](https://karlsims.com/rd.html), [Gray-Scott shader – Pierre Couy](https://pierre-couy.dev/simulations/2024/09/gray-scott-shader.html), [ReactionDiffusionShader – amandaghassaei](https://github.com/amandaghassaei/ReactionDiffusionShader)).

**Iterative warping / ping-pong FBO design**: standard pattern is two FBOs,
swap roles each frame. Read from `prev`, write to `curr`. Apply (a) a small
displacement (e.g. rotate by 0.5°, scale by 0.99), (b) a colour decay
(`col *= 0.96`), (c) inject new content additively. The *Hydra* live-coding
system ([Hydra by Olivia Jack](https://hydra.ojack.xyz/)) is built almost
entirely on this pattern — its `feedback`, `modulate`, and `blend` functions
all collapse to a ping-pong with parameter modulation. *vvvv* and *Synesthesia*
expose the same primitives ([hydra-synth GitHub](https://github.com/hydra-synth/hydra), [Synesthesia VJ software](https://synesthesia.live/)).

**Audio-coupled feedback** is where it sings. The trick is *which* parameter
audio modulates. Bass on feedback gain (a higher gain = trails get longer on
the drop); mid on rotation (kicks make the world tilt); treble on injection
intensity (cymbals create new content). Coupling all three at once turns the
feedback loop into a visual instrument that "plays itself."

**Implication for Easel**: we should expose a generic two-FBO ping-pong
substrate so any shader can opt into "feedback mode." With that, Gray-Scott,
audio-reactive trails, and Hydra-style chains become writeable in <100 lines of
GLSL each.

---

## 6. UV sampling / texture-driven everything

The "data-as-texture" mindset is the dominant idiom of contemporary real-time
shader work. Once you accept that a `sampler2D` can store anything, every
problem simplifies: a position is a texture, a velocity is a texture, a colour
ramp is a texture, a mask is a texture, a histogram of audio over the last
second is a texture.

**Position fields**: a 256×256 RG32F texture stores 65536 particles' XY (or
RGBA32F for XYZW). Read in vertex shader via `gl_VertexID`-keyed sample, write
back from a fragment-shader simulation pass. This is the GPU-particle pattern
used everywhere from Niagara to TouchDesigner to Three.js GPGPU.

**Velocity fields / flow fields**: a 2D vector field stored as RG, sampled
in the simulation step to advect particles. Curl-noise is one source; analytic
fields (rotation, turbulence) another; a *user-painted* field is a third — VJs
literally draw with a brush into an FBO and particles follow.

**Colour LUTs**: 1D or 2D LUT for tonemapping (AGX/Tony McMapface ship as
LUTs), palette lookups, hue remapping. A 256×1 strip of pixels is a palette;
sampling it with `texture(palette, vec2(t, 0.5))` swaps a continuous
parameter for a hand-tuned colour curve, far prettier than `mix(a,b,t)`.

**Displacement maps / heightfields**: textures interpreted as Z. Standard for
terrain, but in shader-art it's how you pretend to have geometry: sample a
luminance map, raise/lower per-pixel, add fake lighting, compositing pass.

**Mask-based blending**: every layer carries a mask that decides where it draws.
Audio-driven masks (FFT bin → opacity) are the single cleanest way to make
audio-reactive layered scenes without baking audio into every shader.

**Implication for Easel**: every shader spec should consider, "what if `inputTex`
is *data*, not just a video frame?" The 15-shader spec already does this in
places (CHLADNI uses inputTex as plate texture, MEMPHIS uses it as a mask). The
new specs should keep doing it.

---

## 7. Audio-reactive depth illusions

Pseudo-3D sells when (a) parallax is wired to motion, (b) lighting is
view-dependent, and (c) the silhouette behaves believably under camera motion.
None of these requires actual 3D geometry.

**Parallax layers**: stack N 2D images, move each at a different rate as the
"camera" moves. The classic 16-bit-game-trick. In a shader, that camera is
`mousePos` or an audio-driven offset. Three layers (background, midground, foreground)
moving at 0.2x / 0.5x / 1.0x of an audio-driven velocity feel convincingly
volumetric for at most 30° of camera rotation.

**Depth-from-luminance** (Anaglyph trick on an LDR input): treat an image's
luminance as Z. Bright pixels are close, dark are far. Now you can apply
parallax shift, depth-of-field blur, fog, all without ever sampling a real
depth buffer. This is the trick behind a lot of "Magic Eye" stylized indie
shaders.

**Screen-space rotation via UV transformation**: rotate UV by a small angle as
a function of `length(uv-0.5)`, and a flat image acquires *swirl* — your eye
reads it as a rotating dome. Couple the angle to bass and the dome breathes.

**Concentric-ring depth**: one of the cheapest "into the scene" tricks —
render rings whose density grows as `1/r`, fog the edges, and the eye will read
infinite recession. Easel's `DEEP_CAVERN` shader is built on exactly this.

**Audio coupling** is where 2D-fakes become genuinely magical: link
parallax-shift amplitude to bass, depth-of-field strength to mid, fog density
to treble, and the image acquires a rhythmic plasticity that real 3D rarely
achieves at the same cost. For Easel's audience and budget, this is the right
target — a 2D shader that *feels* 3D, modulated by music, rather than a real 3D
scene that costs 10× to render and reacts less expressively.

**Closing thought**: the lesson of 2024–2026 is not that real-time graphics is
infinitely better than 2018, but that the *cheap* end has matured. A small
OpenGL host with smart shaders, well-chosen feedback loops, audio-coupled
tonemapping, and one or two Gaussian-splat backdrops is a credible
installation tool — credibly competitive with TouchDesigner for a defined
class of work, and a tractable scope for Easel.

---

## Sources (consolidated)

- [UE 5.7 Niagara Fluids documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-fluids-in-unreal-engine)
- [Unreal Fest Gold Coast 2024 – Advanced Niagara VFX talk](https://dev.epicgames.com/community/learning/talks-and-demos/pLpM/unreal-engine-advanced-niagara-vfx-fluids-simulation-stages-and-more-unreal-fest-gold-coast-2024)
- [CG Channel – UE 5.6 features](https://www.cgchannel.com/2025/01/see-the-new-features-due-in-unreal-engine-5-6-and-beyond/)
- [iRender – Lightweight Emitters](https://irendering.net/introduction-to-lightweight-emitters-for-niagara-in-unreal-engine/)
- [80.lv – UE 5.6 ray-traced translucency demo](https://80.lv/articles/check-out-unreal-engine-5-6-s-lumen-with-ray-traced-refraction-demo)
- [iRender – High Quality Lumen Reflections in 5.5](https://irendering.net/how-to-create-high-quality-lumen-reflections-in-unreal-engine-5-5/)
- [Reframing Tonemapping in Games – Alex Tardif](https://alextardif.com/Tonemapping.html)
- [AgX Unreal port (gist)](https://gist.github.com/nxrighthere/eb208dae8b66dbe452af223f276e46cc)
- [three.js AGX correctness discussion](https://discourse.threejs.org/t/is-agx-tonemapping-implemented-correctly/60609)
- [Tony McMapface adoption in Bevy / Godot proposal](https://github.com/godotengine/godot-proposals/issues/7263)
- [Bitangent Noise – atyuwen](https://atyuwen.github.io/posts/bitangent-noise/)
- [Visualizing Curl Noise – Shadertoy](https://www.shadertoy.com/view/mlsSWH)
- [Dissecting Curl Noise – Emil Dziewanowski](https://emildziewanowski.com/curl-noise/)
- [3D Game Shaders for Beginners – SSR](https://lettier.github.io/3d-game-shaders-for-beginners/screen-space-reflection.html)
- [3DGS paper (arXiv 2308.04079)](https://arxiv.org/abs/2308.04079)
- [INRIA 3DGS reference repo](https://github.com/graphdeco-inria/gaussian-splatting)
- [Splatapult – C++/OpenGL 3DGS renderer](https://github.com/hyperlogic/splatapult)
- [FlashGS – CVPR 2025](https://openaccess.thecvf.com/content/CVPR2025/papers/Feng_FlashGS_Efficient_3D_Gaussian_Splatting_for_Large-scale_and_High-resolution_Rendering_CVPR_2025_paper.pdf)
- [State of Gaussian Splatting 2026 – TheFuture3D](https://www.thefuture3d.com/blog/state-of-gaussian-splatting-2026/)
- [Gaussian Splats: Becoming the JPEG of 3D – Mario Lee, Medium](https://medium.com/@qsibmini123/gaussian-splats-are-becoming-the-jpeg-of-3d-why-2025-is-the-breakout-year-ac841ed39440)
- [Unigine 2.20 adds 3DGS – CG Channel](https://www.cgchannel.com/2025/07/unigine-2-20-adds-support-for-3d-gaussian-splatting/)
- [TouchDesigner GPU Particles + Feedback – AllTouchDesigner](https://alltd.org/touchdesignerparticles-gpu-feedback-effects/)
- [Introduction to TouchDesigner – GPU Particle Systems](https://nvoid.gitbooks.io/introduction-to-touchdesigner/content/GLSL/12-7-GPU-Particle-Systems.html)
- [Palette:particlesGpu – Derivative](https://derivative.ca/UserGuide/Palette:particlesGpu)
- [Hydra by Olivia Jack](https://hydra.ojack.xyz/)
- [hydra-synth GitHub](https://github.com/hydra-synth/hydra)
- [Synesthesia – live music visualizer](https://synesthesia.live/)
- [Reaction-Diffusion Tutorial – Karl Sims](https://karlsims.com/rd.html)
- [Gray-Scott shader writeup – Pierre Couy 2024](https://pierre-couy.dev/simulations/2024/09/gray-scott-shader.html)
- [ReactionDiffusionShader – amandaghassaei](https://github.com/amandaghassaei/ReactionDiffusionShader)
- [Reaction-Diffusion Compute Shader in WebGPU – Tympanus](https://prototypr.io/news/react-diffusion-webgpu)
- [Stable Diffusion latent space walks – Keras docs](https://keras.io/examples/generative/random_walks_with_stable_diffusion/)
