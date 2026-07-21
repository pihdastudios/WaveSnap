# WaveSnap Sony a5100 device findings

Last updated: 2026-07-22. Treat only entries explicitly marked tested as a5100 results.

## Tested hardware and toolchain

- Camera: ILCE-5100; Sony runtime reports model/product `ScalarA`, display `GWK74`, incremental firmware `12`.
- Host build: Amazon Corretto JDK 8.242, Gradle 2.14.1, Android API 10, Build Tools 25.0.2, NDK r14b with GCC 4.9, `armeabi` only.
- Installation: Sony-PMCA-RE commit `a82f5ba` over the camera's Mass Storage USB mode (`054c:07cd`).

## Device-tested stages

1. The source PMCADemo preview, half-press autofocus, full shutter, image saving, and trash/back work.
2. The separate gesture activity preserves the same preview and physical controls and survives repeated entry/exit.
3. `CameraSequence` works concurrently with normal preview using the current probe: 640×480, rate 30000, format 256, one queued frame, 262144-byte maximum buffer. Payloads are JPEG, normally about 55–83 KiB. Every observed buffer was released and shutdown order was worker, sequence, native detector, then `CameraEx`.
4. PicoJPEG reduced decode is stable. Full decode was only about 3 FPS; the original reduced/DC path usually produced about 7–8.4 analytical FPS at roughly 112–130 ms processing time. A later Stage 8 session ranged from 5.95 FPS with roughly 90 KiB JPEGs to 9.33 FPS with roughly 45 KiB JPEGs and ended at 101 ms cumulative average processing time. The original 160×120 detector merely duplicated the decoder's effective 80×60 DC samples.
5. Motion metrics distinguish abrupt whole-camera movement and exposure jumps. Observed global movement commonly reached 370–504 permille and was rejected by the 350-permille limit.
6. The first horizontal reversal detector displays `WAVE` on real hand waves. It generally requires a relatively slow wave and a hand close enough to form at least an 8-permille tracked component. Slowly panning and reversing the camera can also produce a false `WAVE`.

7. Countdown, automatic capture, repeat capture, preview continuity, three-second cooldown, and stillness rearm work on the a5100. The first build exposed a missing automatic shutter release: later calls threw `RuntimeException: takePicture failed`. The corrected build issued three successful automatic capture requests and three `cancelTakePicture()` releases 418–428 ms later, with no capture error. Autofocus occurred about +2.01 seconds and capture about +3.02 seconds after countdown start. A wave confirmed during cooldown did not start a countdown. Analytical preview recovered from each capture and continued around 9 FPS.
8. Pressing trash/back during countdown invalidated the sequence and completed worker → sequence → native → camera shutdown with no later autofocus or capture. Pressing the physical shutter during countdown cancelled the automatic callbacks, requested one manual capture, released it on key-up, and then entered cooldown as designed. Four native wave confirmations during cooldown were logged as ignored without starting a countdown. Repeated five-cycle open/close testing and injected SD-card, low-battery, lens-removal, and unsupported-`CameraSequence` failures remain untested.
9. The 80×60/luma-only/fast-Huffman build is substantially faster on-device. Its cumulative native processing time ended at 65 ms versus 101 ms for the prior build, a roughly 36% reduction. Normal analytical windows held about 9.0–9.18 FPS; capture/cooldown windows temporarily lowered the all-window average to 8.41 FPS. Two accepted waves produced exactly two autofocus, capture, and shutter-release operations with no failure. The same session recorded 37 background-translation rejections. User testing found slow waves reliable, ordinary waves still slightly slow, and quick waves hit-or-miss under the former five-frame/0.4-second gate.
10. The 90 ms polling/four-frame build ended at 41 ms cumulative native processing and about 10.0–10.2 FPS outside capture, showing that Sony frame delivery—not JPEG compute—is now the sampling ceiling. Eight accepted waves produced eight autofocus/capture/release cycles without failure. Confirmation-to-countdown UI took about 53–67 ms; autofocus remained about +2 seconds and capture about +3 seconds, so the visible three-second delay is the required countdown rather than detector latency.
11. The pre-extraction local-flow build accepted three waves in about 23 seconds. All three are inferred as flow confirmations from the packed local-flow flag and unavailable centroid; the first two completed autofocus, capture, and shutter release with no failure, while leaving during the third countdown produced a clean worker → sequence → native → camera shutdown. Normal windows reached 9.89–10.03 FPS, capture windows fell to 4.14–4.50 FPS, and final average native processing was 49 ms.
12. WaveSnap 1.0.0 (`io.pihda.wavesnap`, APK SHA-256 `f0a1297fb3c6b1958e8eff4bda8aefeba1bfaed80678f96d595bdea0b26661c5`) is device-tested. Direct launch, preview, gesture capture, manual autofocus, manual shutter override, trash exit during countdown, cooldown/rearm, and reopening all worked. Four sessions logged five accepted local-flow waves, one complete automatic capture/release, two manual override captures with matching releases, two cooldown rearms, and four clean shutdowns with no errors or exceptions. Normal analytical windows were 9.87–9.99 FPS with final average processing around 44–45 ms; the automatic-capture window was 4.09 FPS as expected while capture interrupted frame delivery.

