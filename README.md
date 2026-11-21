# GKPRO Recorder

A free, lightweight screen and game recorder for low-end Windows PCs.
**No watermark. No time limit. No ads.**

> **v0.2 beta.** Tested on an Intel Core i3-8145U (UHD 620). Feedback and bug reports are welcome.

## Features
- **Records one game window**, not your whole desktop (or the full screen if you prefer), with the mouse cursor included
- **Audio:** game sound, plus an optional microphone on its own track. No audio UI, just tray toggles
- **Built-in MKV to MP4 converter:** an MP4 is made automatically after each recording (fast, no re-encoding)
- **Hardware encoding:** H.264 on the GPU (Intel QuickSync, NVIDIA NVENC, AMD AMF) for low CPU use
- **GPU-only video pipeline:** capture, scaling and color conversion stay on the GPU
- **720p / 30 fps** by default at ~5 Mbps, for good quality on weak hardware
- **Crash-safe MKV recording:** a crash loses at most ~2 seconds of footage
- **Tiny tray app:** no window, no heavy UI framework; press **F9** to start/stop
- **Auto-stops and saves** if you close the game
- Saves to `Videos\GKPRO Recorder`, named after the game window

## How to use
1. Download `GKPRORecorder.exe` from the [Releases](../../releases) page.
2. Run it. A notification confirms it is running in the system tray (click the **^** arrow near the clock if the icon is hidden).
3. **Click your game window, then press F9** to start recording. Press **F9** again to stop.
4. Find your MKV and MP4 in `Videos\GKPRO Recorder`.

Right-click the tray icon to choose: record the active window or the full screen, game audio on/off, microphone on/off, automatic MP4 conversion, and whether to delete the MKV after converting.

Windows may show a **SmartScreen** warning because the app is unsigned. Click **More info → Run anyway**.

## Tips
- Use **windowed or borderless** mode for games. True exclusive fullscreen can record black.
- For Vulkan/DXVK games (for example GTA IV with DXVK), borderless windowed mode is the way to go. Reports are welcome.
- Advanced settings (fps, resolution, bitrate) are in `%LOCALAPPDATA%\GKPRO Recorder\settings.ini`.

## Known limitations (v0.2 beta)
- If the game window is resized during a recording, the video may crop or show black bars
- Audio can drift by a few tens of milliseconds on some PCs
- Windows may draw a yellow border around the recorded window (a Windows setting)
- Records the default audio devices only (no device picker yet)
- Needs a GPU with a hardware H.264 encoder (most PCs from the last ~10 years)
- Windows 10 version 1903 or newer is needed for window capture

## Roadmap
- [ ] Settings in the tray menu: resolution, fps and bitrate presets (720p30, 1080p30, 1080p60)
- [ ] Choose audio devices; separate volume for game and microphone
- [ ] Instant replay (save the last 30 seconds)
- [ ] FPS / recording overlay
- [ ] Published benchmarks (RAM and FPS impact) on real games
- [ ] Installer

## Build from source
No local setup is needed: pushing to GitHub builds the exe with GitHub Actions
(see `.github/workflows/build.yml`) and publishes it under **Actions → Artifacts**.
If the build fails, the compile errors are shown on the run's Summary page.

To build locally you need Visual Studio (Desktop C++) and CMake:
```
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Reporting bugs
Open an [Issue](../../issues) and attach the `.mkv.log` file from `Videos\GKPRO Recorder`,
plus your CPU/GPU and the game you were recording.

## License
MIT. See [LICENSE](LICENSE).
