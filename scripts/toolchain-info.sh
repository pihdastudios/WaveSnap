#!/usr/bin/env bash

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"

require_file "$GRADLE_BIN"
require_file "$ANDROID_NDK_ROOT/source.properties"

echo "JAVA_HOME=$JAVA_HOME"
"$JAVA_HOME/bin/java" -version 2>&1
echo
"$GRADLE_BIN" --version
echo
echo "SDK=$ANDROID_SDK_ROOT"
sed -n '1,20p' "$ANDROID_SDK_ROOT/build-tools/25.0.2/source.properties"
echo
echo "NDK=$ANDROID_NDK_ROOT"
sed -n '1,20p' "$ANDROID_NDK_ROOT/source.properties"
echo
echo "Sony-PMCA-RE commit: $(git -C "$TOOLCHAIN_DIR/Sony-PMCA-RE" rev-parse HEAD)"
