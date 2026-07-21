---
name: sony-pmca-workflow
description: Build, verify, install, and diagnose the legacy Sony WaveSnap Android application with the pinned JDK 8, Gradle 2.14.1, API 10 SDK, NDK r14b armeabi toolchain, Sony-PMCA-RE, and a connected a5100. Use for WaveSnap build requests, APK inspection, camera installation, USB-mode checks, device-log collection, and validation.
---

# Sony PMCA Workflow

Read `AGENTS.md` before changing camera or JNI code. Read `DEVICE_FINDINGS.md` before tuning the detector or reporting device behavior. Preserve the hard stage gates and distinguish build success from a5100-tested behavior.

Run commands from the WaveSnap source root:

- Run the complete host/native/build/APK smoke suite: `./scripts/smoke-test.sh`
- Run fast host/native checks without Gradle: `./scripts/smoke-test.sh --host-only`
- Build and verify the newest debug APK: `./scripts/build.sh`
- Force a clean build: `./scripts/build.sh --clean`
- Verify an existing APK: `./scripts/verify-apk.sh [apk]`
- Check camera USB mode: `./scripts/usb-status.sh`
- Install the newest APK: `./scripts/install.sh [apk]`
- Build, verify, and install: `./scripts/build-install.sh [--clean]`
- Snapshot and filter the device log: `./scripts/collect-log.sh <stage-label>`
- Analyze the latest activity session in a collected log: `./scripts/analyze-gesture-log.sh <log-file>`
- Report pinned host tool versions: `./scripts/toolchain-info.sh`

Use offline builds by default. Pass `--online` only when dependency resolution is intentionally required. Treat installation and USB access as operations that may require sandbox escalation.

Before reporting device validation:

1. Run the complete smoke suite.
2. Install through the script with the a5100 in Mass Storage mode.
3. Ask for the stage-specific physical camera test.
4. Reconnect the camera and collect its log.
5. Inspect errors, resource-release order, frame metrics, and timing before marking the stage complete.

Preserve the current first-pass Stage 6 percentage thresholds unless a new diagnostic log justifies a change. The detector uses the JPEG decoder's native 80×60 DC grid and Sony delivers about 10 analytical FPS even when polled faster; do not pursue incompatible SIMD to solve sampling limits. The quick-wave build fuses the centroid trajectory with experimental preallocated 4×4 local optical flow and continues past oversized components to find plausible regions. Tune flow support/confidence only from `localFlow`/`blocks` logs. Do not reintroduce the reverted 4×3 tile-spread rule. Stage 7 repeat capture is device-tested with a guarded 400 ms `cancelTakePicture()` release and three-second cooldown. Use the log analyzer to check capture/release, flow-confirmed-wave, FPS, and process-time counts. Treat each new sensitivity adjustment as experimental until ordinary waves, slow pan reversal, decode stability, and analytical FPS pass on the a5100.

Do not hand-write the full Gradle or PMCA command when these scripts cover the operation. Do not claim hardware success from an install result alone.
