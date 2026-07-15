#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  collect_gadget_table.sh [--latex] <log-dir>

The log directory must contain:
  openssl_O2.log libgcrypt_O2.log libsodium_O2.log OpenBLAS_O2.log
  musl_O2.log zlib_O2.log wolfssl_O2.log
USAGE
}

format="markdown"
if [[ "${1:-}" == "--latex" ]]; then
  format="latex"
  shift
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

log_dir="$1"
if [[ ! -d "$log_dir" ]]; then
  echo "error: log directory does not exist: $log_dir" >&2
  exit 1
fi

libs=(
  "OpenSSL 1.1.1q"
  "Libgcrypt 1.11.0"
  "libsodium 1.0.20"
  "OpenBLAS 0.3.0"
  "musl 1.2.5"
  "zlib 1.3.1"
  "wolfssl 5.8.4"
)

logs=(
  "openssl_O2.log"
  "libgcrypt_O2.log"
  "libsodium_O2.log"
  "OpenBLAS_O2.log"
  "musl_O2.log"
  "zlib_O2.log"
  "wolfssl_O2.log"
)

variable_marker="Variable-time memory access Gadgets Found"
rep_marker="REP-MOVSB Gadgets Found"

count_marker() {
  local marker="$1"
  local file="$2"
  grep -F -c "$marker" "$file" || true
}

variable_counts=()
rep_counts=()

for log_name in "${logs[@]}"; do
  log_path="$log_dir/$log_name"
  if [[ ! -f "$log_path" ]]; then
    echo "error: missing log file: $log_path" >&2
    exit 1
  fi
  variable_counts+=("$(count_marker "$variable_marker" "$log_path")")
  rep_counts+=("$(count_marker "$rep_marker" "$log_path")")
done

if [[ "$format" == "latex" ]]; then
  printf '\\begin{tabular}{lrrrrrrr}\n'
  printf '\\toprule\n'
  printf ' & %s' "${libs[0]}"
  for ((i = 1; i < ${#libs[@]}; i++)); do
    printf ' & %s' "${libs[$i]}"
  done
  printf ' \\\\\n'
  printf '\\midrule\n'
  printf 'variable-time memory access gadgets'
  for value in "${variable_counts[@]}"; do
    printf ' & %s' "$value"
  done
  printf ' \\\\\n'
  printf 'REP-MOVSB gadgets'
  for value in "${rep_counts[@]}"; do
    printf ' & %s' "$value"
  done
  printf ' \\\\\n'
  printf '\\bottomrule\n'
  printf '\\end{tabular}\n'
else
  printf '| Gadget type |'
  for lib in "${libs[@]}"; do
    printf ' %s |' "$lib"
  done
  printf '\n'

  printf '|---|'
  for _ in "${libs[@]}"; do
    printf '%s' '---:|'
  done
  printf '\n'

  printf '| variable-time memory access gadgets |'
  for value in "${variable_counts[@]}"; do
    printf ' %s |' "$value"
  done
  printf '\n'

  printf '| REP-MOVSB gadgets |'
  for value in "${rep_counts[@]}"; do
    printf ' %s |' "$value"
  done
  printf '\n'
fi
