#!/usr/bin/env bash
set -euo pipefail

MSR_MISC_FEATURE_CONTROL=0x1a4
MSR_SPEC_CTRL=0x48

BIT_L2_HW=0
BIT_L2_ADJ=1
BIT_L1_DCU=2
BIT_L1_IP=3
BIT_L2_AMP=5
BIT_SSBD=2
BIT_DDPD_U=8

KNOWN_1A4_MASK=$(( (1 << BIT_L2_HW) | (1 << BIT_L2_ADJ) | (1 << BIT_L1_DCU) | (1 << BIT_L1_IP) | (1 << BIT_L2_AMP) ))
SPEC_DDP_MASK=$((1 << BIT_DDPD_U))
SPEC_SSBD_MASK=$((1 << BIT_SSBD))

core=""
action=""
prefetcher=""
keep_ssbd=0

usage() {
    cat <<'EOF'
Usage:
  sudo ./intel-prefetcher.sh --core <cpu> --show
  sudo ./intel-prefetcher.sh --core <cpu> --all
  sudo ./intel-prefetcher.sh --core <cpu> --only <prefetcher>
  sudo ./intel-prefetcher.sh --core <cpu> --disable-all

Prefetcher names:
  l2_hw        MSR_1A4H[0] L2 hardware prefetcher
  l2_adjacent  MSR_1A4H[1] L2 adjacent cache line prefetcher
  l1_dcu       MSR_1A4H[2] L1 data cache prefetcher
  l1_ip        MSR_1A4H[3] L1 data cache IP prefetcher
  l2_amp       MSR_1A4H[5] L2 Adaptive Multipath Probability prefetcher
  ddp          MSR_48H[8]  Data Dependent Prefetcher disable bit

Options:
  --keep-ssbd  When enabling ddp, do not clear MSR_48H[2] SSBD.

Notes:
  MSR_1A4H prefetcher bits are disable bits: 1 disables, 0 enables.
  MSR_48H[8] DDPD_U is also a disable bit: 1 disables DDP at CPL=3.
  MSR_48H[2] SSBD also disables DDP. By default, --all and --only ddp
  clear SSBD so that DDP can actually be enabled. Use --keep-ssbd to
  leave the security mitigation untouched.

Examples:
  sudo ./intel-prefetcher.sh --core 0 --only l1_ip
  sudo ./intel-prefetcher.sh --core 0 --only l2_amp
  sudo ./intel-prefetcher.sh --core 0 --only ddp
  sudo ./intel-prefetcher.sh --core 0 --all
  sudo ./intel-prefetcher.sh --core 0 --show
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

need_arg() {
    local opt="$1"
    local val="${2:-}"
    [[ -n "$val" ]] || die "$opt requires an argument"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--core)
            need_arg "$1" "${2:-}"
            core="$2"
            shift 2
            ;;
        --show)
            action="show"
            shift
            ;;
        --all)
            action="all"
            shift
            ;;
        --disable-all)
            action="disable_all"
            shift
            ;;
        --only)
            need_arg "$1" "${2:-}"
            action="only"
            prefetcher="$2"
            shift 2
            ;;
        --keep-ssbd)
            keep_ssbd=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ -n "$core" ]] || die "missing --core <cpu>"
[[ "$core" =~ ^[0-9]+$ ]] || die "--core must be a non-negative integer"
[[ -n "$action" ]] || die "missing action: --show, --all, --only, or --disable-all"

command -v rdmsr >/dev/null 2>&1 || die "rdmsr not found; install msr-tools"
command -v wrmsr >/dev/null 2>&1 || die "wrmsr not found; install msr-tools"

if [[ ! -e "/dev/cpu/$core/msr" ]]; then
    die "/dev/cpu/$core/msr does not exist; try: sudo modprobe msr"
fi

read_msr() {
    local msr="$1"
    local value
    value="$(rdmsr -p "$core" "$msr")" || die "failed to read MSR $msr on CPU $core"
    printf '%d\n' "$((16#$value))"
}

write_msr() {
    local msr="$1"
    local value="$2"
    wrmsr -p "$core" "$msr" "$(printf '0x%x' "$value")" || die "failed to write MSR $msr on CPU $core"
}

bit_state() {
    local value="$1"
    local bit="$2"
    if (( (value & (1 << bit)) == 0 )); then
        echo "enabled"
    else
        echo "disabled"
    fi
}

