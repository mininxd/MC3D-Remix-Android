# Midnight Club 3: DUB Edition Remix (Android)

<p align="center">
  <img src="logo.png" alt="MC3: DUB Edition Remix Logo" width="360"/>
</p>

<p align="center">
  <strong>Static Recompilation Native Android Launcher & Host</strong><br>
  Built with <strong>100% Pure Rust</strong> — Zero Java, Zero C.
</p>

---

## Overview

This repository houses the Android launcher and native host application for **Midnight Club 3: DUB Edition Remix**, targeting modern 64-bit ARM devices (`aarch64-linux-android`).

Unlike standard Android apps, this project uses **`android-activity` and `cargo-apk`** to compile directly into a native `NativeActivity` APK with **no JVM/Java runtime overhead**.

> [!IMPORTANT]
> **No Copyrighted Game Assets**: This repository contains **NO** game binaries (`.xbe`), disc images (`.iso`), sound banks, or texture archives. Users provide their own legal copy of the game on device storage.

---

## Features

- **Pure Rust Architecture**: 100% Rust using `android-activity` (`NativeActivity` backend). Zero Java / Kotlin.
- **On-Device ISO Discovery**: Automatically scans device storage (`/sdcard/xbox/`, `/sdcard/Download/`, `/sdcard/ROMs/`) and verifies Xbox XDVDFS disc headers (`MICROSOFT*XBOX*MEDIA`).
- **Graphics & Display Customization**:
  - **Resolution Scaling**: 1.0x (480p Native), 1.5x (720p HD), 2.0x (1080p Full HD), 3.0x (1440p 2K).
  - **Aspect Ratio**: 16:9 Widescreen (activates Xbox EEPROM widescreen mode and engine UI) or 4:3 Standard.
  - **Frame Rate Limiter**: 30 FPS Lock, 60 FPS Mode, or Uncapped.
  - **VSync**: Tear-free display synchronization toggle.
  - **Texture Filtering**: Bilinear, Anisotropic 8x, or Anisotropic 16x.
- **Persistent Settings**: Saves user configuration to Android app data as JSON (`mc3dr_config.json`).
- **Cloud CI Builds**: Automated APK packaging via GitHub Actions with `cargo-apk` and Android NDK r26d.

---

## Project Structure

```text
mc3dr/
├── .github/workflows/
│   └── build.yml       # GitHub Actions workflow for building the APK
├── src/
│   ├── lib.rs          # NativeActivity entry point (android_main) & event pump
│   ├── config.rs       # Persistent graphics, display, and controller settings
│   ├── iso.rs          # XDVDFS ISO disc header parser & storage scanner
│   └── ui.rs           # Launcher screens & interactive state machine
├── Cargo.toml          # Rust package & Android APK manifest metadata
├── logo.png            # Application icon / banner
└── README.md
```

---

## Building the APK

Per project guidelines, APK builds are automated in the cloud via **GitHub Actions** (`.github/workflows/build.yml`) on every push and pull request.

To trigger a build:
1. Push your commit to GitHub:
   ```bash
   git push origin main
   ```
2. Navigate to the **Actions** tab on your GitHub repository.
3. Download the built APK artifact: `MC3D-Remix-Android-Rust-APK`.
