#!/usr/bin/env bash

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"

host_only=false
clean=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-only) host_only=true ;;
        --clean) clean=true ;;
        --help|-h)
            echo "Usage: $0 [--host-only] [--clean]"
            echo "  default     host checks, native ASan suite, build, APK and ARM ABI checks"
            echo "  --host-only skip the Gradle build and APK checks"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

require_file "$JDK8_HOME/include/jni.h"
require_file "$PROJECT_DIR/tests/native/gesture_detector_smoke.cpp"
require_file "$PROJECT_DIR/tests/native/jpeg_fixture.h"

temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT

pass_count=0
pass() {
    pass_count=$((pass_count + 1))
    echo "PASS: $1"
}

require_literal() {
    local file="$1"
    local literal="$2"
    local description="$3"
    if ! grep -Fq "$literal" "$file"; then
        echo "FAIL: $description" >&2
        echo "Missing '$literal' in $file" >&2
        exit 1
    fi
    pass "$description"
}

echo "Running WaveSnap smoke tests"

while IFS= read -r script; do
    bash -n "$script"
done < <(find "$PROJECT_DIR/scripts" -maxdepth 1 -type f -name '*.sh' | sort)
pass "all shell scripts parse"

require_literal "$PROJECT_DIR/build.gradle" \
    "classpath 'com.android.tools.build:gradle:2.2.3'" "legacy Android Gradle plugin pinned"
require_literal "$PROJECT_DIR/app/build.gradle" \
    "compileSdkVersion 10" "compile SDK remains API 10"
require_literal "$PROJECT_DIR/app/build.gradle" \
    "buildToolsVersion \"25.0.2\"" "Build Tools remain 25.0.2"
require_literal "$PROJECT_DIR/app/build.gradle" \
    "sourceCompatibility JavaVersion.VERSION_1_6" "Java source remains version 6"
require_literal "$PROJECT_DIR/app/build.gradle" \
    "targetCompatibility JavaVersion.VERSION_1_6" "Java bytecode target remains version 6"
require_literal "$PROJECT_DIR/app/build.gradle" \
    'applicationId "io.pihda.wavesnap"' "WaveSnap application ID is pinned"
require_literal "$PROJECT_DIR/app/src/main/AndroidManifest.xml" \
    'package="io.pihda.wavesnap"' "WaveSnap manifest package is pinned"
require_literal "$PROJECT_DIR/app/src/main/AndroidManifest.xml" \
    'android:name=".GestureCameraActivity"' "gesture camera is the direct launcher"
require_literal "$PROJECT_DIR/app/src/main/jni/Application.mk" \
    "APP_ABI := armeabi" "native ABI remains armeabi"
require_literal "$PROJECT_DIR/app/src/main/jni/Application.mk" \
    "APP_PLATFORM := android-10" "native API remains android-10"
require_literal "$PROJECT_DIR/app/src/main/jni/Android.mk" \
    "LOCAL_ARM_MODE := arm" "native hot path remains ARM mode"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/CameraSequenceFrameSource.java" \
    "POLL_INTERVAL_MS = 90" "analytical polling remains within the 8-12 FPS target"

require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/GestureCameraActivity.java" \
    "CameraEx.open(0, null)" "Sony camera opens through the verified path"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/GestureCameraActivity.java" \
    "takePicture(null, null, null)" "manual and automatic capture use the verified path"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/GestureCameraActivity.java" \
    "mainHandler.removeCallbacksAndMessages(null)" "pause cancels pending UI callbacks"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/GestureCameraActivity.java" \
    "stopAndJoin(2000)" "pause performs bounded worker join"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/CameraSequenceFrameSource.java" \
    "memory.release()" "analytical frame buffers are released"
require_literal "$PROJECT_DIR/app/src/main/java/io/pihda/wavesnap/GestureCameraActivity.java" \
    "AUTOMATIC_SHUTTER_RELEASE_MS = 400" "automatic shutter release guard remains enabled"

java_source_count="$(find "$PROJECT_DIR/app/src/main/java" -type f -name '*.java' | wc -l)"
if [[ "$java_source_count" -ne 6 ]]; then
    echo "FAIL: expected exactly six WaveSnap Java source files, found $java_source_count" >&2
    exit 1
fi
pass "only the six gesture runtime Java classes remain"

if rg -n 'ACCESS_WIFI_STATE|CHANGE_WIFI_STATE|android.permission.INTERNET' \
        "$PROJECT_DIR/app/src/main/AndroidManifest.xml" > "$temporary_dir/permissions.txt"; then
    echo "FAIL: demo network permissions remain" >&2
    cat "$temporary_dir/permissions.txt" >&2
    exit 1
fi
pass "demo network permissions are absent"

