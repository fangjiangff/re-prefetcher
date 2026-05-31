#!/usr/bin/env bash
set -euo pipefail

MSR_MISC_FEATURE_CONTROL=0x1a4
MSR_SPEC_CTRL=0x48

# MSR_1A4 bits.  The bit numbers are reused across profiles, but the
# *meaning* differs between Core/P-core and Atom/Gracemont E-core.
BIT_1A4_0=0
BIT_1A4_1=1
BIT_1A4_2=2
BIT_1A4_3=3
BIT_1A4_5=5

BIT_SSBD=2
BIT_DDPD_U=8

# Core/P-core and old Core profiles: documented MSR_1A4[0:3].
COMMON_1A4_MASK=$(( (1 << BIT_1A4_0) | (1 << BIT_1A4_1) | (1 << BIT_1A4_2) | (1 << BIT_1A4_3) ))
# Raptor Cove/P-core: documented MSR_1A4[0:3,5].
PCORE_1A4_MASK=$(( COMMON_1A4_MASK | (1 << BIT_1A4_5) ))
# Gracemont/Atom E-core per Intel Atom MSR 0x1A4 table:
#   bit 0: MLC Stream disabled
#   bit 2: L1 data Stream disabled
#   bit 3: L1 instruction Stream disabled
#   bit 5: L2 AMP disabled
# Do not touch bit 1 on Atom cores; it is not the L2 adjacent-line bit there.
ATOM_1A4_MASK=$(( (1 << BIT_1A4_0) | (1 << BIT_1A4_2) | (1 << BIT_1A4_3) | (1 << BIT_1A4_5) ))

SPEC_DDP_MASK=$((1 << BIT_DDPD_U))
SPEC_SSBD_MASK=$((1 << BIT_SSBD))

core=""
action=""
prefetcher=""
cpu_arch="auto"

resolved_arch=""
arch_label=""
detected_cpu_name=""
detected_cpu_family=""
detected_cpu_model=""
support_spec_ctrl=0
known_1a4_mask=0
supported_prefetchers=()

