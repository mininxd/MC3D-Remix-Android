# Midnight Club 3: DUB Edition Remix (Android Recomp)

<p align="center">
  <img src="logo.png" alt="MC3: DUB Edition Remix Logo" width="360"/>
</p>

<p align="center">
  <strong>Static Recompilation Native Android Port (ARM64-v8a)</strong><br>
  Built with <strong>XboxXBERecomp</strong>, <strong>SDL3</strong>, and <strong>Vulkan 1.1</strong>.
</p>

---

## Overview

An unofficial Android (arm64-v8a) port of the original Xbox game **Midnight Club 3: DUB Edition Remix**, recompiled ahead-of-time using the XboxXBERecomp SDK.

The game's x86 code in `default.xbe` (Microsoft XDK 5849) is statically recompiled into 64 optimized C++ partitions. That C++ is built directly for Android together with the SDK's portable runtime and native host engine:
- **Rendering**: Direct3D 8 translated to Vulkan 1.1 on a dedicated render thread.
- **Windowing & Input**: SDL3 host with complete physical Xbox / Bluetooth gamepad support.
- **Audio**: DirectSound software mixer, ADPCM streams, and reverb via SDL3 audio.
- **Filesystem**: Automatic ISO / extracted file discovery and XDVDFS mounting.

> [!IMPORTANT]
> **No Copyrighted Game Assets**: This repository contains **NO** game binaries (`.xbe`), disc images (`.iso`), or copyrighted game archives. Users must provide their own legal copy of Midnight Club 3: DUB Edition Remix on device storage.

---

## Requirements

- Android 10+ (API 29+), 64-bit ARM (`arm64-v8a`)
- GPU with Vulkan 1.1 support
- Physical Xbox / Bluetooth Gamepad (recommended)
- Midnight Club 3: DUB Edition Remix original Xbox game files (ISO or extracted files)

---

## How to Play

1. Install `MC3D-Remix.apk` on your Android device.
2. Grant "All files access" (Manage External Storage) permission when prompted.
3. Place your Midnight Club 3 disc image (`.iso`) or extracted game folder (with `default.xbe`, `assets_x.dat`, etc.) in:
   ```text
   /storage/emulated/0/mc3dr/
   ```
   *(or `/sdcard/mc3dr/`)*
4. Connect your Xbox controller and launch **MC3D Remix**.

---

## Architecture

```text
mc3dr/
├── app/                  # Android Gradle application module (MC3DActivity)
├── config/               # Game recompilation config (game.toml, XDK 5849 symbols)
├── generated/game/       # 64 partitions of lifted C++ game code (x86_recomp.*.cpp)
├── native/               # Android host, SDL3 windowing, Vulkan backend & gamepad input
├── src/
│   ├── d3d/              # Direct3D 8 (XDK 5849) command translation to Vulkan 1.1
│   ├── sound/            # DirectSound audio mixer & ADPCM streams
│   ├── common/           # Game info, settings menu & options
│   └── game/             # Game host integration
├── tools/                # Xbox symbol discovery and recompiler tools
├── CMakeLists.txt        # Native library (libmain.so) build script
├── build.gradle          # Root Gradle configuration
└── release.jks           # Release signing keystore
```

---

## Building the APK

Builds are automated via GitHub Actions on every push to `main` and on version tags (`v*`).

To build locally:
```bash
./gradlew assembleRelease
```
The output APK is generated at:
`app/build/outputs/apk/release/app-release.apk`
