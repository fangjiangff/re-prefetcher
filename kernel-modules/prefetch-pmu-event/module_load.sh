#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODULE_PATH="$SCRIPT_DIR/prefetch_pmu_event.ko"
MODULE_NAME="prefetch_pmu_event"
TARGET_CPU=""
FORCE=0

usage()
{
	echo "Usage: sudo $0 --cpu <target_cpu> [--force]"
	echo "       sudo $0 <target_cpu> [force=0|1]"
}

if [ "$#" -gt 0 ] && [ "${1#--}" = "$1" ]; then
	TARGET_CPU="$1"
	FORCE="${2:-0}"
	if [ "$#" -gt 2 ]; then
		usage
		exit 2
	fi
else
	while [ "$#" -gt 0 ]; do
		case "$1" in
		--cpu)
			if [ "$#" -lt 2 ]; then
				echo "--cpu requires a value"
				usage
				exit 2
			fi
			TARGET_CPU="$2"
			shift 2
			;;
		--force)
			FORCE=1
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1"
			usage
			exit 2
			;;
		esac
	done
fi

if [ -z "$TARGET_CPU" ]; then
	echo "--cpu is required"
	usage
	exit 2
fi

case "$TARGET_CPU" in
*[!0-9]*)
	echo "Invalid CPU number: $TARGET_CPU"
	exit 2
	;;
esac

case "$FORCE" in
0|1) ;;
*)
	echo "force must be 0 or 1"
	exit 2
	;;
esac

if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run with root permissions."
	exit 1
fi

if [ -r /proc/sys/kernel/perf_user_access ] &&
   [ "$(cat /proc/sys/kernel/perf_user_access)" != "1" ]; then
	echo "EL0 direct PMU reads are disabled by kernel.perf_user_access."
	echo "Explicitly enable the global setting first:"
	echo "  sudo sysctl -w kernel.perf_user_access=1"
	echo "Warning: this broadens PMU access for user processes on the system."
	exit 1
fi

if [ ! -f "$MODULE_PATH" ]; then
	echo "Module not found: $MODULE_PATH"
	echo "Compile it first with: make -C $SCRIPT_DIR"
	exit 1
fi

if awk -v name="$MODULE_NAME" '$1 == name { found = 1 } END { exit !found }' /proc/modules; then
	echo "$MODULE_NAME is already loaded; no changes made."
	exit 0
fi

echo "Loading $MODULE_PATH for CPU$TARGET_CPU (force=$FORCE)"
insmod "$MODULE_PATH" target_cpu="$TARGET_CPU" force="$FORCE"

DEVICE_PATH=/dev/prefetch_pmu_event
if [ -e "$DEVICE_PATH" ] && [ -n "${SUDO_UID:-}" ]; then
	chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$DEVICE_PATH"
	chmod 0600 "$DEVICE_PATH"
	echo "$DEVICE_PATH is accessible only to the user that loaded the module."
fi

echo "$MODULE_NAME loaded. Do not run perf on CPU$TARGET_CPU."
echo "Test EL0 reads with: taskset -c $TARGET_CPU $SCRIPT_DIR/probe_prefetch_pmu"