usage() {
    cat <<'EOF_USAGE'
Usage:
  sudo ./intel-prefetcher-fixed.sh --core <cpu> --show
  sudo ./intel-prefetcher-fixed.sh --core <cpu> --all
  sudo ./intel-prefetcher-fixed.sh --core <cpu> --only <prefetcher>
  sudo ./intel-prefetcher-fixed.sh --core <cpu> --disable-all

Prefetcher names by profile:
  generic / Skylake / Cascade Lake:
    l2_hw        MSR_1A4H[0] L2 hardware prefetcher
    l2_adjacent  MSR_1A4H[1] L2 adjacent cache-line prefetcher
    l1_dcu       MSR_1A4H[2] DCU/L1D hardware prefetcher
    l1_ip        MSR_1A4H[3] DCU IP/L1D IP prefetcher

  raptorcove / P-core:
    l2_hw        MSR_1A4H[0] L2 hardware prefetcher
    l2_adjacent  MSR_1A4H[1] L2 adjacent cache-line prefetcher
    l1_dcu       MSR_1A4H[2] DCU/L1D hardware prefetcher
    l1_ip        MSR_1A4H[3] DCU IP/L1D IP prefetcher
    l2_amp       MSR_1A4H[5] L2 AMP prefetcher
    ddp          MSR_48H[8]  Data Dependent Prefetcher disable bit for CPL3

  gracemont / Atom E-core:
    mlc_stream              MSR_1A4H[0] MLC Streamer
    l1_data_stream          MSR_1A4H[2] L1 data Streamer
    l1_instruction_stream   MSR_1A4H[3] L1 instruction Streamer
    l2_amp                  MSR_1A4H[5] L2 AMP prefetcher

Options:
  --cpu-arch <name>
      auto            Detect from /sys/devices/cpu_{core,atom}/cpus when available
      raptorcove|rc   12th/13th Gen Core P-core 0x1A4[0:3,5], plus optional DDP
      gracemont|gm    Atom E-core 0x1A4[0,2,3,5]
      cascadelake|cl  Conservative MSR_1A4H[0:3]
      skylake|skx     Conservative MSR_1A4H[0:3]
      generic         Conservative MSR_1A4H[0:3]

Notes:
  MSR_1A4H bits are disable bits: 1 disables, 0 enables.
  Gracemont MSR_1A4H[3] is L1 instruction Stream, not Core-family DCU IP.
  This script never clears MSR_48H[2] SSBD automatically. If SSBD is set,
  DDP remains effectively disabled even if DDPD_U is clear.

Examples:
  sudo ./intel-prefetcher-fixed.sh --core 0 --show
  sudo ./intel-prefetcher-fixed.sh --core 0 --all
  sudo ./intel-prefetcher-fixed.sh --core 0 --only l1_ip
  sudo ./intel-prefetcher-fixed.sh --core 16 --cpu-arch gracemont --only l1_data_stream
  sudo ./intel-prefetcher-fixed.sh --core 16 --cpu-arch gracemont --disable-all
EOF_USAGE
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

join_by() {
    local sep="$1"
    shift
    local first=1
    local item
    for item in "$@"; do
        if (( first )); then
            printf '%s' "$item"
            first=0
        else
            printf '%s%s' "$sep" "$item"
        fi
    done
}

cpu_in_cpulist() {
    local cpu="$1"
    local list="$2"
    local part start end
    IFS=',' read -r -a parts <<<"$list"
    for part in "${parts[@]}"; do
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            start="${BASH_REMATCH[1]}"
            end="${BASH_REMATCH[2]}"
            if (( cpu >= start && cpu <= end )); then
                return 0
            fi
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            if (( cpu == part )); then
                return 0
            fi
        fi
    done
    return 1
}

detect_core_type_from_sysfs() {
    local list
    if [[ -r /sys/devices/cpu_atom/cpus ]]; then
        list="$(cat /sys/devices/cpu_atom/cpus)"
        if cpu_in_cpulist "$core" "$list"; then
            echo "atom"
            return 0
        fi
    fi

    if [[ -r /sys/devices/cpu_core/cpus ]]; then
        list="$(cat /sys/devices/cpu_core/cpus)"
        if cpu_in_cpulist "$core" "$list"; then
            echo "core"
            return 0
        fi
    fi

    echo "unknown"
}

detect_cpu_signature() {
    awk -F: -v target="$core" '
        function trim(s) {
            sub(/^[ 	]+/, "", s)
            sub(/[ 	]+$/, "", s)
            return s
        }

        /^processor[[:space:]]*:/ {
            current = trim($2)
            found = (current == target)
            next
        }

        found && /^cpu family[[:space:]]*:/ {
            family = trim($2)
            next
        }

        found && /^model[[:space:]]*:/ && $1 !~ /model name/ {
            model = trim($2)
            next
        }

        found && /^model name[[:space:]]*:/ {
            name = trim($2)
            next
        }

        found && NF == 0 {
            print family "|" model "|" name
            exit
        }

        END {
            if (found) {
                print family "|" model "|" name
            }
        }
    ' /proc/cpuinfo
}

configure_arch() {
    local signature core_type

    case "$cpu_arch" in
        auto)
            signature="$(detect_cpu_signature)"
            [[ -n "$signature" ]] || die "failed to detect CPU signature for core $core"
            IFS='|' read -r detected_cpu_family detected_cpu_model detected_cpu_name <<<"$signature"

            core_type="$(detect_core_type_from_sysfs)"
            case "$core_type" in
                atom)
                    resolved_arch="gracemont"
                    ;;
                core)
                    resolved_arch="raptorcove"
                    ;;
                unknown)
                    case "${detected_cpu_family}:${detected_cpu_model}" in
                        # Hybrid Alder/Raptor Lake: family/model alone cannot distinguish P-core vs E-core.
                        6:151|6:154|6:183|6:186|6:191)
                            die "hybrid CPU model detected, but core type for CPU $core could not be determined; pass --cpu-arch raptorcove or --cpu-arch gracemont"
                            ;;
                        # Skylake client/mobile and Skylake-SP/Cascade Lake.
                        6:78|6:94|6:85)
                            resolved_arch="generic"
                            ;;
                        *)
                            resolved_arch="generic"
                            ;;
                    esac
                    ;;
            esac
            ;;
        raptorcove|rc)
            resolved_arch="raptorcove"
            ;;
        gracemont|gm)
            resolved_arch="gracemont"
            ;;
        cascadelake|cl|skylake|skx|generic)
            resolved_arch="generic"
            ;;
        *)
            die "unknown --cpu-arch '$cpu_arch'"
            ;;
    esac

    case "$resolved_arch" in
        raptorcove)
            arch_label="Raptor Cove / Core P-core profile"
            support_spec_ctrl=1
            known_1a4_mask=$PCORE_1A4_MASK
            supported_prefetchers=(l2_hw l2_adjacent l1_dcu l1_ip l2_amp ddp)
            ;;
        gracemont)
            arch_label="Gracemont / Atom E-core profile"
            support_spec_ctrl=0
            known_1a4_mask=$ATOM_1A4_MASK
            supported_prefetchers=(mlc_stream l1_data_stream l1_instruction_stream l2_amp)
            ;;
        generic)
            arch_label="Generic MSR_1A4H[0:3] profile"
            support_spec_ctrl=0
            known_1a4_mask=$COMMON_1A4_MASK
            supported_prefetchers=(l2_hw l2_adjacent l1_dcu l1_ip)
            ;;
    esac
}

