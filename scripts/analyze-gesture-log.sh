#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 || ! -f "$1" ]]; then
    echo "Usage: $0 /path/to/WAVESNAP-log.txt" >&2
    exit 2
fi

log_file="$1"
session_start="$(awk '/GestureCamera: onResume begin/ { line = NR } END { print line + 0 }' "$log_file")"
if [[ "$session_start" -eq 0 ]]; then
    echo "No GestureCamera session found in $log_file" >&2
    exit 2
fi

session_file="$(mktemp)"
trap 'rm -f "$session_file"' EXIT
awk -v start="$session_start" 'NR >= start { print }' "$log_file" > "$session_file"

count_text() {
    awk -v text="$1" 'index($0, text) { count++ } END { print count + 0 }' "$session_file"
}

accepted="$(count_text 'countdown accepted-wave')"
countdowns="$(count_text 'countdown start')"
auto_focus="$(count_text 'autofocus requested source=automatic')"
auto_capture="$(count_text 'capture requested source=automatic')"
auto_release="$(count_text 'automatic shutter released')"
manual_overrides="$(count_text 'manual shutter cancelled automatic countdown')"
manual_capture="$(count_text 'capture requested source=manual')"
manual_release="$(count_text 'manual shutter release')"
capture_failures="$(count_text 'operation=takePicture')"
cooldowns="$(count_text 'cooldown start')"
rearms="$(count_text 'cooldown end and rearm')"
ignored="$(count_text 'ignored-wave')"
pauses="$(count_text 'GestureCamera: onPause complete')"
translations="$(awk '
    /cameraShift=/ {
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^cameraShift=/) {
                split($i, pair, "=")
                split(pair[2], shift, ",")
                if (shift[1] + 0 != 0 || shift[2] + 0 != 0) count++
            }
        }
    }
    END { print count + 0 }
' "$session_file")"
flow_vectors="$(awk '
    /localFlow=/ {
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^localFlow=/) {
                split($i, pair, "=")
                if (pair[2] + 0 != 0) count++
            }
        }
    }
    END { print count + 0 }
' "$session_file")"
flow_waves="$(awk '
    /confirmed-wave/ && /source=local-flow/ { confirmed++ }
    /result=2/ && /localFlow=/ {
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^localFlow=/) {
                split($i, pair, "=")
                if (pair[2] + 0 != 0) diagnostic++
            }
        }
    }
    END { print (confirmed > 0 ? confirmed : diagnostic + 0) }
' "$session_file")"

echo "Gesture session starts at source line $session_start"
printf '%-28s %s\n' \
    "accepted waves" "$accepted" \
    "countdowns" "$countdowns" \
    "automatic autofocus" "$auto_focus" \
    "automatic captures" "$auto_capture" \
    "automatic releases" "$auto_release" \
    "manual overrides" "$manual_overrides" \
    "manual captures" "$manual_capture" \
    "manual releases" "$manual_release" \
    "takePicture failures" "$capture_failures" \
    "cooldowns" "$cooldowns" \
    "rearms" "$rearms" \
    "ignored cooldown waves" "$ignored" \
    "camera translations" "$translations" \
    "local flow vectors logged" "$flow_vectors" \
    "flow-confirmed waves" "$flow_waves" \
    "clean pause completions" "$pauses"

awk '
    /GestureFrames: analyticalFps=/ {
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^analyticalFps=/) {
                value = substr($i, index($i, "=") + 1) + 0
                fps_sum += value
                fps_count++
                if (fps_count == 1 || value < fps_min) fps_min = value
                if (fps_count == 1 || value > fps_max) fps_max = value
            }
        }
    }
    /GestureCamera: nativeFrame=/ {
        for (i = 1; i <= NF; ++i) {
            if ($i ~ /^averageProcessMs=/) {
                final_process = substr($i, index($i, "=") + 1) + 0
            }
        }
    }
    END {
        if (fps_count > 0) {
            printf "%-28s %.2f (%.2f..%.2f, %d windows)\n", \
                    "analytical FPS", fps_sum / fps_count, fps_min, fps_max, fps_count
        }
        if (final_process > 0) {
            printf "%-28s %d\n", "final average process ms", final_process
        }
    }
' "$session_file"

echo "Event timeline:"
if command -v rg >/dev/null 2>&1; then
    rg 'countdown (accepted-wave|start)|manual shutter cancelled|autofocus requested source=automatic|capture requested source=(automatic|manual)|automatic shutter released|manual shutter release|cooldown (start|end)|ignored-wave|onPause (begin|complete)|\[ERROR\]' "$session_file" || true
else
    grep -E 'countdown (accepted-wave|start)|manual shutter cancelled|autofocus requested source=automatic|capture requested source=(automatic|manual)|automatic shutter released|manual shutter release|cooldown (start|end)|ignored-wave|onPause (begin|complete)|\[ERROR\]' "$session_file" || true
fi

if [[ "$capture_failures" -ne 0 || "$auto_capture" -ne "$auto_release" ]]; then
    echo "Gesture session failed capture/release consistency checks" >&2
    exit 1
fi
