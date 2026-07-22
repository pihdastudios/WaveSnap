#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg_fixture.h"

extern "C" int __android_log_print(int, const char*, const char*, ...) {
    return 0;
}

/* Compile the production detector into this host test translation unit. */
#include "gesture_detector.cpp"

namespace {

int gFailures = 0;
int gAssertions = 0;

void expectTrue(bool condition, const char* message) {
    ++gAssertions;
    if (!condition) {
        ++gFailures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

void resetWaveCompletely() {
    clearWaveTrajectory();
    gWave.lastConfirmedTimestampMs = 0;
}

int feedTrajectory(const int* x, const int* y, const int* timeMs, int count) {
    int result = GESTURE_RESULT_NONE;
    for (int i = 0; i < count; ++i) {
        int flags = 0;
        result = updateWaveTrajectory(x[i], y[i], 50, 100, timeMs[i], 64, 36, &flags);
    }
    return result;
}

void testFastWaveBothDirections() {
    const int timeMs[] = {0, 90, 180, 270};
    const int steadyY[] = {16, 16, 17, 16};
    const int leftRightLeft[] = {10, 32, 42, 26};
    const int rightLeftRight[] = {54, 32, 22, 38};

    resetWaveCompletely();
    expectTrue(feedTrajectory(leftRightLeft, steadyY, timeMs, 4) == GESTURE_RESULT_WAVE,
               "270 ms left-right-left wave must be recognized");

    resetWaveCompletely();
    expectTrue(feedTrajectory(rightLeftRight, steadyY, timeMs, 4) == GESTURE_RESULT_WAVE,
               "270 ms right-left-right wave must be recognized");
}

void testTrajectoryRejections() {
    const int timeMs[] = {0, 100, 200, 300, 400, 500};
    const int steadyY[] = {16, 16, 16, 16, 16, 16};
    const int monotonic[] = {10, 20, 30, 40, 50, 58};
    const int tooSmall[] = {28, 31, 34, 30, 27, 29};
    const int verticalX[] = {10, 32, 42, 26};
    const int verticalY[] = {5, 8, 22, 24};

    resetWaveCompletely();
    expectTrue(feedTrajectory(monotonic, steadyY, timeMs, 6) != GESTURE_RESULT_WAVE,
               "one-direction motion must not be a wave");

    resetWaveCompletely();
    expectTrue(feedTrajectory(tooSmall, steadyY, timeMs, 6) != GESTURE_RESULT_WAVE,
               "small jitter must not be a wave");

    resetWaveCompletely();
    int result = GESTURE_RESULT_NONE;
    int flags = 0;
    for (int i = 0; i < 4; ++i) {
        flags = 0;
        result =
            updateWaveTrajectory(verticalX[i], verticalY[i], 50, 100, timeMs[i], 64, 36, &flags);
        if ((flags & GESTURE_FLAG_TRAJECTORY_REJECTED) != 0)
            break;
    }
    expectTrue(result != GESTURE_RESULT_WAVE, "large vertical drift must not be a wave");
    expectTrue((flags & GESTURE_FLAG_TRAJECTORY_REJECTED) != 0,
               "vertical drift must report trajectory rejection");

    resetWaveCompletely();
    flags = 0;
    updateWaveTrajectory(10, 16, 50, 100, 0, 64, 36, &flags);
    flags = 0;
    updateWaveTrajectory(32, 16, 50, 100, 100, 64, 36, &flags);
    flags = GESTURE_FLAG_GLOBAL_MOTION;
    result = updateWaveTrajectory(42, 16, 50, 100, 200, 64, 36, &flags);
    expectTrue(result == GESTURE_RESULT_NONE, "global motion must cancel a trajectory");
    expectTrue((flags & GESTURE_FLAG_TRAJECTORY_REJECTED) != 0,
               "global motion must report trajectory rejection");
}

void testRefractoryPeriod() {
    const int x[] = {10, 32, 42, 26};
    const int y[] = {16, 16, 16, 16};
    const int firstTime[] = {0, 100, 200, 300};
    const int blockedTime[] = {900, 1000, 1100, 1200};
    const int laterTime[] = {2200, 2300, 2400, 2500};

    resetWaveCompletely();
    expectTrue(feedTrajectory(x, y, firstTime, 4) == GESTURE_RESULT_WAVE,
               "first wave must establish refractory period");
    expectTrue(feedTrajectory(x, y, blockedTime, 4) != GESTURE_RESULT_WAVE,
               "wave inside refractory period must be ignored");
    expectTrue(feedTrajectory(x, y, laterTime, 4) == GESTURE_RESULT_WAVE,
               "wave after refractory period must be accepted");
}

void testPackedMetrics() {
    int flags = GESTURE_FLAG_EXPOSURE_CHANGE | GESTURE_FLAG_CAMERA_TRANSLATION;
    int64_t packed = packMetrics(GESTURE_RESULT_WAVE, 123, 456, 789, 19, 17, 91, flags);
    expectTrue((packed & 0x3) == GESTURE_RESULT_WAVE, "packed result code must round-trip");
    expectTrue((packed & 0x4) != 0, "camera translation must use extended result bit");
    expectTrue(((packed >> kMeanLumaShift) & 0xff) == 123, "packed luminance must round-trip");
    expectTrue(((packed >> kMotionPermilleShift) & 0x3ff) == 456,
               "packed ROI motion must round-trip");
    expectTrue(((packed >> kGlobalPermilleShift) & 0x3ff) == 789,
               "packed global motion must round-trip");
    expectTrue(((packed >> kAreaPermilleShift) & 0x3ff) == 91,
               "packed component area must round-trip");
    expectTrue(((packed >> kFlagsShift) & 0x1f) == GESTURE_FLAG_EXPOSURE_CHANGE,
               "packed base flags must exclude extended translation bit");

    packed = packMetrics(GESTURE_RESULT_MOTION, 80, 90, 70, 44, 5, 20, GESTURE_FLAG_LOCAL_FLOW);
    expectTrue((packed & 0x8) != 0, "local optical flow must use second extended result bit");
}

void renderSyntheticHand(uint8_t* destination, int handX) {
    int width = gState.outputWidth;
    int height = gState.outputHeight;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            destination[y * width + x] = static_cast<uint8_t>(35 + ((x * 3 + y * 5) & 7));
        }
    }
    for (int y = 10; y < 26; ++y) {
        for (int x = handX; x < handX + 16; ++x) {
            destination[y * width + x] =
                static_cast<uint8_t>(100 + (((x - handX) * 13 + (y - 10) * 9) & 63));
        }
    }
}

void createSyntheticMotionMask() {
    for (size_t i = 0; i < gState.pixelCount; ++i) {
        gState.mask[i] = absoluteDifference(gState.grayscale[i], gState.previous[i]) > 8 ? 1 : 0;
    }
}

void testLocalOpticalFlow() {
    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 640, 480,
                                                                             80, 60) == JNI_TRUE,
               "local flow test initialization must succeed");
    renderSyntheticHand(gState.previous, 12);
    renderSyntheticHand(gState.grayscale, 24);
    createSyntheticMotionMask();
    LocalFlowEstimate right = estimateLocalFlow(8, 72, 3, 39);
    expectTrue(right.valid, "local flow must detect a translated hand");
    expectTrue(right.displacementX >= 8, "rightward hand flow must retain positive displacement");
    expectTrue(right.supportingBlocks >= kConfig.flowMinimumSupportingBlocks,
               "local flow must retain spatial support count");

    renderSyntheticHand(gState.previous, 24);
    renderSyntheticHand(gState.grayscale, 10);
    createSyntheticMotionMask();
    LocalFlowEstimate left = estimateLocalFlow(8, 72, 3, 39);
    expectTrue(left.valid, "reverse local flow must be detected");
    expectTrue(left.displacementX <= -8, "leftward hand flow must retain negative displacement");

    resetWaveCompletely();
    clearFlowTrajectory();
    int flags = 0;
    LocalFlowEstimate flow = {true, 12, 0, 5, 800};
    expectTrue(updateFlowTrajectory(flow, 100, 64, &flags) == GESTURE_RESULT_MOTION,
               "first flow vector must begin a trajectory");
    flags = 0;
    expectTrue(updateFlowTrajectory(flow, 190, 64, &flags) == GESTURE_RESULT_MOTION,
               "second same-direction vector must extend trajectory");
    flow.displacementX = -14;
    flags = 0;
    expectTrue(updateFlowTrajectory(flow, 280, 64, &flags) == GESTURE_RESULT_WAVE,
               "strong optical-flow reversal must confirm a quick wave");

    resetWaveCompletely();
    clearFlowTrajectory();
    flow.displacementX = 12;
    flags = 0;
    updateFlowTrajectory(flow, 100, 64, &flags);
    flags = GESTURE_FLAG_CAMERA_TRANSLATION;
    flow.displacementX = -14;
    expectTrue(updateFlowTrajectory(flow, 200, 64, &flags) == GESTURE_RESULT_NONE,
               "camera translation must cancel local-flow trajectory");
    expectTrue(gFlow.state == FLOW_IDLE, "cancelled local-flow trajectory must return to idle");
    destroyLocked();
}