supports_prefetcher() {
    local name="$1"
    case "$resolved_arch:$name" in
        generic:l2_hw|generic:l2_adjacent|generic:l2_adj|generic:l1_dcu|generic:l1_data|generic:l1_ip|generic:dcu_ip)
            return 0
            ;;
        raptorcove:l2_hw|raptorcove:l2_adjacent|raptorcove:l2_adj|raptorcove:l1_dcu|raptorcove:l1_data|raptorcove:l1_ip|raptorcove:dcu_ip|raptorcove:l2_amp|raptorcove:amp|raptorcove:ddp)
            return 0
            ;;
        gracemont:mlc_stream|gracemont:l1_data_stream|gracemont:l1_data|gracemont:data_stream|gracemont:l1_instruction_stream|gracemont:l1_instr_stream|gracemont:instruction_stream|gracemont:l2_amp|gracemont:amp)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

prefetcher_bit() {
    if ! supports_prefetcher "$1"; then
        die "prefetcher '$1' is not supported by arch '$resolved_arch'; supported: $(join_by ', ' "${supported_prefetchers[@]}")"
    fi

    case "$resolved_arch:$1" in
        generic:l2_hw|raptorcove:l2_hw) echo "$BIT_1A4_0" ;;
        generic:l2_adjacent|generic:l2_adj|raptorcove:l2_adjacent|raptorcove:l2_adj) echo "$BIT_1A4_1" ;;
        generic:l1_dcu|generic:l1_data|raptorcove:l1_dcu|raptorcove:l1_data) echo "$BIT_1A4_2" ;;
        generic:l1_ip|generic:dcu_ip|raptorcove:l1_ip|raptorcove:dcu_ip) echo "$BIT_1A4_3" ;;
        raptorcove:l2_amp|raptorcove:amp) echo "$BIT_1A4_5" ;;
        raptorcove:ddp) echo "ddp" ;;
        gracemont:mlc_stream) echo "$BIT_1A4_0" ;;
        gracemont:l1_data_stream|gracemont:l1_data|gracemont:data_stream) echo "$BIT_1A4_2" ;;
        gracemont:l1_instruction_stream|gracemont:l1_instr_stream|gracemont:instruction_stream) echo "$BIT_1A4_3" ;;
        gracemont:l2_amp|gracemont:amp) echo "$BIT_1A4_5" ;;
        *) die "unknown prefetcher '$1' for arch '$resolved_arch'" ;;
    esac
}

read_msr() {
    local msr="$1"
    local value
    value="$(rdmsr -p "$core" "$msr")" || die "failed to read MSR $msr on CPU $core"
    printf '%d
' "$((16#$value))"
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

show_1a4_state() {
    local misc="$1"
    case "$resolved_arch" in
        gracemont)
            printf '    mlc_stream              bit 0: %s
' "$(bit_state "$misc" "$BIT_1A4_0")"
            printf '    l1_data_stream          bit 2: %s
' "$(bit_state "$misc" "$BIT_1A4_2")"
            printf '    l1_instruction_stream   bit 3: %s
' "$(bit_state "$misc" "$BIT_1A4_3")"
            printf '    l2_amp                  bit 5: %s
' "$(bit_state "$misc" "$BIT_1A4_5")"
            ;;
        raptorcove|generic)
            printf '    l2_hw       bit 0: %s
' "$(bit_state "$misc" "$BIT_1A4_0")"
            printf '    l2_adjacent bit 1: %s