## Current detector thresholds

- ROI: x 10–90%, y 5–65% of the detector frame.
- Per-pixel difference: minimum 10, adaptive multiplier 2, capped at 40.
- Global rejection: full frame 350 permille, ROI 450 permille, mean-luminance jump 20.
- Components: at least 6 pixels in the 80×60 DC image (equivalent to 24 duplicated pixels in the prior 160×120 image); at most 300 permille area, 850 permille ROI width, and 800 permille ROI height.
- Trajectory: at least 8 permille area and 20 permille ROI motion; first travel 24% of ROI width; reversal hysteresis 8%; return travel 20%; at least four candidate frames; 0.25–2.2 seconds; vertical drift at most 42% of ROI height; candidate gap at most 350 ms; refractory period 1.8 seconds. The four-frame/0.25-second temporal gate is host-tested but awaits a5100 validation.
- Stage 8 pan probe: background-only horizontal translation search of ±6 detector pixels, sampled every two pixels outside an expanded ROI; require average background difference 6 and at least 180-permille SAD improvement. The flag is encoded in an unused result-nibble bit without changing the JNI call count. The first device log recorded five translation rejections and successful waves/captures, but controlled pan-versus-wave validation is still incomplete.
- Stage 8 performance build: analyze the native 80×60 JPEG DC image directly, remove duplicated expansion and smoothing, reuse three byte planes, use a 16-bit component queue, skip reduced-mode RGB conversion, add an 8-bit Huffman prefix table with canonical fallback, and compile ARM mode at `-O3`. Native detector workspace falls from about 154 KiB to 24 KiB. The 10 FPS polling ceiling remains intentional. The 65 ms final native average and normal 9+ FPS windows are device-validated.
- Experimental optical flow: 4×4 blocks stepped every four pixels, ±16 horizontal/±2 vertical search, at least four motion pixels, zero-shift average difference 8, best-match average at most 30, 300-permille SAD improvement, 2–64 supporting blocks, and 550-permille direction consensus. Flow trajectory requires 16% first-direction travel, 12% reverse travel, at least two vectors, 80–1500 ms duration, and gaps no longer than 300 ms. Global motion, exposure change, and background camera translation reset it. One a5100 session produced three consistent flow-confirmed waves without capture errors; controlled ordinary-wave, quick-wave, and reversing-pan coverage remains pending.
- Stage 7 rearm: three-second cooldown plus one second at or below 20-permille ROI motion and 35-permille global motion. The three-second deadline and motion-delayed rearm are device-validated.

## Reverted experiment

A second Stage 6 pass lowered the tracked area to 5 permille, reduced the minimum to four frames/0.3 seconds, and rejected motion active in six of twelve full-frame tiles. On the a5100 it produced no `WAVE`: normal hand motion repeatedly set the global flag even around 50–120 permille. The experiment was reverted in full. Do not restore it without a diagnostic that separately records per-tile counts.

## Logs

Collected logs live outside the Git repository in `../toolchain/device-logs/`:

- `stage3-camera-sequence.log`
- `stage4-reduced.log`
- `stage5-motion.log`
- `stage6-wave-first-pass.log`
- `stage6-wave-tile-rejected.log`
- `stage7-first-capture.log`
- `stage7-repeat-capture.log`
- `stage8-lifecycle.log`
- `stage8-pan-translation.log`
- `stage8-optimized-fast.log`
- `stage8-quick-wave-90ms.log`
- `stage8-local-flow-consistent.log`
- `wavesnap-v1-validation.log`

## Unresolved device questions

- Only the current `CameraSequence.Options` probe is verified; supported alternatives and exact meaning of Sony format value 256 remain undocumented.
- There is no capture-completion callback in the existing `takePicture(null, null, null)` path, so Stage 7 treats successful request return as the start of cooldown and releases the automatic shutter after a fixed 400 ms. The minimum safe release delay remains an empirical assumption.
- Repeated automated `takePicture` while the analytical sequence runs is tested. Normal/analytical preview continued without a restart.
- Slow-pan rejection needs a controlled follow-up even though the first translation-probe session recorded five background shifts without breaking two automatic captures.
- The fused optical-flow/centroid build still needs controlled quick-wave, ordinary-wave, and reversing-pan tests. The source repository smoke suite passed 49 native assertions under AddressSanitizer plus the legacy Android build and ARMv5TE APK checks before WaveSnap extraction.
