#!/usr/bin/env bash

set -euo pipefail

sony_devices="$(lsusb | awk 'tolower($0) ~ /sony|054c:/ { print }')"
if [[ -z "$sony_devices" ]]; then
    echo "No Sony USB device detected" >&2
    exit 1
fi

echo "$sony_devices"
if grep -q '054c:0994' <<< "$sony_devices"; then
    echo "Camera is in charging mode; select USB Connection -> Mass Storage and reconnect." >&2
    exit 2
fi
if grep -q '054c:07cd' <<< "$sony_devices"; then
    echo "ILCE-5100 detected in a PMCA-compatible USB mode."
fi
