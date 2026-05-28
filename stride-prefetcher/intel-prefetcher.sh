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

COMMON_1A4_MASK=$(( (1 << BIT_L2_HW) | (1 << BIT_L2_ADJ) | (1 << BIT_L1_DCU) | (1 << BIT_L1_IP) ))
AMP_1A4_MASK=$((1 << BIT_L2_AMP))
SPEC_DDP_MASK=$((1 << BIT_DDPD_U))
SPEC_SSBD_MASK=$((1 << BIT_SSBD))

core=""
action=""
prefetcher=""
keep_ssbd=0
cpu_arch="auto"

resolved_arch=""
arch_label=""
detected_cpu_name=""
detected_cpu_family=""
detected_cpu_model=""
support_l2_amp=0
support_ddp=0
support_spec_ctrl=0
known_1a4_mask=0
supported_prefetchers=()

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
  l1_dcu       MSR_1A4H[2] L1 data cache prefetcher / DCU streamer
  l1_ip        MSR_1A4H[3] L1 data cache IP prefetcher
  l2_amp       MSR_1A4H[5] L2 AMP prefetcher (13th Gen profile only)
  ddp          MSR_48H[8]  DDP disable bit (13th Gen profile only)

Options:
  --cpu-arch <name>
      auto            Detect from /proc/cpuinfo using the selected core
      raptorcove|rc   Special handling with l2_amp + ddp
      gracemont|gm    Conservative MSR_1A4H[0:3] handling
      cascadelake|cl  Conservative MSR_1A4H[0:3] handling
      skylake|skx     Conservative MSR_1A4H[0:3] handling
      generic         Conservative MSR_1A4H[0:3] handling
  --keep-ssbd  When enabling ddp, do not clear MSR_48H[2] SSBD.

Notes:
  MSR_1A4H bits are disable bits: 1 disables, 0 enables.
  All non-Raptor Cove architectures use the conservative MSR_1A4H[0:3]
  handling path in this script.

Examples:
  sudo ./intel-prefetcher.sh --core 0 --show
  sudo ./intel-prefetcher.sh --core 0 --all
  sudo ./intel-prefetcher.sh --core 0 --only l1_ip
  sudo ./intel-prefetcher.sh --core 0 --cpu-arch cascadelake --disable-all
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

detect_cpu_signature() {
    awk -F: -v target="$core" '
        function trim(s) {
            sub(/^[ \t]+/, "", s)
            sub(/[ \t]+$/, "", s)
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
    local signature

    case "$cpu_arch" in
        auto)
            signature="$(detect_cpu_signature)"
            [[ -n "$signature" ]] || die "failed to detect CPU signature for core $core"
            IFS='|' read -r detected_cpu_family detected_cpu_model detected_cpu_name <<<"$signature"

            case "${detected_cpu_family}:${detected_cpu_model}" in
                6:183)
                    resolved_arch="raptorcove"
                    ;;
                6:85|6:94)
                    resolved_arch="generic"
                    ;;
                *)
                    resolved_arch="generic"
                    ;;
            esac
            ;;
        raptorcove|rc)
            resolved_arch="raptorcove"
            ;;
        gracemont|gm|cascadelake|cl|skylake|skx|generic)
            resolved_arch="generic"
            ;;
        *)
            die "unknown --cpu-arch '$cpu_arch'"
            ;;
    esac

    case "$resolved_arch" in
        raptorcove)
            arch_label="Raptor Cove profile"
            support_l2_amp=1
            support_ddp=1
            support_spec_ctrl=1
            known_1a4_mask=$((COMMON_1A4_MASK | AMP_1A4_MASK))
            supported_prefetchers=(l2_hw l2_adjacent l1_dcu l1_ip l2_amp ddp)
            ;;
        generic)
            arch_label="Generic MSR_1A4H[0:3] profile"
            known_1a4_mask=$COMMON_1A4_MASK
            supported_prefetchers=(l2_hw l2_adjacent l1_dcu l1_ip)
            ;;
    esac

    if (( keep_ssbd == 1 && support_ddp == 0 )); then
        die "--keep-ssbd is only valid for profiles that expose ddp"
    fi
}

