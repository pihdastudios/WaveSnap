#include "gesture_detector.h"

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "third_party/picojpeg/picojpeg.h"
}

#define LOG_TAG "WaveSnapGesture"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

const int kMaximumDimension = 4096;
const size_t kMaximumQueuePixels = 65535;
const unsigned char kLumaOnlyReducedJpegDecode = 2;

const int kResultShift = 0;
const int kMeanLumaShift = 4;
const int kMotionPermilleShift = 12;
const int kGlobalPermilleShift = 22;
const int kCentroidXShift = 32;
const int kCentroidYShift = 40;
const int kAreaPermilleShift = 48;
const int kFlagsShift = 58;

struct DetectorConfig {
    int roiLeftPercent;
    int roiRightPercent;
    int roiTopPercent;
    int roiBottomPercent;
    int minimumDifference;
    int adaptiveDifferenceMultiplier;
    int maximumDifferenceThreshold;
    int globalMotionPermille;
    int roiMotionPermille;
    int exposureJump;
    int translationSearchX;
    int translationSearchY;
    int translationSampleStep;
    int translationMinimumAverageDifference;
    int translationMinimumImprovementPermille;
    int flowBlockSize;
    int flowBlockStep;
    int flowSearchX;
    int flowSearchY;
    int flowMinimumMaskPixels;
    int flowMinimumZeroAverage;
    int flowMaximumBestAverage;
    int flowMinimumImprovementPermille;
    int flowMinimumSupportingBlocks;
    int flowMaximumSupportingBlocks;
    int flowMinimumSignConsensusPermille;
    int flowMinimumVectorPixels;
    int flowMinimumFirstTravelPercent;
    int flowMinimumReverseTravelPercent;
    int flowMinimumVectorFrames;
    int flowMaximumGapMs;
    int flowMinimumDurationMs;
    int flowMaximumDurationMs;
    int minimumComponentArea;
    int maximumComponentAreaPermille;
    int maximumComponentWidthPermille;
    int maximumComponentHeightPermille;
    int minimumTrackedAreaPermille;
    int minimumTrackedMotionPermille;
    int directionStartPercent;
    int minimumFirstTravelPercent;
    int reversalHysteresisPercent;
    int minimumReverseTravelPercent;
    int maximumVerticalDriftPercent;
    int maximumHorizontalStepPercent;
    int minimumCandidateFrames;
    int maximumCandidateGapMs;
    int minimumWaveDurationMs;
    int maximumWaveDurationMs;
    int refractoryDurationMs;
};

const DetectorConfig kConfig = {10, 90,  5,  65, 10,  2,  40,   350, 450, 20,   6,   0,  2,
                                6,  180, 4,  4,  16,  2,  4,    8,   30,  300,  2,   64, 550,
                                2,  16,  12, 2,  300, 80, 1500, 6,   300, 850,  800, 8,  20,
                                6,  24,  8,  20, 42,  45, 4,    350, 250, 2200, 1800};

enum WaveTrackingState {
    WAVE_IDLE = 0,
    WAVE_TRACKING_FIRST_DIRECTION = 1,
    WAVE_FIRST_EXTREME_REACHED = 2,
    WAVE_TRACKING_REVERSE = 3
};

struct WaveTracker {
    int state;
    int firstDirection;
    int startX;
    int startY;
    int lastX;
    int lastY;
    int firstExtremeX;
    int reverseExtremeX;
    int minimumY;
    int maximumY;
    int candidateFrames;
    int firstTravel;
    int reverseTravel;
    int64_t startTimestampMs;
    int64_t lastCandidateTimestampMs;
    int64_t lastConfirmedTimestampMs;
};

struct TranslationEstimate {
    bool detected;
    int shiftX;
    int shiftY;
    int zeroAverageDifference;
    int bestAverageDifference;
    int improvementPermille;
};

struct LocalFlowEstimate {
    bool valid;
    int displacementX;
    int displacementY;
    int supportingBlocks;
    int confidencePermille;
};

enum FlowTrackingState {
    FLOW_IDLE = 0,
    FLOW_TRACKING_FIRST_DIRECTION = 1,
    FLOW_TRACKING_REVERSE = 2
};

struct FlowTracker {
    int state;
    int firstDirection;
    int firstTravel;
    int reverseTravel;
    int vectorFrames;
    int64_t startTimestampMs;
    int64_t lastTimestampMs;
};

struct DetectorState {
    int sourceWidth;
    int sourceHeight;
    int outputWidth;
    int outputHeight;
    size_t pixelCount;
    uint8_t* workspace;
    size_t workspaceBytes;
    uint8_t* grayscale;
    uint8_t* previous;
    uint8_t* mask;
    uint16_t* componentQueue;
    int currentMeanLuminance;
    int previousMeanLuminance;
    bool hasPrevious;
    bool initialized;
};

struct JpegInput {
    const uint8_t* data;
    size_t length;
    size_t offset;
};

