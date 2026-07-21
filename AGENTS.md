# WaveSnap Sony a5100 Engineering Guide

WaveSnap is a single-purpose legacy Sony PlayMemories Camera App. Treat the connected ILCE-5100 as constrained camera hardware, not as a generic Android phone.

## Compatibility contract

- Keep Android Gradle Plugin 2.2.3, Gradle 2.14.1, Java 6 bytecode, `compileSdkVersion 10`, `minSdkVersion 10`, `targetSdkVersion 10`, and Build Tools 25.0.2.
- Run Gradle with JDK 8. Build native code with NDK r14b, `APP_PLATFORM := android-10`, and `APP_ABI := armeabi`.
- Keep only the OpenMemories Framework and Sony stubs dependencies. Do not introduce AndroidX, Kotlin, Jetpack, modern Camera APIs, or a current OpenCV release.
- Preserve package/application ID `io.pihda.wavesnap` and direct launch into `GestureCameraActivity`.
- Use `CameraEx.open(0, null)`, the normal Sony preview path, and the existing `BaseActivity` key mappings.
- Preserve half-press autofocus, full-shutter capture, trash/delete exit, countdown cancellation, cooldown, and stillness rearm behavior.

## Safety and lifecycle

- Camera safety and deterministic release take priority over gesture sensitivity.
- Serialize camera state transitions on the UI thread. Use one bounded frame worker and at most one pending analytical frame.
- Never let a worker access `CameraEx` after release. Release every acquired `DeviceBuffer` in `finally`.
- Cancel countdown callbacks and invalidate trigger tokens before releasing sequence, native detector, and camera resources.
- Guard and log every Sony-specific operation through `Logger`; analytical failure must leave normal preview and manual controls usable.
- Never claim device behavior without testing it on the connected a5100 and collecting `WAVESNAP/LOG.TXT`.

## Local workflow

Toolchains and caches live under `../toolchain/`; JDK 8 is `/home/klm/.package-manager/jdk`. Use the project-local `sony-pmca-workflow` skill and scripts instead of reconstructing commands:

- `./scripts/smoke-test.sh [--host-only] [--clean]`
- `./scripts/build.sh [--clean]`
- `./scripts/install.sh [apk]`
- `./scripts/build-install.sh [--clean]`
- `./scripts/collect-log.sh <label>`
- `./scripts/analyze-gesture-log.sh <log-file>`
- `./scripts/toolchain-info.sh`

Use offline builds by default. Installation requires the a5100 in Mass Storage USB mode.

Before device validation, run the complete smoke suite. After installation, physically test gesture capture, manual override, countdown exit, cooldown/rearm, and repeated entry/exit; reconnect the camera, collect the log, and verify errors, capture/release balance, timing, FPS, and worker → sequence → native → camera shutdown order.

## Detector constraints

Read `DEVICE_FINDINGS.md` before tuning or reporting device behavior. The detector consumes the JPEG decoder's real 80×60 DC grid at about 10 analytical FPS. Preserve the current centroid thresholds and experimental preallocated 4×4 local optical-flow fallback unless a new diagnostic log justifies a change. Do not restore the rejected 4×3 tile-spread rule.

Keep Java compatible with Java 6 syntax, avoid per-frame heap allocation, use exact pinned Sony method descriptors, and run host smoke tests while iterating plus the complete suite before installation.