supports_prefetcher() {
    local name="$1"
    case "$name" in
        l2_hw|l2_adjacent|l2_adj|l1_dcu|l1_data|l1_ip|dcu_ip)
            return 0
            ;;
        l2_amp|amp)
            (( support_l2_amp == 1 ))
            return
            ;;
        ddp)
            (( support_ddp == 1 ))
            return
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

    case "$1" in
        l2_hw) echo "$BIT_L2_HW" ;;
        l2_adjacent|l2_adj) echo "$BIT_L2_ADJ" ;;
        l1_dcu|l1_data) echo "$BIT_L1_DCU" ;;
        l1_ip|dcu_ip) echo "$BIT_L1_IP" ;;
        l2_amp|amp) echo "$BIT_L2_AMP" ;;
        ddp) echo "ddp" ;;
        *)
            die "unknown prefetcher '$1'"
            ;;
    esac
}

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
    local spec="${2:-}"

    printf 'CPU %s\n' "$core"
    printf '  arch         : %s\n' "$arch_label"
    if [[ -n "$detected_cpu_name" ]]; then
        printf '  detected cpu : %s (family %s model %s)\n' \
            "$detected_cpu_name" "$detected_cpu_family" "$detected_cpu_model"
    fi
    printf '  supported    : %s\n' "$(join_by ', ' "${supported_prefetchers[@]}")"
    printf '  MSR_1A4H     = 0x%x\n' "$misc"
    printf '    l2_hw       bit 0: %s\n' "$(bit_state "$misc" "$BIT_L2_HW")"
    printf '    l2_adjacent bit 1: %s\n' "$(bit_state "$misc" "$BIT_L2_ADJ")"
    printf '    l1_dcu      bit 2: %s\n' "$(bit_state "$misc" "$BIT_L1_DCU")"
    printf '    l1_ip       bit 3: %s\n' "$(bit_state "$misc" "$BIT_L1_IP")"
    if (( support_l2_amp == 1 )); then
        printf '    l2_amp      bit 5: %s\n' "$(bit_state "$misc" "$BIT_L2_AMP")"
    fi

    if (( support_spec_ctrl == 1 )); then
        printf '  MSR_48H      = 0x%x\n' "$spec"
        printf '    ddp         bit 8: %s\n' "$(ddp_state "$spec")"
        if (( (spec & SPEC_SSBD_MASK) != 0 )); then
            printf '    ssbd        bit 2: set, DDP is also disabled\n'
        else
            printf '    ssbd        bit 2: clear\n'
        fi
    else
        printf '  MSR_48H      : not used by this profile\n'
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
            spec_after=$((spec_after & ~SPEC_DDP_MASK))
            if (( keep_ssbd == 0 )); then
                spec_after=$((spec_after & ~SPEC_SSBD_MASK))
            fi
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
if (( support_spec_ctrl == 1 )) && (( spec_after != spec_before )); then
    write_msr "$MSR_SPEC_CTRL" "$spec_after"
fi

misc_verify="$(read_msr "$MSR_MISC_FEATURE_CONTROL")"
spec_verify=""
if (( support_spec_ctrl == 1 )); then
    spec_verify="$(read_msr "$MSR_SPEC_CTRL")"
fi

printf '\nAfter:\n'
show_state "$misc_verify" "$spec_verify"

if (( support_spec_ctrl == 1 )) && [[ "$action" == "all" || ( "$action" == "only" && "$prefetcher" == "ddp" ) ]]; then
    if (( keep_ssbd == 1 && (spec_verify & SPEC_SSBD_MASK) != 0 )); then
        printf '\nwarning: SSBD is still set, so DDP remains disabled even though DDPD_U is clear.\n' >&2
    fi
fi
