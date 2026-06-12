[简体中文](README.md) | **English**

# USB DAC Player

Captures the **system media audio** that other apps are playing, bypasses Android's
mixing/resampling path, and sends it **exclusively** to a USB DAC dongle via
**libusb isochronous OUT transfers**.

> Author: 三水深 · License: Apache-2.0 · Only one third-party dependency: libusb (LGPL-2.1, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md))

```
Other apps' media audio
   │  AudioPlaybackCapture (MediaProjection, float)
   ▼
AudioRecord ──► 16/24/32-bit conversion ──► ring buffer
   ──► native libusb isochronous OUT (async-feedback loop) ──► USB DAC (dongle)
```

## Features

- **Exclusive output**: libusb takes over the USB audio interface and sends bits straight to the DAC.
- **Adaptive quality**: probes the DAC's supported bit depths / sample rates; "Auto" picks the highest, manual selection is also available and is **remembered per device (VID:PID)**.
- **Stable**: asynchronous feedback-endpoint loop + pre-buffering eliminate the dropouts caused by clock drift.
- **Clean stop**: resets the device on stop so normal system playback recovers automatically.
- **One-tap log sharing**: export device info + probe results + full runtime log for easy bug reports.

## Modules

| Module | Description |
| --- | --- |
| `:usbaudio` | Core library: `UsbAudioManager` (Kotlin) + JNI bridge + native engine `usb_audio_output.c` (UAC1/UAC2 parsing, sample-rate/volume control, isochronous transfers, async feedback, format probing) + prebuilt libusb. |
| `:app` | Player app: `MainActivity` (device/format selection, permissions, start/stop, log sharing) + foreground service `AudioCaptureService` (capture → USB pipeline). |

## Build

- **JDK 17** (required by AGP 8.2 / Gradle 8.5; the JBR bundled with Android Studio is easiest).
- Android SDK: platform `android-35`, build-tools `35.x`.
- **Signing**: the repo does **not** contain `release.keystore` (gitignored). Drop in your own
  keystore + a `keystore.properties` file, or use your own signing config (see `app/build.gradle`).
- The native engine ships **prebuilt** under `usbaudio/src/main/libusbLib/` (no CMake needed). To
  rebuild it from `cpp/usb_audio_output.c`, install CMake and re-enable the `externalNativeBuild`
  blocks in `usbaudio/build.gradle`.

```bash
./gradlew :app:assembleRelease    # output: app/build/outputs/apk/release/
./gradlew installDebug            # install to a connected device
```

> ABIs: `armeabi-v7a`, `arm64-v8a`, `x86_64`.

## Reporting bugs

Tap **"Share log"** in the app and send it to the author, or attach it to a GitHub Issue
(it includes phone model, dongle model, probed formats, and where it got stuck).
Forks / pull requests welcome.

## Known limitations (stated honestly)

- **Not bit-perfect**: the source is Android's 48 kHz system mix (upsampled to the chosen format),
  not the original file's bit-exact data. Running the DAC at 32-bit/high rates mainly leverages the
  DAC's better operating mode — it does not add information. True lossless / native DSD would need a
  "decode local files → DAC" player path (not implemented).
- **Capturability**: only apps that allow capture (`USAGE_MEDIA/GAME/UNKNOWN`, not opted out) can be captured.
- **Output power is hardware**: the app already outputs at full scale; for more drive, max the media
  volume and use the dongle's high-gain mode.
- **App-store publishing is hard**: capturing other apps' audio is policy-sensitive (especially
  Google Play, plus copyright), and the prebuilt libusb is not yet 16 KB-page aligned. **Sideloading is unaffected.**

## License

This project's code (author: 三水深) is licensed under **Apache-2.0**, see [LICENSE](LICENSE).
Third-party components: [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
