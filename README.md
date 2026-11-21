# GKPRO Recorder

A free, lightweight screen and game recorder for low-end Windows PCs.
**No watermark. No time limit. No ads.**

> **v0.1 beta, video only.** Tested on an Intel Core i3-8145U (UHD 620). Feedback and bug reports are welcome.

## Features
- **Hardware encoding:** H.264 on the GPU (Intel QuickSync, NVIDIA NVENC, AMD AMF) for low CPU use
- **GPU-only pipeline:** capture, scaling and color conversion stay on the GPU, with no CPU frame copies
- **720p / 30 fps** by default at ~5 Mbps, for good quality on weak hardware
- **MKV output:** a crash loses at most ~2 seconds of footage
- **Tiny tray app:** no window, no heavy UI framework; press **F9** to start/stop
- **Runs at low priority** so it yields to your game
- **Saves to** `Videos\GKPRO Recorder`
- **Debug log** saved next to each recording (`.mkv.log`) for easy bug reports

## How to use
1. Download `GKPRORecorder.exe` from the [Releases](../../releases) page.
2. Run it. A message tells you it's running in the system tray (click the **^** arrow near the clock if you can't see the icon).
3. Press **F9** to start recording and **F9** again to stop.
4. Find your video in `Videos\GKPRO Recorder`.

Windows may show a **SmartScreen** warning because the app is unsigned. Click **More info → Run anyway**.

## Known limitations (v0.1 beta)
- No audio yet (game sound and mic are planned)
- The mouse cursor is not recorded yet
- Records the primary monitor only
- Exclusive-fullscreen games may record black; use **borderless windowed** mode
- Needs a GPU with a hardware H.264 encoder (most PCs from the last ~10 years)
- Output is MKV; to get MP4, remux with OBS (File → Remux Recordings) or similar

## Roadmap
- [ ] Audio: game sound and microphone as separate tracks
- [ ] Mouse cursor capture
- [ ] Settings: resolution, fps and bitrate presets (720p30, 1080p30, 1080p60)
- [ ] Window / game selection
- [ ] Instant replay (save the last 30 seconds)
- [ ] FPS / recording overlay
- [ ] Published benchmarks (RAM and FPS impact) on real games
- [ ] Installer

## Build from source
No local setup is needed: pushing to GitHub builds the exe with GitHub Actions
(see `.github/workflows/build.yml`) and publishes it under **Actions → Artifacts**.

To build locally you need Visual Studio 2022 (Desktop C++) and CMake:
```
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Reporting bugs
Open an [Issue](../../issues) and attach the `.mkv.log` file from `Videos\GKPRO Recorder`,
plus your CPU/GPU and the game you were recording.

## License
MIT. See [LICENSE](LICENSE).
