#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON:-python3}"

usage() {
    printf 'Usage: %s --exp <experiment>\n' "$(basename "$0")"
    printf '\nAvailable experiments:\n'
    printf '  cc-pmu  Run the X925 store and load covert-channel PMU experiments.\n'
    printf '  cc-cache  Run the X925 store and load covert-channel cache experiments.\n'
    printf '  cc  Run both cc-pmu and cc-cache and save output to cc.txt.\n'
}

run_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

run_cc_pmu() {
    run_command "$PYTHON_BIN" "$SCRIPT_DIR/covert-channel-poc.py" \
        --arch X925 \
        --rounds 10000
    run_command "$PYTHON_BIN" "$SCRIPT_DIR/covert-channel-poc.py" \
        --arch X925 \
        --access load \
        --train-step 20 \
        --rounds 10000
}

run_cc_cache() {
    run_command "$PYTHON_BIN" "$SCRIPT_DIR/covert-channel-cache.py" \
        --arch X925 \
        --rounds 10000
    run_command "$PYTHON_BIN" "$SCRIPT_DIR/covert-channel-cache.py" \
        --arch X925 \
        --access load \
        --train-step 20 \
        --rounds 10000
}

experiment=""
while (($# > 0)); do
    case "$1" in
        --exp)
            if (($# < 2)); then
                printf 'Error: --exp requires a value.\n' >&2
                usage >&2
                exit 2
            fi
            experiment="$2"
            shift 2
            ;;
        --exp=*)
            experiment="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Error: unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$experiment" ]]; then
    printf 'Error: --exp is required.\n' >&2
    usage >&2
    exit 2
fi

case "$experiment" in
    cc-pmu)
        run_cc_pmu
        ;;
    cc-cache)
        run_cc_cache
        ;;
    cc)
        output_path="$SCRIPT_DIR/cc.txt"
        {
            printf "Run date: "
            date "+%Y-%m-%d %H:%M:%S %Z"
            run_cc_pmu
            run_cc_cache
        } 2>&1 | tee "$output_path"
        ;;
    *)
        printf "Error: unknown experiment: %s\n" "$experiment" >&2
        usage >&2
        exit 2
        ;;
esac