DetectorState gState = {0, 0, 0, 0, 0, NULL, 0, NULL, NULL, NULL, NULL, 0, 0, false, false};
WaveTracker gWave = {WAVE_IDLE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
FlowTracker gFlow = {FLOW_IDLE, 0, 0, 0, 0, 0, 0};
pthread_mutex_t gDetectorMutex = PTHREAD_MUTEX_INITIALIZER;

unsigned char readJpegBytes(unsigned char* destination, unsigned char requested,
                            unsigned char* actual, void* callbackData) {
    if (destination == NULL || actual == NULL || callbackData == NULL) {
        return PJPG_STREAM_READ_ERROR;
    }
    JpegInput* input = static_cast<JpegInput*>(callbackData);
    size_t remaining = input->offset < input->length ? input->length - input->offset : 0;
    size_t count = remaining < requested ? remaining : requested;
    if (count > 0) {
        memcpy(destination, input->data + input->offset, count);
        input->offset += count;
    }
    *actual = static_cast<unsigned char>(count);
    return 0;
}

int ceilScale(int sourceCoordinate, int outputSize, int sourceSize) {
    return (sourceCoordinate * outputSize + sourceSize - 1) / sourceSize;
}

int mcuBufferOffset(int localX, int localY) {
    return (localY / 8) * 128 + (localX / 8) * 64;
}

int clampPermille(int numerator, int denominator) {
    if (denominator <= 0 || numerator <= 0) {
        return 0;
    }
    int value = numerator * 1000 / denominator;
    return value > 1000 ? 1000 : value;
}

int absoluteDifference(int first, int second) {
    int difference = first - second;
    return difference < 0 ? -difference : difference;
}

int percentageOf(int value, int percent) {
    return value * percent / 100;
}

void clearWaveTrajectory() {
    gWave.state = WAVE_IDLE;
    gWave.firstDirection = 0;
    gWave.startX = 0;
    gWave.startY = 0;
    gWave.lastX = 0;
    gWave.lastY = 0;
    gWave.firstExtremeX = 0;
    gWave.reverseExtremeX = 0;
    gWave.minimumY = 0;
    gWave.maximumY = 0;
    gWave.candidateFrames = 0;
    gWave.firstTravel = 0;
    gWave.reverseTravel = 0;
    gWave.startTimestampMs = 0;
    gWave.lastCandidateTimestampMs = 0;
}

void clearFlowTrajectory() {
    gFlow.state = FLOW_IDLE;
    gFlow.firstDirection = 0;
    gFlow.firstTravel = 0;
    gFlow.reverseTravel = 0;
    gFlow.vectorFrames = 0;
    gFlow.startTimestampMs = 0;
    gFlow.lastTimestampMs = 0;
}

void beginFlowTrajectory(int displacementX, int64_t timestampMs) {
    clearFlowTrajectory();
    gFlow.state = FLOW_TRACKING_FIRST_DIRECTION;
    gFlow.firstDirection = displacementX > 0 ? 1 : -1;
    gFlow.firstTravel = absoluteDifference(displacementX, 0);
    gFlow.vectorFrames = 1;
    gFlow.startTimestampMs = timestampMs;
    gFlow.lastTimestampMs = timestampMs;
}

void beginWaveTrajectory(int x, int y, int64_t timestampMs) {
    clearWaveTrajectory();
    gWave.state = WAVE_TRACKING_FIRST_DIRECTION;
    gWave.startX = x;
    gWave.startY = y;
    gWave.lastX = x;
    gWave.lastY = y;
    gWave.firstExtremeX = x;
    gWave.reverseExtremeX = x;
    gWave.minimumY = y;
    gWave.maximumY = y;
    gWave.candidateFrames = 1;
    gWave.startTimestampMs = timestampMs;
    gWave.lastCandidateTimestampMs = timestampMs;
}

bool trajectoryWasMeaningful() {
    return gWave.firstTravel >= 12 || gWave.candidateFrames >= 3;
}

int updateWaveTrajectory(int x, int y, int areaPermille, int motionPermille, int64_t timestampMs,
                         int roiWidth, int roiHeight, int* flags) {
    bool validCandidate = x >= 0 && y >= 0 && areaPermille >= kConfig.minimumTrackedAreaPermille &&
                          motionPermille >= kConfig.minimumTrackedMotionPermille;

    if ((*flags & (GESTURE_FLAG_GLOBAL_MOTION | GESTURE_FLAG_EXPOSURE_CHANGE |
                   GESTURE_FLAG_COMPONENT_TOO_LARGE | GESTURE_FLAG_CAMERA_TRANSLATION)) != 0) {
        if (trajectoryWasMeaningful()) {
            *flags |= GESTURE_FLAG_TRAJECTORY_REJECTED;
            LOGI("wave rejected reason=disruptive_motion state=%d first=%d reverse=%d frames=%d",
                 gWave.state, gWave.firstTravel, gWave.reverseTravel, gWave.candidateFrames);
        }
        clearWaveTrajectory();
        return GESTURE_RESULT_NONE;
    }

    if (gWave.lastConfirmedTimestampMs > 0 && timestampMs >= gWave.lastConfirmedTimestampMs &&
        timestampMs - gWave.lastConfirmedTimestampMs < kConfig.refractoryDurationMs) {
        clearWaveTrajectory();
        return validCandidate ? GESTURE_RESULT_MOTION : GESTURE_RESULT_NONE;
    }

    if (!validCandidate) {
        if (gWave.state != WAVE_IDLE &&
            timestampMs - gWave.lastCandidateTimestampMs > kConfig.maximumCandidateGapMs) {
            if (trajectoryWasMeaningful()) {
                *flags |= GESTURE_FLAG_TRAJECTORY_REJECTED;
                LOGI("wave rejected reason=motion_gap state=%d first=%d reverse=%d frames=%d",
                     gWave.state, gWave.firstTravel, gWave.reverseTravel, gWave.candidateFrames);
            }
            clearWaveTrajectory();
        }
        return GESTURE_RESULT_NONE;
    }

    if (gWave.state == WAVE_IDLE) {
        beginWaveTrajectory(x, y, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    int64_t durationMs = timestampMs - gWave.startTimestampMs;
    if (durationMs < 0 || durationMs > kConfig.maximumWaveDurationMs) {
        if (trajectoryWasMeaningful()) {
            *flags |= GESTURE_FLAG_TRAJECTORY_REJECTED;
            LOGI("wave rejected reason=timeout durationMs=%lld first=%d reverse=%d frames=%d",
                 static_cast<long long>(durationMs), gWave.firstTravel, gWave.reverseTravel,
                 gWave.candidateFrames);
        }
        beginWaveTrajectory(x, y, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    int maximumHorizontalStep = percentageOf(roiWidth, kConfig.maximumHorizontalStepPercent);
    int maximumVerticalDrift = percentageOf(roiHeight, kConfig.maximumVerticalDriftPercent);
    if (absoluteDifference(x, gWave.lastX) > maximumHorizontalStep) {
        if (trajectoryWasMeaningful()) {
            *flags |= GESTURE_FLAG_TRAJECTORY_REJECTED;
            LOGI("wave rejected reason=centroid_jump step=%d first=%d reverse=%d frames=%d",
                 absoluteDifference(x, gWave.lastX), gWave.firstTravel, gWave.reverseTravel,
                 gWave.candidateFrames);
        }
        beginWaveTrajectory(x, y, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    if (y < gWave.minimumY)
        gWave.minimumY = y;
    if (y > gWave.maximumY)
        gWave.maximumY = y;
    if (gWave.maximumY - gWave.minimumY > maximumVerticalDrift) {
        *flags |= GESTURE_FLAG_TRAJECTORY_REJECTED;
        LOGI("wave rejected reason=vertical_drift drift=%d first=%d reverse=%d frames=%d",
             gWave.maximumY - gWave.minimumY, gWave.firstTravel, gWave.reverseTravel,
             gWave.candidateFrames);
        clearWaveTrajectory();
        return GESTURE_RESULT_NONE;
    }

    gWave.lastX = x;
    gWave.lastY = y;
    gWave.lastCandidateTimestampMs = timestampMs;
    ++gWave.candidateFrames;

    int directionStart = percentageOf(roiWidth, kConfig.directionStartPercent);
    int minimumFirstTravel = percentageOf(roiWidth, kConfig.minimumFirstTravelPercent);
    int reversalHysteresis = percentageOf(roiWidth, kConfig.reversalHysteresisPercent);
    int minimumReverseTravel = percentageOf(roiWidth, kConfig.minimumReverseTravelPercent);

    if (gWave.firstDirection == 0) {
        int displacement = x - gWave.startX;
        if (absoluteDifference(x, gWave.startX) >= directionStart) {
            gWave.firstDirection = displacement > 0 ? 1 : -1;
            gWave.firstExtremeX = x;
            gWave.firstTravel = absoluteDifference(x, gWave.startX);
        }
        return GESTURE_RESULT_MOTION;
    }

    bool extendsFirstDirection = (gWave.firstDirection > 0 && x > gWave.firstExtremeX) ||
                                 (gWave.firstDirection < 0 && x < gWave.firstExtremeX);
    if (gWave.state != WAVE_TRACKING_REVERSE && extendsFirstDirection) {
        gWave.firstExtremeX = x;
        gWave.firstTravel = absoluteDifference(gWave.firstExtremeX, gWave.startX);
    }

    if (gWave.state == WAVE_TRACKING_FIRST_DIRECTION && gWave.firstTravel >= minimumFirstTravel) {
        gWave.state = WAVE_FIRST_EXTREME_REACHED;
    }

    int movementBack = gWave.firstDirection > 0 ? gWave.firstExtremeX - x : x - gWave.firstExtremeX;
    if (gWave.state == WAVE_FIRST_EXTREME_REACHED && movementBack >= reversalHysteresis) {
        gWave.state = WAVE_TRACKING_REVERSE;
        gWave.reverseExtremeX = x;
        gWave.reverseTravel = movementBack;
    } else if (gWave.state == WAVE_TRACKING_REVERSE) {
        bool extendsReverseDirection = (gWave.firstDirection > 0 && x < gWave.reverseExtremeX) ||
                                       (gWave.firstDirection < 0 && x > gWave.reverseExtremeX);
        if (extendsReverseDirection) {
            gWave.reverseExtremeX = x;
            gWave.reverseTravel = absoluteDifference(gWave.reverseExtremeX, gWave.firstExtremeX);
        }
    }

    if (gWave.state == WAVE_TRACKING_REVERSE && gWave.reverseTravel >= minimumReverseTravel &&
        gWave.candidateFrames >= kConfig.minimumCandidateFrames &&
        durationMs >= kConfig.minimumWaveDurationMs) {
        LOGI("wave confirmed timestampMs=%lld durationMs=%lld direction=%d first=%d reverse=%d "
             "vertical=%d frames=%d",
             static_cast<long long>(timestampMs), static_cast<long long>(durationMs),
             gWave.firstDirection, gWave.firstTravel, gWave.reverseTravel,
             gWave.maximumY - gWave.minimumY, gWave.candidateFrames);
        gWave.lastConfirmedTimestampMs = timestampMs;
        clearWaveTrajectory();
        return GESTURE_RESULT_WAVE;
    }

    return GESTURE_RESULT_MOTION;
}

int updateFlowTrajectory(const LocalFlowEstimate& flow, int64_t timestampMs, int roiWidth,
                         int* flags) {
    if ((*flags & (GESTURE_FLAG_GLOBAL_MOTION | GESTURE_FLAG_EXPOSURE_CHANGE |
                   GESTURE_FLAG_CAMERA_TRANSLATION)) != 0) {
        clearFlowTrajectory();
        return GESTURE_RESULT_NONE;
    }

    if (gWave.lastConfirmedTimestampMs > 0 && timestampMs >= gWave.lastConfirmedTimestampMs &&
        timestampMs - gWave.lastConfirmedTimestampMs < kConfig.refractoryDurationMs) {
        clearFlowTrajectory();
        return flow.valid ? GESTURE_RESULT_MOTION : GESTURE_RESULT_NONE;
    }

    if (!flow.valid ||
        absoluteDifference(flow.displacementX, 0) < kConfig.flowMinimumVectorPixels) {
        if (gFlow.state != FLOW_IDLE &&
            timestampMs - gFlow.lastTimestampMs > kConfig.flowMaximumGapMs) {
            clearFlowTrajectory();
        }
        return GESTURE_RESULT_NONE;
    }

    if (gFlow.state == FLOW_IDLE) {
        beginFlowTrajectory(flow.displacementX, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    int64_t durationMs = timestampMs - gFlow.startTimestampMs;
    if (durationMs < 0 || durationMs > kConfig.flowMaximumDurationMs ||
        timestampMs - gFlow.lastTimestampMs > kConfig.flowMaximumGapMs) {
        beginFlowTrajectory(flow.displacementX, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    int direction = flow.displacementX > 0 ? 1 : -1;
    int distance = absoluteDifference(flow.displacementX, 0);
    int minimumFirstTravel = percentageOf(roiWidth, kConfig.flowMinimumFirstTravelPercent);
    int minimumReverseTravel = percentageOf(roiWidth, kConfig.flowMinimumReverseTravelPercent);
    ++gFlow.vectorFrames;
    gFlow.lastTimestampMs = timestampMs;

    if (gFlow.state == FLOW_TRACKING_FIRST_DIRECTION) {
        if (direction == gFlow.firstDirection) {
            gFlow.firstTravel += distance;
        } else if (gFlow.firstTravel >= minimumFirstTravel) {
            gFlow.state = FLOW_TRACKING_REVERSE;
            gFlow.reverseTravel = distance;
        } else {
            beginFlowTrajectory(flow.displacementX, timestampMs);
            return GESTURE_RESULT_MOTION;
        }
    } else if (direction == -gFlow.firstDirection) {
        gFlow.reverseTravel += distance;
    } else {
        beginFlowTrajectory(flow.displacementX, timestampMs);
        return GESTURE_RESULT_MOTION;
    }

    if (gFlow.state == FLOW_TRACKING_REVERSE && gFlow.firstTravel >= minimumFirstTravel &&
        gFlow.reverseTravel >= minimumReverseTravel &&
        gFlow.vectorFrames >= kConfig.flowMinimumVectorFrames &&
        durationMs >= kConfig.flowMinimumDurationMs) {
        LOGI("flow wave confirmed timestampMs=%lld durationMs=%lld direction=%d first=%d "
             "reverse=%d vectors=%d support=%d confidence=%d",
             static_cast<long long>(timestampMs), static_cast<long long>(durationMs),
             gFlow.firstDirection, gFlow.firstTravel, gFlow.reverseTravel, gFlow.vectorFrames,
             flow.supportingBlocks, flow.confidencePermille);
        gWave.lastConfirmedTimestampMs = timestampMs;
        clearWaveTrajectory();
        clearFlowTrajectory();
        return GESTURE_RESULT_WAVE;
    }
    return GESTURE_RESULT_MOTION;
}

int64_t packMetrics(int result, int meanLuminance, int motionPermille, int globalPermille,
                    int centroidX, int centroidY, int areaPermille, int flags) {
    uint64_t packed = 0;
    int resultAndExtendedFlags = result & 0x3;
    if ((flags & GESTURE_FLAG_CAMERA_TRANSLATION) != 0) {
        resultAndExtendedFlags |= 0x4;
    }
    if ((flags & GESTURE_FLAG_LOCAL_FLOW) != 0) {
        resultAndExtendedFlags |= 0x8;
    }
    packed |= static_cast<uint64_t>(resultAndExtendedFlags) << kResultShift;
    packed |= static_cast<uint64_t>(meanLuminance & 0xff) << kMeanLumaShift;
    packed |= static_cast<uint64_t>(motionPermille & 0x3ff) << kMotionPermilleShift;
    packed |= static_cast<uint64_t>(globalPermille & 0x3ff) << kGlobalPermilleShift;
    packed |= static_cast<uint64_t>(centroidX & 0xff) << kCentroidXShift;
    packed |= static_cast<uint64_t>(centroidY & 0xff) << kCentroidYShift;
    packed |= static_cast<uint64_t>(areaPermille & 0x3ff) << kAreaPermilleShift;
    packed |= static_cast<uint64_t>(flags & 0x1f) << kFlagsShift;
    return static_cast<int64_t>(packed);
}

bool outsideExpandedRoi(int x, int y, int roiLeft, int roiRight, int roiTop, int roiBottom,
                        int marginX, int marginY) {
    return x < roiLeft - marginX || x >= roiRight + marginX || y < roiTop - marginY ||
           y >= roiBottom + marginY;
}

TranslationEstimate estimateCameraTranslation(int roiLeft, int roiRight, int roiTop,
                                              int roiBottom) {
    TranslationEstimate estimate = {false, 0, 0, 0, 0, 0};
    int width = gState.outputWidth;
    int height = gState.outputHeight;
    int searchX = kConfig.translationSearchX;
    int searchY = kConfig.translationSearchY;
    int sampleStep = kConfig.translationSampleStep;
    int64_t zeroSum = 0;
    int zeroSamples = 0;

    for (int y = searchY; y < height - searchY; y += sampleStep) {
        for (int x = searchX; x < width - searchX; x += sampleStep) {
            if (!outsideExpandedRoi(x, y, roiLeft, roiRight, roiTop, roiBottom, searchX, searchY)) {
                continue;
            }
            int index = y * width + x;
            zeroSum += absoluteDifference(gState.grayscale[index], gState.previous[index]);
            ++zeroSamples;
        }
    }
    if (zeroSamples < 200) {
        return estimate;
    }
    estimate.zeroAverageDifference = static_cast<int>(zeroSum / zeroSamples);
    if (estimate.zeroAverageDifference < kConfig.translationMinimumAverageDifference) {
        return estimate;
    }

    int bestAverage = estimate.zeroAverageDifference;
    int bestShiftX = 0;
    int bestShiftY = 0;
    for (int shiftY = -searchY; shiftY <= searchY; ++shiftY) {
        for (int shiftX = -searchX; shiftX <= searchX; ++shiftX) {
            if (shiftX == 0 && shiftY == 0) {
                continue;
            }
            int64_t differenceSum = 0;
            int samples = 0;
            for (int y = searchY; y < height - searchY; y += sampleStep) {
                for (int x = searchX; x < width - searchX; x += sampleStep) {
                    int previousX = x + shiftX;
                    int previousY = y + shiftY;
                    if (!outsideExpandedRoi(x, y, roiLeft, roiRight, roiTop, roiBottom, searchX,
                                            searchY)) {
                        continue;
                    }
                    int currentIndex = y * width + x;
                    int previousIndex = previousY * width + previousX;
                    differenceSum += absoluteDifference(gState.grayscale[currentIndex],
                                                        gState.previous[previousIndex]);
                    ++samples;
                }
            }
            if (samples >= 200) {
                int average = static_cast<int>(differenceSum / samples);
                if (average < bestAverage) {
                    bestAverage = average;
                    bestShiftX = shiftX;
                    bestShiftY = shiftY;
                }
            }
        }
    }

    estimate.bestAverageDifference = bestAverage;
    estimate.shiftX = bestShiftX;
    estimate.shiftY = bestShiftY;
    if (bestShiftX != 0 || bestShiftY != 0) {
        estimate.improvementPermille =
            (estimate.zeroAverageDifference - bestAverage) * 1000 / estimate.zeroAverageDifference;
        estimate.detected =
            estimate.improvementPermille >= kConfig.translationMinimumImprovementPermille;
    }
    return estimate;
}

int blockAbsoluteDifference(int currentX, int currentY, int previousX, int previousY,
                            int blockSize) {
    int difference = 0;
    int width = gState.outputWidth;
    for (int y = 0; y < blockSize; ++y) {
        int currentIndex = (currentY + y) * width + currentX;
        int previousIndex = (previousY + y) * width + previousX;
        for (int x = 0; x < blockSize; ++x) {
            difference += absoluteDifference(gState.grayscale[currentIndex + x],
                                             gState.previous[previousIndex + x]);
        }
    }
    return difference;
}

LocalFlowEstimate estimateLocalFlow(int roiLeft, int roiRight, int roiTop, int roiBottom) {
    const int kMaximumSearchX = 24;
    LocalFlowEstimate estimate = {false, 0, 0, 0, 0};
    if (kConfig.flowSearchX > kMaximumSearchX) {
        return estimate;
    }

    int weights[kMaximumSearchX * 2 + 1];
    int supports[kMaximumSearchX * 2 + 1];
    int verticalWeights[kMaximumSearchX * 2 + 1];
    memset(weights, 0, sizeof(weights));
    memset(supports, 0, sizeof(supports));
    memset(verticalWeights, 0, sizeof(verticalWeights));

    int width = gState.outputWidth;
    int blockSize = kConfig.flowBlockSize;
    int blockPixels = blockSize * blockSize;
    int totalWeight = 0;
    int totalSupport = 0;
    for (int currentY = roiTop; currentY + blockSize <= roiBottom;
         currentY += kConfig.flowBlockStep) {
        for (int currentX = roiLeft; currentX + blockSize <= roiRight;
             currentX += kConfig.flowBlockStep) {
            int maskPixels = 0;
            for (int y = 0; y < blockSize; ++y) {
                int index = (currentY + y) * width + currentX;
                for (int x = 0; x < blockSize; ++x) {
                    maskPixels += gState.mask[index + x] != 0;
                }
            }
            if (maskPixels < kConfig.flowMinimumMaskPixels) {
                continue;
            }

            int zeroDifference =
                blockAbsoluteDifference(currentX, currentY, currentX, currentY, blockSize);
            if (zeroDifference / blockPixels < kConfig.flowMinimumZeroAverage) {
                continue;
            }

            int bestDifference = zeroDifference;
            int bestDx = 0;
            int bestDy = 0;
            for (int dy = -kConfig.flowSearchY; dy <= kConfig.flowSearchY; ++dy) {
                int previousY = currentY - dy;
                if (previousY < roiTop || previousY + blockSize > roiBottom) {
                    continue;
                }
                for (int dx = -kConfig.flowSearchX; dx <= kConfig.flowSearchX; ++dx) {
                    if (absoluteDifference(dx, 0) < kConfig.flowMinimumVectorPixels) {
                        continue;
                    }
                    int previousX = currentX - dx;
                    if (previousX < roiLeft || previousX + blockSize > roiRight) {
                        continue;
                    }
                    int difference = blockAbsoluteDifference(currentX, currentY, previousX,
                                                             previousY, blockSize);
                    if (difference < bestDifference) {
                        bestDifference = difference;
                        bestDx = dx;
                        bestDy = dy;
                    }
                }
            }

            if (bestDx == 0 || bestDifference / blockPixels > kConfig.flowMaximumBestAverage) {
                continue;
            }
            int improvementPermille = (zeroDifference - bestDifference) * 1000 / zeroDifference;
            if (improvementPermille < kConfig.flowMinimumImprovementPermille) {
                continue;
            }
            int slot = bestDx + kMaximumSearchX;
            weights[slot] += improvementPermille;
            supports[slot] += 1;
            verticalWeights[slot] += bestDy * improvementPermille;
            totalWeight += improvementPermille;
            ++totalSupport;
        }
    }

    estimate.supportingBlocks = totalSupport;
    if (totalSupport < kConfig.flowMinimumSupportingBlocks ||
        totalSupport > kConfig.flowMaximumSupportingBlocks || totalWeight <= 0) {
        return estimate;
    }

    int positiveWeight = 0;
    int negativeWeight = 0;
    int positiveSupport = 0;
    int negativeSupport = 0;
    int positiveDistance = 0;
    int negativeDistance = 0;
    int positiveVertical = 0;
    int negativeVertical = 0;
    for (int dx = -kConfig.flowSearchX; dx <= kConfig.flowSearchX; ++dx) {
        int slot = dx + kMaximumSearchX;
        if (dx > 0) {
            positiveWeight += weights[slot];
            positiveSupport += supports[slot];
            positiveDistance += dx * weights[slot];
            positiveVertical += verticalWeights[slot];
        } else if (dx < 0) {
            negativeWeight += weights[slot];
            negativeSupport += supports[slot];
            negativeDistance += dx * weights[slot];
            negativeVertical += verticalWeights[slot];
        }
    }

    int winningWeight = positiveWeight >= negativeWeight ? positiveWeight : negativeWeight;
    estimate.confidencePermille = winningWeight <= 0 ? 0 : winningWeight * 1000 / totalWeight;
    if (winningWeight <= 0 ||
        estimate.confidencePermille < kConfig.flowMinimumSignConsensusPermille) {
        return estimate;
    }
    if (positiveWeight >= negativeWeight) {
        estimate.displacementX = positiveDistance / positiveWeight;
        estimate.displacementY = positiveVertical / positiveWeight;
        estimate.supportingBlocks = positiveSupport;
    } else {
        estimate.displacementX = negativeDistance / negativeWeight;
        estimate.displacementY = negativeVertical / negativeWeight;
        estimate.supportingBlocks = negativeSupport;
    }
    estimate.valid =
        estimate.supportingBlocks >= kConfig.flowMinimumSupportingBlocks &&
        absoluteDifference(estimate.displacementX, 0) >= kConfig.flowMinimumVectorPixels;
    return estimate;
}

int decodeToGrayscale(const uint8_t* jpegData, size_t jpegLength) {
    JpegInput input;
    input.data = jpegData;
    input.length = jpegLength;
    input.offset = 0;

    pjpeg_image_info_t imageInfo;
    unsigned char status =
        pjpeg_decode_init(&imageInfo, readJpegBytes, &input, kLumaOnlyReducedJpegDecode);
    if (status != 0) {
        LOGE("pjpeg_decode_init failed status=%u", static_cast<unsigned int>(status));
        return GESTURE_ERROR_JPEG_DECODE;
    }
    if (imageInfo.m_width != gState.sourceWidth || imageInfo.m_height != gState.sourceHeight) {
        LOGE("unexpected JPEG dimensions actual=%dx%d expected=%dx%d", imageInfo.m_width,
             imageInfo.m_height, gState.sourceWidth, gState.sourceHeight);
        return GESTURE_ERROR_JPEG_DIMENSIONS;
    }

    memset(gState.grayscale, 0, gState.pixelCount);
    uint64_t luminanceSum = 0;
    int writtenPixels = 0;

    for (int mcuYIndex = 0; mcuYIndex < imageInfo.m_MCUSPerCol; ++mcuYIndex) {
        for (int mcuXIndex = 0; mcuXIndex < imageInfo.m_MCUSPerRow; ++mcuXIndex) {
            status = pjpeg_decode_mcu();
            if (status != 0) {
                LOGE("pjpeg_decode_mcu failed status=%u mcu=%d,%d",
                     static_cast<unsigned int>(status), mcuXIndex, mcuYIndex);
                return GESTURE_ERROR_JPEG_DECODE;
            }

            int sourceLeft = mcuXIndex * imageInfo.m_MCUWidth;
            int sourceTop = mcuYIndex * imageInfo.m_MCUHeight;
            int sourceRight = sourceLeft + imageInfo.m_MCUWidth;
            int sourceBottom = sourceTop + imageInfo.m_MCUHeight;
            if (sourceRight > imageInfo.m_width) {
                sourceRight = imageInfo.m_width;
            }
            if (sourceBottom > imageInfo.m_height) {
                sourceBottom = imageInfo.m_height;
            }

            int outputLeft = ceilScale(sourceLeft, gState.outputWidth, imageInfo.m_width);
            int outputTop = ceilScale(sourceTop, gState.outputHeight, imageInfo.m_height);
            int outputRight = ceilScale(sourceRight, gState.outputWidth, imageInfo.m_width);
            int outputBottom = ceilScale(sourceBottom, gState.outputHeight, imageInfo.m_height);

            for (int outputY = outputTop; outputY < outputBottom; ++outputY) {
                int sourceY = outputY * imageInfo.m_height / gState.outputHeight;
                int localY = sourceY - sourceTop;
                for (int outputX = outputLeft; outputX < outputRight; ++outputX) {
                    int sourceX = outputX * imageInfo.m_width / gState.outputWidth;
                    int localX = sourceX - sourceLeft;
                    int offset = mcuBufferOffset(localX, localY);
                    int luminance = imageInfo.m_pMCUBufR[offset];
                    gState.grayscale[outputY * gState.outputWidth + outputX] =
                        static_cast<uint8_t>(luminance);
                    luminanceSum += static_cast<uint64_t>(luminance);
                    ++writtenPixels;
                }
            }
        }
    }

    int expectedPixels = gState.outputWidth * gState.outputHeight;
    if (writtenPixels != expectedPixels) {
        LOGE("grayscale coverage mismatch actual=%d expected=%d", writtenPixels, expectedPixels);
        return GESTURE_ERROR_JPEG_DECODE;
    }
    gState.currentMeanLuminance =
        expectedPixels == 0
            ? 0
            : static_cast<int>(luminanceSum / static_cast<uint64_t>(expectedPixels));
    return 0;
}

int64_t analyzeMotion(int64_t timestampMs) {
    if (!gState.hasPrevious) {
        memcpy(gState.previous, gState.grayscale, gState.pixelCount);
        gState.previousMeanLuminance = gState.currentMeanLuminance;
        gState.hasPrevious = true;
        return packMetrics(GESTURE_RESULT_NONE, gState.currentMeanLuminance, 0, 0, 255, 255, 0, 0);
    }

    int width = gState.outputWidth;
    int height = gState.outputHeight;
    int roiLeft = width * kConfig.roiLeftPercent / 100;
    int roiRight = width * kConfig.roiRightPercent / 100;
    int roiTop = height * kConfig.roiTopPercent / 100;
    int roiBottom = height * kConfig.roiBottomPercent / 100;
    int roiWidth = roiRight - roiLeft;
    int roiHeight = roiBottom - roiTop;
    int roiPixels = roiWidth * roiHeight;

    uint64_t differenceSum = 0;
    for (size_t i = 0; i < gState.pixelCount; ++i) {
        differenceSum +=
            static_cast<uint64_t>(absoluteDifference(gState.grayscale[i], gState.previous[i]));
    }
    int averageDifference = static_cast<int>(differenceSum / gState.pixelCount);
    int differenceThreshold =
        kConfig.minimumDifference + averageDifference * kConfig.adaptiveDifferenceMultiplier;
    if (differenceThreshold > kConfig.maximumDifferenceThreshold) {
        differenceThreshold = kConfig.maximumDifferenceThreshold;
    }

    memset(gState.mask, 0, gState.pixelCount);
    int globalChanged = 0;
    int roiChanged = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int index = y * width + x;
            int difference = absoluteDifference(gState.grayscale[index], gState.previous[index]);
            if (difference > differenceThreshold) {
                ++globalChanged;
                if (x >= roiLeft && x < roiRight && y >= roiTop && y < roiBottom) {
                    gState.mask[index] = 1;
                    ++roiChanged;
                }
            }
        }
    }

    int globalPermille = clampPermille(globalChanged, static_cast<int>(gState.pixelCount));
    int motionPermille = clampPermille(roiChanged, roiPixels);
    int flags = 0;
    if (globalPermille >= kConfig.globalMotionPermille ||
        motionPermille >= kConfig.roiMotionPermille) {
        flags |= GESTURE_FLAG_GLOBAL_MOTION;
    }
    if (absoluteDifference(gState.currentMeanLuminance, gState.previousMeanLuminance) >=
        kConfig.exposureJump) {
        flags |= GESTURE_FLAG_EXPOSURE_CHANGE;
    }
    TranslationEstimate translation = {false, 0, 0, 0, 0, 0};
    if (flags == 0) {
        translation = estimateCameraTranslation(roiLeft, roiRight, roiTop, roiBottom);
        if (translation.detected) {
            flags |= GESTURE_FLAG_CAMERA_TRANSLATION;
        }
    }
    LocalFlowEstimate flow = {false, 0, 0, 0, 0};
    if (flags == 0) {
        flow = estimateLocalFlow(roiLeft, roiRight, roiTop, roiBottom);
    }

    /* Preserve the DC image, then reuse its storage for component marks. */
    memcpy(gState.previous, gState.grayscale, gState.pixelCount);
    gState.previousMeanLuminance = gState.currentMeanLuminance;

    int bestArea = 0;
    int64_t bestSumX = 0;
    int64_t bestSumY = 0;
    bool rejectedOversizedComponent = false;

    if (flags == 0) {
        memset(gState.grayscale, 0, gState.pixelCount);
        for (int y = roiTop + 1; y < roiBottom - 1; ++y) {
            for (int x = roiLeft + 1; x < roiRight - 1; ++x) {
                int index = y * width + x;
                if (gState.mask[index] == 0) {
                    continue;
                }
                int neighbours = 0;
                for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        if (offsetX != 0 || offsetY != 0) {
                            neighbours += gState.mask[index + offsetY * width + offsetX] != 0;
                        }
                    }
                }
                if (neighbours >= 2) {
                    gState.grayscale[index] = 1;
                }
            }
        }

        for (int y = roiTop; y < roiBottom; ++y) {
            for (int x = roiLeft; x < roiRight; ++x) {
                int startIndex = y * width + x;
                if (gState.grayscale[startIndex] != 1) {
                    continue;
                }

                int queueHead = 0;
                int queueTail = 0;
                int area = 0;
                int minX = x;
                int maxX = x;
                int minY = y;
                int maxY = y;
                int64_t sumX = 0;
                int64_t sumY = 0;
                gState.componentQueue[queueTail++] = static_cast<uint16_t>(startIndex);
                gState.grayscale[startIndex] = 2;

                while (queueHead < queueTail) {
                    int index = gState.componentQueue[queueHead++];
                    int pointY = index / width;
                    int pointX = index - pointY * width;
                    ++area;
                    sumX += pointX;
                    sumY += pointY;
                    if (pointX < minX)
                        minX = pointX;
                    if (pointX > maxX)
                        maxX = pointX;
                    if (pointY < minY)
                        minY = pointY;
                    if (pointY > maxY)
                        maxY = pointY;

                    const int neighbourIndexes[4] = {index - 1, index + 1, index - width,
                                                     index + width};
                    for (int neighbour = 0; neighbour < 4; ++neighbour) {
                        int neighbourIndex = neighbourIndexes[neighbour];
                        int neighbourY = neighbourIndex / width;
                        int neighbourX = neighbourIndex - neighbourY * width;
                        if (neighbourX >= roiLeft && neighbourX < roiRight &&
                            neighbourY >= roiTop && neighbourY < roiBottom &&
                            gState.grayscale[neighbourIndex] == 1) {
                            gState.grayscale[neighbourIndex] = 2;
                            gState.componentQueue[queueTail++] =
                                static_cast<uint16_t>(neighbourIndex);
                        }
                    }
                }

                int candidateAreaPermille = clampPermille(area, roiPixels);
                int candidateWidthPermille = clampPermille(maxX - minX + 1, roiWidth);
                int candidateHeightPermille = clampPermille(maxY - minY + 1, roiHeight);
                bool plausible = area >= kConfig.minimumComponentArea &&
                                 candidateAreaPermille <= kConfig.maximumComponentAreaPermille &&
                                 candidateWidthPermille <= kConfig.maximumComponentWidthPermille &&
                                 candidateHeightPermille <= kConfig.maximumComponentHeightPermille;
                if (!plausible && area >= kConfig.minimumComponentArea &&
                    (candidateAreaPermille > kConfig.maximumComponentAreaPermille ||
                     candidateWidthPermille > kConfig.maximumComponentWidthPermille ||
                     candidateHeightPermille > kConfig.maximumComponentHeightPermille)) {
                    rejectedOversizedComponent = true;
                }
                if (plausible && area > bestArea) {
                    bestArea = area;
                    bestSumX = sumX;
                    bestSumY = sumY;
                }
            }
        }
    }

    int result = GESTURE_RESULT_NONE;
    int centroidX = 255;
    int centroidY = 255;
    int areaPermille = clampPermille(bestArea, roiPixels);
    if (flags == 0 && bestArea >= kConfig.minimumComponentArea) {
        centroidX = static_cast<int>(bestSumX / bestArea);
        centroidY = static_cast<int>(bestSumY / bestArea);
        result = GESTURE_RESULT_MOTION;
    } else if (flags == 0 && rejectedOversizedComponent) {
        flags |= GESTURE_FLAG_COMPONENT_TOO_LARGE;
    } else if (flags == 0 && roiChanged > 0) {
        flags |= GESTURE_FLAG_NO_PLAUSIBLE_COMPONENT;
    }

    result = updateWaveTrajectory(centroidX == 255 ? -1 : centroidX,
                                  centroidY == 255 ? -1 : centroidY, areaPermille, motionPermille,
                                  timestampMs, roiWidth, roiHeight, &flags);

    if (result == GESTURE_RESULT_WAVE) {
        clearFlowTrajectory();
    } else {
        int flowResult = updateFlowTrajectory(flow, timestampMs, roiWidth, &flags);
        if (flowResult == GESTURE_RESULT_WAVE) {
            result = GESTURE_RESULT_WAVE;
        } else if (result == GESTURE_RESULT_NONE && flowResult == GESTURE_RESULT_MOTION) {
            result = GESTURE_RESULT_MOTION;
        }
    }

    if ((flags & GESTURE_FLAG_CAMERA_TRANSLATION) != 0) {
        centroidX = translation.shiftX + 16;
        centroidY = translation.shiftY + 16;
    } else if (flow.valid) {
        flags |= GESTURE_FLAG_LOCAL_FLOW;
        centroidX = flow.displacementX + 32;
        centroidY = flow.supportingBlocks;
    }

    return packMetrics(result, gState.currentMeanLuminance, motionPermille, globalPermille,
                       centroidX, centroidY, areaPermille, flags);
}

void resetLocked() {
    if (!gState.initialized) {
        return;
    }
    memset(gState.workspace, 0, gState.workspaceBytes);
    memset(gState.componentQueue, 0, gState.pixelCount * sizeof(uint16_t));
    gState.currentMeanLuminance = 0;
    gState.previousMeanLuminance = 0;
    gState.hasPrevious = false;
    clearWaveTrajectory();
    clearFlowTrajectory();
    gWave.lastConfirmedTimestampMs = 0;
}

void destroyLocked() {
    if (gState.workspace != NULL) {
        free(gState.workspace);
    }
    if (gState.componentQueue != NULL) {
        free(gState.componentQueue);
    }
    gState.sourceWidth = 0;
    gState.sourceHeight = 0;
    gState.outputWidth = 0;
    gState.outputHeight = 0;
    gState.pixelCount = 0;
    gState.workspace = NULL;
    gState.workspaceBytes = 0;
    gState.grayscale = NULL;
    gState.previous = NULL;
    gState.mask = NULL;
    gState.componentQueue = NULL;
    gState.currentMeanLuminance = 0;
    gState.previousMeanLuminance = 0;
    gState.hasPrevious = false;
    gState.initialized = false;
    clearWaveTrajectory();
    clearFlowTrajectory();
    gWave.lastConfirmedTimestampMs = 0;
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_io_pihda_wavesnap_NativeGestureDetector_nativeInitialize(
    JNIEnv*, jclass, jint sourceWidth, jint sourceHeight, jint outputWidth, jint outputHeight) {
    pthread_mutex_lock(&gDetectorMutex);
    destroyLocked();

    if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputHeight <= 0 ||
        sourceWidth > kMaximumDimension || sourceHeight > kMaximumDimension ||
        outputWidth > sourceWidth || outputHeight > sourceHeight) {
        pthread_mutex_unlock(&gDetectorMutex);
        return JNI_FALSE;
    }

    size_t pixelCount = static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight);
    if (pixelCount > kMaximumQueuePixels) {
        pthread_mutex_unlock(&gDetectorMutex);
        return JNI_FALSE;
    }
    size_t workspaceBytes = pixelCount * 3;
    gState.workspace = static_cast<uint8_t*>(malloc(workspaceBytes));
    gState.componentQueue = static_cast<uint16_t*>(malloc(pixelCount * sizeof(uint16_t)));
    if (gState.workspace == NULL || gState.componentQueue == NULL) {
        destroyLocked();
        pthread_mutex_unlock(&gDetectorMutex);
        return JNI_FALSE;
    }

    gState.sourceWidth = sourceWidth;
    gState.sourceHeight = sourceHeight;
    gState.outputWidth = outputWidth;
    gState.outputHeight = outputHeight;
    gState.pixelCount = pixelCount;
    gState.workspaceBytes = workspaceBytes;
    gState.grayscale = gState.workspace;
    gState.previous = gState.grayscale + pixelCount;
    gState.mask = gState.previous + pixelCount;
    gState.initialized = true;
    resetLocked();

    size_t totalBytes = workspaceBytes + pixelCount * sizeof(uint16_t);
    LOGI("initialized source=%dx%d output=%dx%d nativeBytes=%u", sourceWidth, sourceHeight,
         outputWidth, outputHeight, static_cast<unsigned int>(totalBytes));
    pthread_mutex_unlock(&gDetectorMutex);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL Java_io_pihda_wavesnap_NativeGestureDetector_nativeProcess(
    JNIEnv* env, jclass, jobject jpegBuffer, jint jpegLength, jlong timestampMs) {
    if (jpegBuffer == NULL || jpegLength <= 0) {
        return GESTURE_ERROR_INVALID_ARGUMENT;
    }
    void* address = env->GetDirectBufferAddress(jpegBuffer);
    jlong capacity = env->GetDirectBufferCapacity(jpegBuffer);
    if (address == NULL || capacity < 0 || static_cast<jlong>(jpegLength) > capacity) {
        return GESTURE_ERROR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&gDetectorMutex);
    if (!gState.initialized || gState.workspace == NULL) {
        pthread_mutex_unlock(&gDetectorMutex);
        return GESTURE_ERROR_NOT_INITIALIZED;
    }
    int decodeResult =
        decodeToGrayscale(static_cast<const uint8_t*>(address), static_cast<size_t>(jpegLength));
    if (decodeResult < 0) {
        pthread_mutex_unlock(&gDetectorMutex);
        return decodeResult;
    }
    int64_t result = analyzeMotion(static_cast<int64_t>(timestampMs));
    pthread_mutex_unlock(&gDetectorMutex);
    return static_cast<jlong>(result);
}

extern "C" JNIEXPORT void JNICALL Java_io_pihda_wavesnap_NativeGestureDetector_nativeReset(JNIEnv*,
                                                                                           jclass) {
    pthread_mutex_lock(&gDetectorMutex);
    resetLocked();
    pthread_mutex_unlock(&gDetectorMutex);
}

extern "C" JNIEXPORT void JNICALL
Java_io_pihda_wavesnap_NativeGestureDetector_nativeDestroy(JNIEnv*, jclass) {
    pthread_mutex_lock(&gDetectorMutex);
    destroyLocked();
    pthread_mutex_unlock(&gDetectorMutex);
    LOGI("destroyed");
}
