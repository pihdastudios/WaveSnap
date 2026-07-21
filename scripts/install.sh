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

require_file "$PMCA_PYTHON"
require_file "$PMCA_CONSOLE"
"$SCRIPT_DIR/usb-status.sh"
"$SCRIPT_DIR/verify-apk.sh" "$apk"

echo "Installing $(basename "$apk") with Sony-PMCA-RE"
(
    cd "$WORKSPACE_DIR"
    "$PMCA_PYTHON" "$PMCA_CONSOLE" install -f "$apk"
)