if rg -n 'com\.github\.ma1co\.pmcademo\.app|Java_com_github_ma1co_pmcademo_app_' \
        "$PROJECT_DIR/app/src" "$PROJECT_DIR/tests" > "$temporary_dir/old-identity.txt"; then
    echo "FAIL: PMCADemo package or JNI identity remains" >&2
    cat "$temporary_dir/old-identity.txt" >&2
    exit 1
fi
pass "PMCADemo package and JNI identities are absent"

if rg -n 'androidx\.|java\.util\.stream|kotlin\.' "$PROJECT_DIR/app/src" \
        --glob '*.java' > "$temporary_dir/prohibited.txt"; then
    echo "FAIL: prohibited modern runtime dependency found" >&2
    cat "$temporary_dir/prohibited.txt" >&2
    exit 1
fi
pass "no prohibited modern Java runtime dependencies"

host_cc="${CC:-cc}"
host_cxx="${CXX:-c++}"
common_sanitizer_flags=(-O1 -g -fsanitize=address -fno-omit-frame-pointer)
"$host_cc" -std=gnu89 "${common_sanitizer_flags[@]}" \
    -I"$PROJECT_DIR/app/src/main/jni/third_party/picojpeg" \
    -c "$PROJECT_DIR/app/src/main/jni/third_party/picojpeg/picojpeg.c" \
    -o "$temporary_dir/picojpeg.o"
"$host_cxx" -std=c++98 "${common_sanitizer_flags[@]}" -Wall -Wextra \
    -I"$PROJECT_DIR/tests/native/include" \
    -I"$PROJECT_DIR/tests/native" \
    -I"$PROJECT_DIR/app/src/main/jni" \
    -I"$JDK8_HOME/include" \
    -I"$JDK8_HOME/include/linux" \
    "$PROJECT_DIR/tests/native/gesture_detector_smoke.cpp" \
    "$temporary_dir/picojpeg.o" -lpthread \
    -o "$temporary_dir/gesture_detector_smoke"
ASAN_OPTIONS=detect_leaks=0 "$temporary_dir/gesture_detector_smoke"
pass "native detector passes AddressSanitizer smoke suite"

"$PROJECT_DIR/scripts/analyze-gesture-log.sh" \
    "$PROJECT_DIR/tests/fixtures/gesture-session.txt" \
    > "$temporary_dir/analyzer-output.txt"
grep -Eq '^automatic captures[[:space:]]+1$' "$temporary_dir/analyzer-output.txt"
grep -Eq '^automatic releases[[:space:]]+1$' "$temporary_dir/analyzer-output.txt"
grep -Eq '^camera translations[[:space:]]+1$' "$temporary_dir/analyzer-output.txt"
grep -Eq '^local flow vectors logged[[:space:]]+1$' "$temporary_dir/analyzer-output.txt"
grep -Eq '^flow-confirmed waves[[:space:]]+1$' "$temporary_dir/analyzer-output.txt"
grep -Eq '^final average process ms[[:space:]]+21$' "$temporary_dir/analyzer-output.txt"
pass "gesture log analyzer passes deterministic fixture"

if [[ "$host_only" == true ]]; then
    echo "Smoke tests complete: $pass_count checks passed (host-only)"
    exit 0
fi

build_args=()
if [[ "$clean" == true ]]; then
    build_args+=(--clean)
fi
"$PROJECT_DIR/scripts/build.sh" "${build_args[@]}"
pass "legacy Gradle build and APK verification succeed"

apk="$(latest_apk)"
readelf_bin="$ANDROID_NDK_ROOT/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-readelf"
require_file "$readelf_bin"
unzip -p "$apk" lib/armeabi/libgesture_detector.so \
    > "$temporary_dir/libgesture_detector.so"
"$readelf_bin" -h "$temporary_dir/libgesture_detector.so" \
    > "$temporary_dir/elf-header.txt"
"$readelf_bin" -A "$temporary_dir/libgesture_detector.so" \
    > "$temporary_dir/elf-attributes.txt"
grep -Fq "Class:                             ELF32" "$temporary_dir/elf-header.txt"
grep -Fq "Machine:                           ARM" "$temporary_dir/elf-header.txt"
grep -Fq "soft-float ABI" "$temporary_dir/elf-header.txt"
grep -Fq "Tag_CPU_arch: v5TE" "$temporary_dir/elf-attributes.txt"
grep -Fq "Tag_ARM_ISA_use: Yes" "$temporary_dir/elf-attributes.txt"
if grep -Fq "Tag_Advanced_SIMD_arch" "$temporary_dir/elf-attributes.txt"; then
    echo "FAIL: APK unexpectedly requires Advanced SIMD/NEON" >&2
    exit 1
fi
pass "APK native library is ARMv5TE ELF32 soft-float without NEON"

echo "Smoke tests complete: $pass_count checks passed"