void testInitializationAndJpegDecode() {
    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 640, 480,
                                                                             80, 60) == JNI_TRUE,
               "80x60 detector initialization must succeed");
    expectTrue(gState.workspaceBytes == 80U * 60U * 3U,
               "detector must retain only three byte planes");
    expectTrue(gState.pixelCount == 4800U, "detector pixel count must be 4800");
    destroyLocked();
    destroyLocked();

    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 4096, 4096,
                                                                             256, 256) == JNI_FALSE,
               "16-bit queue limit must reject 65536 pixels");
    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 32, 24, 4,
                                                                             3) == JNI_TRUE,
               "fixture-sized detector initialization must succeed");
    expectTrue(decodeToGrayscale(kSmokeJpeg, kSmokeJpegLength) == 0,
               "valid baseline JPEG must decode");
    uint32_t fixtureChecksum = 2166136261U;
    for (size_t i = 0; i < gState.pixelCount; ++i) {
        fixtureChecksum = (fixtureChecksum ^ gState.grayscale[i]) * 16777619U;
    }
    expectTrue(fixtureChecksum == 1382946109U,
               "luma-only fast-Huffman output must remain deterministic");
    expectTrue(gState.currentMeanLuminance > 20 && gState.currentMeanLuminance < 235,
               "decoded fixture luminance must be plausible");
    int64_t first = analyzeMotion(0);
    expectTrue((first & 0x3) == GESTURE_RESULT_NONE,
               "first decoded frame must seed background only");
    expectTrue(decodeToGrayscale(kSmokeJpeg, kSmokeJpegLength) == 0,
               "same JPEG must decode repeatedly");
    int64_t second = analyzeMotion(100);
    expectTrue((second & 0x3) == GESTURE_RESULT_NONE, "identical frame must not create motion");
    expectTrue(((second >> kMotionPermilleShift) & 0x3ff) == 0,
               "identical frame must have zero ROI motion");
    expectTrue(decodeToGrayscale(kSmokeJpeg, 80) < 0, "truncated JPEG must fail safely");
    destroyLocked();

    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 31, 24, 4,
                                                                             3) == JNI_TRUE,
               "mismatch test initialization must succeed");
    expectTrue(decodeToGrayscale(kSmokeJpeg, kSmokeJpegLength) == GESTURE_ERROR_JPEG_DIMENSIONS,
               "unexpected JPEG dimensions must be rejected");
    destroyLocked();
}

