# Easel - C++ Projection Mapping Application

## Build System
- CMake 3.20+ with C++17
- Windows: Visual Studio 2022 (MSVC), solution: `build/Easel.sln`
- macOS: Apple Clang (Xcode Command Line Tools), Makefiles

## Build Commands
### Windows
```bash
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### macOS
```bash
brew install cmake ffmpeg opencv
mkdir build && cd build
cmake ..
cmake --build .
```

## Dependencies (auto-fetched via FetchContent)
- GLFW 3.4, GLM, Dear ImGui (docking), nlohmann/json, Catch2, libdatachannel

## External Pre-built (in external/)
- FFmpeg (GPL shared) - video codec
- OpenCV - scanning (optional)
- NDI SDK headers - network video
- kiss_fft - public domain FFT (manually placed in `external/kiss_fft/`)

## Conditional Features
- `HAS_FFMPEG` - VideoSource, RTMPOutput, VideoRecorder
- `HAS_OPENCV` - WebcamSource, SceneScanner
- `HAS_NDI` - NDISource, NDIOutput
- `HAS_WHEP` - WHEPSource (requires libdatachannel + FFmpeg, currently disabled)
- `HAS_WHISPER` - WhisperSpeech (CUDA arch 89, currently disabled — slow compile)

## Architecture

### Audio Analysis (`src/app/AudioAnalyzer`)
WASAPI loopback capture → kiss_fft FFT → 4-band frequency analysis (bass, lowMid, highMid, treble) + RMS + beat detection. Updates per-frame, drives audio-reactive shader uniforms and composite blend effects.

### DataBus (`src/app/DataBus`)
Named key-value store for routing live data to shader text parameters. Writers (EthereaClient, future MIDI/OSC) push values by key; consumers (ISF text inputs) bind via `layerId:paramName → dataKey`.

### EthereaClient (`src/speech/EthereaClient`)
Connects to Etherea server via WebSocket (real-time transcript) and SSE (hints/state). Pushes transcript text to DataBus for shader consumption. Replaces the older SSE-only EthereaTranscript.

### Compositing Pipeline
Layers composited via ping-pong FBO with blend modes. `AudioState` struct carries frequency bands + beat info to composite/passthrough shaders. ShaderSource (ISF) receives audio state via `setAudioState()` for `audioLevel`, `audioBass`, `audioMid`, `audioHigh`, `audioFFT` uniforms.

## Testing
```bash
cmake --build build --config Release --target test_audio_analyzer
build/Release/test_audio_analyzer.exe
```