ddp_state() {
    local spec="$1"
    if (( (spec & SPEC_DDP_MASK) != 0 )); then
        echo "disabled"
    elif (( (spec & SPEC_SSBD_MASK) != 0 )); then
        echo "disabled-by-SSBD"
    else
        echo "enabled"
    fi
}

show_state() {
    local misc="$1"
    local spec="$2"
    printf 'CPU %s\n' "$core"
    printf '  MSR_1A4H = 0x%x\n' "$misc"
    printf '    l2_hw       bit 0: %s\n' "$(bit_state "$misc" "$BIT_L2_HW")"
    printf '    l2_adjacent bit 1: %s\n' "$(bit_state "$misc" "$BIT_L2_ADJ")"
    printf '    l1_dcu      bit 2: %s\n' "$(bit_state "$misc" "$BIT_L1_DCU")"
    printf '    l1_ip       bit 3: %s\n' "$(bit_state "$misc" "$BIT_L1_IP")"
    printf '    l2_amp      bit 5: %s\n' "$(bit_state "$misc" "$BIT_L2_AMP")"
    printf '  MSR_48H  = 0x%x\n' "$spec"
    printf '    ddp         bit 8: %s\n' "$(ddp_state "$spec")"
    if (( (spec & SPEC_SSBD_MASK) != 0 )); then
        printf '    ssbd        bit 2: set, DDP is also disabled\n'
    else
        printf '    ssbd        bit 2: clear\n'
    fi
}

prefetcher_bit() {
    case "$1" in
        l2_hw) echo "$BIT_L2_HW" ;;
        l2_adjacent|l2_adj) echo "$BIT_L2_ADJ" ;;
        l1_dcu|l1_data) echo "$BIT_L1_DCU" ;;
        l1_ip|dcu_ip) echo "$BIT_L1_IP" ;;
        l2_amp|amp) echo "$BIT_L2_AMP" ;;
        ddp) echo "ddp" ;;
        *)
            die "unknown prefetcher '$1'; use one of: l2_hw, l2_adjacent, l1_dcu, l1_ip, l2_amp, ddp"
            ;;
    esac
}

misc_before="$(read_msr "$MSR_MISC_FEATURE_CONTROL")"
spec_before="$(read_msr "$MSR_SPEC_CTRL")"
misc_after="$misc_before"
spec_after="$spec_before"

case "$action" in
    show)
        show_state "$misc_before" "$spec_before"
        exit 0
        ;;
    all)
        misc_after=$((misc_after & ~KNOWN_1A4_MASK))
        spec_after=$((spec_after & ~SPEC_DDP_MASK))
        if (( keep_ssbd == 0 )); then
            spec_after=$((spec_after & ~SPEC_SSBD_MASK))
        fi
        ;;
    disable_all)
        misc_after=$((misc_after | KNOWN_1A4_MASK))
        spec_after=$((spec_after | SPEC_DDP_MASK))
        ;;
    only)
        bit="$(prefetcher_bit "$prefetcher")"
        misc_after=$((misc_after | KNOWN_1A4_MASK))
        spec_after=$((spec_after | SPEC_DDP_MASK))
        if [[ "$bit" == "ddp" ]]; then
            spec_after=$((spec_after & ~SPEC_DDP_MASK))
            if (( keep_ssbd == 0 )); then
                spec_after=$((spec_after & ~SPEC_SSBD_MASK))
            fi
        else
            misc_after=$((misc_after & ~(1 << bit)))
        fi
        ;;
esac

printf 'Before:\n'
show_state "$misc_before" "$spec_before"

if (( misc_after != misc_before )); then
    write_msr "$MSR_MISC_FEATURE_CONTROL" "$misc_after"
fi
if (( spec_after != spec_before )); then
    write_msr "$MSR_SPEC_CTRL" "$spec_after"
fi

misc_verify="$(read_msr "$MSR_MISC_FEATURE_CONTROL")"
spec_verify="$(read_msr "$MSR_SPEC_CTRL")"

printf '\nAfter:\n'
show_state "$misc_verify" "$spec_verify"

if [[ "$action" == "all" || ( "$action" == "only" && "$prefetcher" == "ddp" ) ]]; then
    if (( keep_ssbd == 1 && (spec_verify & SPEC_SSBD_MASK) != 0 )); then
        printf '\nwarning: SSBD is still set, so DDP remains disabled even though DDPD_U is clear.\n' >&2
    fi
fi