' "$(bit_state "$misc" "$BIT_1A4_1")"
            printf '    l1_dcu      bit 2: %s
' "$(bit_state "$misc" "$BIT_1A4_2")"
            printf '    l1_ip       bit 3: %s
' "$(bit_state "$misc" "$BIT_1A4_3")"
            if [[ "$resolved_arch" == "raptorcove" ]]; then
                printf '    l2_amp      bit 5: %s
' "$(bit_state "$misc" "$BIT_1A4_5")"
            fi
            ;;
    esac
}

show_state() {
    local misc="$1"
    local spec="${2:-}"

    printf 'CPU %s
' "$core"
    printf '  arch         : %s
' "$arch_label"
    if [[ -n "$detected_cpu_name" ]]; then
        printf '  detected cpu : %s (family %s model %s)
'             "$detected_cpu_name" "$detected_cpu_family" "$detected_cpu_model"
    fi
    printf '  supported    : %s
' "$(join_by ', ' "${supported_prefetchers[@]}")"
    printf '  MSR_1A4H     = 0x%x
' "$misc"
    show_1a4_state "$misc"

    if (( support_spec_ctrl == 1 )); then
        printf '  MSR_48H      = 0x%x
' "$spec"
        printf '    ddp         bit 8: %s
' "$(ddp_state "$spec")"
        if (( (spec & SPEC_SSBD_MASK) != 0 )); then
            printf '    ssbd        bit 2: set; DDP is also disabled
'
        else
            printf '    ssbd        bit 2: clear
'
        fi
    else
        printf '  MSR_48H      : not used by this profile
'
    fi
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
        --cpu-arch)
            need_arg "$1" "${2:-}"
            cpu_arch="$2"
            shift 2
            ;;
        --keep-ssbd)
            # Backward-compatible no-op: this version never clears SSBD automatically.
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

configure_arch

misc_before="$(read_msr "$MSR_MISC_FEATURE_CONTROL")"
misc_after="$misc_before"
spec_before=""
spec_after=""

if (( support_spec_ctrl == 1 )); then
    spec_before="$(read_msr "$MSR_SPEC_CTRL")"
    spec_after="$spec_before"
fi

case "$action" in
    show)
        show_state "$misc_before" "$spec_before"
        exit 0
        ;;
    all)
        misc_after=$((misc_after & ~known_1a4_mask))
        if (( support_spec_ctrl == 1 )); then
            # Enable DDP by clearing DDPD_U, but do not clear SSBD automatically.
            spec_after=$((spec_after & ~SPEC_DDP_MASK))
        fi
        ;;
    disable_all)
        misc_after=$((misc_after | known_1a4_mask))
        if (( support_spec_ctrl == 1 )); then
            spec_after=$((spec_after | SPEC_DDP_MASK))
        fi
        ;;
    only)
        bit="$(prefetcher_bit "$prefetcher")"
        misc_after=$((misc_after | known_1a4_mask))
        if (( support_spec_ctrl == 1 )); then
            spec_after=$((spec_after | SPEC_DDP_MASK))
        fi

        if [[ "$bit" == "ddp" ]]; then
            spec_after=$((spec_after & ~SPEC_DDP_MASK))
            # Do not clear SSBD automatically.
        else
            misc_after=$((misc_after & ~(1 << bit)))
        fi
        ;;
esac

printf 'Before:
'
show_state "$misc_before" "$spec_before"

if (( misc_after != misc_before )); then
    write_msr "$MSR_MISC_FEATURE_CONTROL" "$misc_after"
fi
if (( support_spec_ctrl == 1 )) && (( spec_after != spec_before )); then
    write_msr "$MSR_SPEC_CTRL" "$spec_after"
fi

misc_verify="$(read_msr "$MSR_MISC_FEATURE_CONTROL")"
spec_verify=""
if (( support_spec_ctrl == 1 )); then
    spec_verify="$(read_msr "$MSR_SPEC_CTRL")"
fi

printf '
After:
'
show_state "$misc_verify" "$spec_verify"

if (( support_spec_ctrl == 1 )) && [[ "$action" == "all" || ( "$action" == "only" && "$prefetcher" == "ddp" ) ]]; then
    if (( (spec_verify & SPEC_SSBD_MASK) != 0 )); then
        printf '
warning: SSBD is still set, so DDP remains disabled even though DDPD_U is clear.
' >&2
    fi
fi
