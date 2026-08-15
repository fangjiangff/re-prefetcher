#!/bin/sh

set -eu

MODULE_NAME="prefetch_pmu_event"

if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run with root permissions."
	exit 1
fi

if ! awk -v name="$MODULE_NAME" '$1 == name { found = 1 } END { exit !found }' /proc/modules; then
	echo "$MODULE_NAME is not loaded."
	exit 0
fi

rmmod "$MODULE_NAME"
echo "$MODULE_NAME unloaded; counters 4/5 were restored."
