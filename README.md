# Easel

Real-time projection mapping and live visual effects application built in C++/OpenGL. Designed for live performance, events, and creative installations.

Easel composites multiple content sources (shaders, video, screen capture, network streams) into output zones that can be warped and projected onto surfaces, streamed via RTMP/NDI, or displayed fullscreen.

## Features

### Multi-Source Compositing

- **ISF Shaders** — Full [Interactive Shader Format](https://isf.video/) support with multi-pass rendering, persistent buffers, and hot-reload for live editing
- **Video** — H.264/H.265 playback via FFmpeg with frame-accurate seeking
- **Screen & Window Capture** — ScreenCaptureKit (macOS) / Windows Graphics Capture
- **NDI** — Network Device Interface input for receiving video over LAN
- **WHEP** — WebRTC pull streaming via embedded WebKit (macOS)
- **Images** — Static image sources

### Layer Effects & Blending

- 7 blend modes (Normal, Multiply, Screen, Overlay, Add, Subtract, Difference)
- Per-layer effects: Blur, Color Adjust, Invert, Pixelate, Feedback (trails/echo)
- Transitions: Fade, Wipe, Dissolve
- Mosaic tiling modes (Mirror, Hex) with audio-reactive density
- Edge feather, crop controls, and auto-crop for black border removal
- Bezier curve masking

### Audio Reactivity

- FFT-based frequency analysis via kiss_fft (bass, low-mid, high-mid, treble bands)
- Beat detection with configurable cooldown
- BPM sync with tap tempo and manual phase nudge
- Audio state injected into shaders as uniforms (`audioBass`, `audioMid`, `audioHigh`, `audioFFT`, `beatPhase`, `beatPulse`)
- Multi-device audio mixing
- Platform audio: WASAPI loopback (Windows), CoreAudio (macOS)

### Projection Mapping

- Multi-zone output with independent compositing per zone
- Corner-pin warp (4-point perspective via homography)
- Deformable mesh warp (adjustable grid resolution)
- 3D object mesh warp (load .obj files for curved surfaces)
- Edge blending between projectors
- Optional auto-calibration via structured light scanning (requires OpenCV)

### Output

- **Fullscreen** — Direct-to-projector with monitor auto-detection and reconnection
- **RTMP Streaming** — H.264 + AAC to Twitch, YouTube, etc. with async PBO readback
- **NDI Broadcast** — Per-zone or per-layer network video output
- **Per-layer NDI** — Send individual layers as separate NDI sources

### Live Control

- **MIDI** — Device enumeration, learn mode, CC/note mapping to layer parameters
- **OSC** — Open Sound Control receiver/sender over UDP
- **Scenes** — Snapshot and recall layer visibility, opacity, blend modes, effects, and transforms
- **Undo/Redo** — Full undo stack

### Speech & Data Integration

- **Etherea Client** — Real-time speech transcripts via WebSocket + SSE for hints/state
- **DataBus** — Named key-value routing from external data sources to shader text parameters

## Building

### Requirements

- CMake 3.20+
- C++17 compiler

### macOS (Apple Silicon)

```bash
brew install cmake ffmpeg
cmake -B build
cmake --build build
```

### Windows

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Optional: NDI Support

Download the NDI runtime from https://ndi.video/tools/ or extract manually:

```bash
curl -L -o /tmp/libNDI.pkg https://downloads.ndi.tv/SDK/NDI_SDK_Mac/libNDI_for_Mac.pkg
mkdir -p /tmp/ndi && cd /tmp/ndi && xar -xf /tmp/libNDI.pkg
cd libNDIComponent.pkg && cat Payload | gunzip | cpio -id
cp libndi.dylib <easel>/build/
```

## Dependencies

Auto-fetched via CMake FetchContent:

- [GLFW 3.4](https://www.glfw.org/) — windowing
- [GLM](https://github.com/g-truc/glm) — math
- [Dear ImGui](https://github.com/ocornut/imgui) (docking branch) — UI
- [nlohmann/json](https://github.com/nlohmann/json) — project serialization
- [Catch2](https://github.com/catchorg/Catch2) — testing
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) — WebRTC

External / pre-built:

- [FFmpeg](https://ffmpeg.org/) — video codec & streaming
- [OpenCV](https://opencv.org/) — scanning/calibration (optional)
- [NDI SDK](https://ndi.video/) — network video (optional)
- [kiss_fft](https://github.com/mborgerding/kissfft) — FFT for audio analysis

## Conditional Features

Features are enabled/disabled based on available libraries:

| Flag | Enables | Requires |
|------|---------|----------|
| `HAS_FFMPEG` | VideoSource, RTMPOutput, VideoRecorder | FFmpeg |
| `HAS_OPENCV` | SceneScanner, WebcamSource | OpenCV |
| `HAS_NDI` | NDISource, NDIOutput | NDI SDK runtime |
| `HAS_WHEP` | WHEPSource (WebRTC ingest) | libdatachannel + FFmpeg |

## Project Files

Easel projects save as `.easel` JSON files containing layer definitions, zone configurations, scene snapshots, MIDI mappings, and external integration settings.

## Testing

```bash
# macOS
cmake --build build --target test_audio_analyzer
./build/test_audio_analyzer

# Windows
cmake --build build --config Release --target test_audio_analyzer
build/Release/test_audio_analyzer.exe
```

Available test targets: `test_shaders`, `test_scanning`, `test_audio_analyzer`, `test_ndi_receive`, `test_ndi_capture`, `test_ndi_list`.
