#!/bin/sh
set -eu

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FORMATTER=${CLANG_FORMAT:-clang-format}
FORMAT_ARGS=-i

if [ "${1:-}" = "--check" ]; then
    FORMAT_ARGS="--dry-run --Werror"
    shift
fi
if [ "$#" -ne 0 ]; then
    echo "usage: $0 [--check]" >&2
    exit 2
fi

if ! command -v "$FORMATTER" >/dev/null 2>&1; then
    echo "error: clang-format is required (or set CLANG_FORMAT)" >&2
    exit 1
fi

cd "$PROJECT_DIR"
find app/src/main/java app/src/main/jni tests/native \
    -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' \
    -o -name '*.hpp' -o -name '*.java' \) ! -path '*/third_party/*' -print0 \
    | xargs -0 "$FORMATTER" $FORMAT_ARGS
