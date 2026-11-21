# GKPRO Recorder

Free, watermark-free, low-RAM game recorder for low-end PCs.

## Current status: MVP (video only)
- Primary monitor capture via DXGI Desktop Duplication (GPU texture, no CPU copy)
- Hardware H.264 via Media Foundation (NVENC / AMF / QuickSync, picked by Windows)
- Fragmented MP4 output (survives crashes), saved to `Videos\GKPRO Recorder`
- Tray app, **F9** start/stop, runs at below-normal thread priority
- Defaults: 30 fps, auto bitrate (~4.3 Mbps at 1080p30)

## Build (Windows)
Requires Visual Studio 2022 (Desktop C++ workload) and CMake.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```
Output: `build\Release\GKPRORecorder.exe`

## Roadmap
- [ ] Audio: WASAPI loopback + mic as separate tracks
- [ ] Window / game selection, fullscreen handling
- [ ] Low-end preset picker (720p30, 1080p30, 1080p60) + settings file
- [ ] Auto-throttle when game FPS drops
- [ ] Instant replay (ring buffer)
- [ ] FPS / recording overlay
- [ ] Installer + benchmarks vs Bandicam/OBS

## Known MVP limits
- Records monitor 1 only; stops if the display mode changes mid-recording
- No audio yet
- Exclusive-fullscreen games may need borderless mode for Desktop Duplication
- Software (x264) fallback for PCs with no GPU encoder is not added yet
