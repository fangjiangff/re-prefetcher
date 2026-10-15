#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLANG="${CLANG:-$ROOT_DIR/llvm-project/build/bin/clang}"
SOURCE="$ROOT_DIR/LLVM_FIX/tests/dual_address.c"

output="$($CLANG --target=aarch64-unknown-linux-gnu -O2 -g \
  -mllvm -aarch64-mir-analyze -c "$SOURCE" -o /dev/null 2>&1)"
marker="Control-flow dual-address Gadgets Found in ---->"

for function in dual_load_min dual_store_max dual_mixed dual_store_load \
  dual_after_empty_block; do
  if ! grep -Fq "$marker  $function" <<<"$output"; then
    echo "missing dual-address gadget: $function" >&2
    exit 1
  fi
done

for function in dual_too_near dual_too_far; do
  if grep -Fq "$marker  $function" <<<"$output"; then
    echo "unexpected dual-address gadget: $function" >&2
    exit 1
  fi
done
