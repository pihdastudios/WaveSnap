#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
TOOLCHAIN_DIR="${SONY_TOOLCHAIN_DIR:-$WORKSPACE_DIR/toolchain}"
JDK8_HOME="${SONY_JAVA_HOME:-/home/klm/.package-manager/jdk}"

GRADLE_BIN="$TOOLCHAIN_DIR/gradle-2.14.1/bin/gradle"
GRADLE_INIT_SCRIPT="$TOOLCHAIN_DIR/legacy-repositories.gradle"
ANDROID_SDK_ROOT="$TOOLCHAIN_DIR/android-sdk"
ANDROID_NDK_ROOT="$TOOLCHAIN_DIR/android-ndk-r14b"
ANDROID_USER_ROOT="$TOOLCHAIN_DIR/android-user-home"
GRADLE_CACHE_ROOT="$TOOLCHAIN_DIR/gradle-user-home-2.14.1"
PMCA_PYTHON="$TOOLCHAIN_DIR/pmca-venv/bin/python"
PMCA_CONSOLE="$TOOLCHAIN_DIR/Sony-PMCA-RE/pmca-console.py"
APK_DIR="$PROJECT_DIR/app/build/outputs/apk"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "Missing required file: $1" >&2
        exit 1
    fi
}

require_dir() {
    if [[ ! -d "$1" ]]; then
        echo "Missing required directory: $1" >&2
        exit 1
    fi
}

latest_apk() {
    local latest=""
    local candidate
    shopt -s nullglob
    for candidate in "$APK_DIR"/*.apk; do
        if [[ -z "$latest" || "$candidate" -nt "$latest" ]]; then
            latest="$candidate"
        fi
    done
    shopt -u nullglob
    if [[ -z "$latest" ]]; then
        echo "No APK found under $APK_DIR" >&2
        return 1
    fi
    printf '%s\n' "$latest"
}

export JAVA_HOME="$JDK8_HOME"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_SDK_HOME="$ANDROID_USER_ROOT"
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"
export GRADLE_USER_HOME="$GRADLE_CACHE_ROOT"
export PATH="$JDK8_HOME/bin:$PATH"
