#!/usr/bin/env bash

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"

apk="${1:-}"
if [[ -z "$apk" ]]; then
    apk="$(latest_apk)"
fi
if [[ "$apk" != /* ]]; then
    apk="$(cd "$(dirname "$apk")" && pwd)/$(basename "$apk")"
fi

require_file "$apk"
require_file "$ANDROID_SDK_ROOT/build-tools/25.0.2/aapt"
require_file "$ANDROID_SDK_ROOT/build-tools/25.0.2/apksigner"

badging="$($ANDROID_SDK_ROOT/build-tools/25.0.2/aapt dump badging "$apk")"
grep -q "package: name='io.pihda.wavesnap' versionCode='1' versionName='1.0.0'" <<< "$badging"
grep -q "launchable-activity: name='io.pihda.wavesnap.GestureCameraActivity'" <<< "$badging"
grep -q "label='WaveSnap'" <<< "$badging"
grep -q "sdkVersion:'10'" <<< "$badging"
grep -q "targetSdkVersion:'10'" <<< "$badging"

unexpected_libs="$(unzip -Z1 "$apk" | awk '/^lib\// && $0 !~ /^lib\/armeabi\// { print }')"
if [[ -n "$unexpected_libs" ]]; then
    echo "Unexpected native ABI entries:" >&2
    echo "$unexpected_libs" >&2
    exit 1
fi

PATH="$JDK8_HOME/bin:/usr/bin:/bin" \
    "$ANDROID_SDK_ROOT/build-tools/25.0.2/apksigner" verify --verbose "$apk"

sha256sum "$apk"
ls -lh "$apk"
echo "$badging" | sed -n '1,12p'
unzip -Z1 "$apk" | awk '/^lib\// { print "native: " $0 }'
echo "Verified APK: $apk"
