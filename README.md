# WaveSnap

WaveSnap is a gesture-triggered shutter app for Sony PlayMemories Camera Apps, currently validated on the ILCE-5100. It launches directly into the camera preview, recognizes a horizontal hand wave, shows a three-second countdown, focuses, and captures one photo.

Package: `io.pihda.wavesnap`

## Camera controls

- Wave horizontally to start the countdown and automatic capture.
- Half-press the shutter button to autofocus manually.
- Fully press the shutter button to capture manually; this overrides a pending automatic countdown.
- Press trash/delete to exit.

After capture, WaveSnap waits for a three-second cooldown and one second of stillness before rearming. If analytical preview is unavailable, normal preview and manual controls remain usable.

## Build and installation

The project intentionally targets the Sony camera's legacy Android 2.3 runtime: Android API 10, Java 6 bytecode, `armeabi`, Android Gradle Plugin 2.2.3, Gradle 2.14.1, JDK 8, and NDK r14b.

Use the checked-in workflow scripts:

```sh
./scripts/format.sh --check
./scripts/smoke-test.sh
./scripts/install.sh
```

The app writes diagnostics to `WAVESNAP/LOG.TXT` on the camera storage. Collect and analyze it with:

```sh
./scripts/collect-log.sh <label>
./scripts/analyze-gesture-log.sh ../toolchain/device-logs/<label>.log
```

See `DEVICE_FINDINGS.md` for tested behavior, detector thresholds, and unresolved hardware questions.

## Heritage

WaveSnap was extracted from ma1co's PMCADemo and continues to use the OpenMemories Framework and Sony-specific camera APIs. The original license is retained in `LICENSE.txt`.