void testBackgroundTranslation() {
    expectTrue(Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(NULL, NULL, 640, 480,
                                                                             80, 60) == JNI_TRUE,
               "translation test initialization must succeed");

    int width = gState.outputWidth;
    int height = gState.outputHeight;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            gState.previous[y * width + x] =
                static_cast<uint8_t>((x * 17 + y * 31 + x * y * 3) & 0xff);
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sourceX = x + 3;
            gState.grayscale[y * width + x] =
                sourceX < width ? gState.previous[y * width + sourceX] : 0;
        }
    }

    TranslationEstimate estimate = estimateCameraTranslation(8, 72, 3, 39);
    expectTrue(estimate.detected, "three-pixel camera translation must be detected");
    expectTrue(estimate.shiftX == 3 && estimate.shiftY == 0,
               "camera translation direction and magnitude must be retained");
    expectTrue(estimate.improvementPermille >= 900,
               "exact synthetic translation must strongly improve background SAD");
    destroyLocked();
}

} // namespace

int main() {
    testFastWaveBothDirections();
    testTrajectoryRejections();
    testRefractoryPeriod();
    testPackedMetrics();
    testLocalOpticalFlow();
    testInitializationAndJpegDecode();
    testBackgroundTranslation();

    if (gFailures != 0) {
        fprintf(stderr, "Native smoke test: %d/%d assertions failed\n", gFailures, gAssertions);
        return 1;
    }
    printf("Native smoke test: %d assertions passed\n", gAssertions);
    return 0;
}
