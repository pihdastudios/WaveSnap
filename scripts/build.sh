#!/usr/bin/env bash

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"

clean=false
offline=true
tasks=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=true ;;
        --online) offline=false ;;
        --help|-h)
            echo "Usage: $0 [--clean] [--online] [gradle-task ...]"
            exit 0
            ;;
        *) tasks+=("$1") ;;
    esac
    shift
done

require_file "$GRADLE_BIN"
require_file "$GRADLE_INIT_SCRIPT"
require_dir "$ANDROID_SDK_ROOT"
require_dir "$ANDROID_NDK_ROOT"

if [[ ${#tasks[@]} -eq 0 ]]; then
    tasks=(assembleDebug)
fi
if [[ "$clean" == true ]]; then
    tasks=(clean "${tasks[@]}")
fi

gradle_args=(--no-daemon --init-script "$GRADLE_INIT_SCRIPT")
if [[ "$offline" == true ]]; then
    gradle_args+=(--offline)
fi

echo "Building WaveSnap with JDK 8, Gradle 2.14.1, API 10, and NDK r14b"
(
    cd "$PROJECT_DIR"
    "$GRADLE_BIN" "${gradle_args[@]}" "${tasks[@]}"
)

if [[ " ${tasks[*]} " == *" assembleDebug "* ]]; then
    "$SCRIPT_DIR/verify-apk.sh"
fi
