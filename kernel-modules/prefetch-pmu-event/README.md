# Direct EL0 prefetch PMU counters

This research module dedicates two programmable PMUv3 counters on one selected
CPU to prefetch events and permits EL0 to read the counters directly:

| CPU | MIDR part | `PMEVCNTR4_EL0` | `PMEVCNTR5_EL0` |
| --- | --- | --- | --- |
| Cortex-X925 | `0xd85` | `0x8154` L1D hardware prefetch | `0x8155` L2D hardware prefetch |
| Cortex-A725 | `0xd87` | `0x8154` L1D hardware prefetch | `0x8285` L2D hardware/software prefetch fetch |

The module enables the read-only `PMUSERENR_EL0.ER` path and temporarily clears
PMUv3p9 `UEN`, so access is not denied by the fine-grained `PMUACR_EL1` state.
It does not enable broad EL0 PMU write access. Kernel/EL1 events are filtered
out. Counter 4 and counter 5 stay enabled, so a benchmark can bracket only its
trigger with `MRS` instructions and exclude flush, training, cache probing, and
PMU syscalls from the measured delta.

## Build and load

```sh
make
make probe

# Linux must preserve EL0 PMU access across task switches. This is a global
# security setting, so enable it explicitly only on a controlled test system.
sudo sysctl -w kernel.perf_user_access=1

# Cortex-X925, configured as CPU6 in this repository
sudo ./module_load.sh --cpu 6
taskset -c 6 ./probe_prefetch_pmu
sudo ./module_unload.sh

# Cortex-A725, configured as CPU4 in this repository
sudo ./module_load.sh --cpu 4
taskset -c 4 ./probe_prefetch_pmu
sudo ./module_unload.sh
```

The original positional form, `./module_load.sh 6 0`, remains supported for
compatibility.

The module checks MIDR itself; passing CPU4 or CPU6 does not select event
numbers by convention. Unsupported MIDR part numbers are rejected.

By default, loading fails if counter 4 or 5 is already enabled. `force=1` is
available only for controlled recovery/experimentation:

```sh
sudo ./module_load.sh --cpu 6 --force
```

## Benchmark use

Include `prefetch_pmu_event.h` and `prefetch_pmu_ioctl.h`, then pin the process
to the configured CPU. `region-prefetcher/test-exist` calls the restricted
rearm ioctl once when the PMU-only experiment starts, before the first
snapshot; it does not rearm on every round. The ioctl remains outside every
measured delta. A later PMU-overwriting task switch is therefore not repaired
until the next experiment:

```c
int pmu_fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
ioctl(pmu_fd, PREFETCH_PMU_IOC_REARM);

struct prefetch_pmu_snapshot before = prefetch_pmu_read();

trigger(target);
wait_for_prefetch();

struct prefetch_pmu_snapshot after = prefetch_pmu_read();
uint32_t l1_delta = prefetch_pmu_delta(after.l1_hwprf,
					 before.l1_hwprf);
uint32_t l2_delta = prefetch_pmu_delta(after.l2_prefetch,
					 before.l2_prefetch);
```

Perform cache probes only after the second snapshot. The helper computes a
32-bit modular delta, which also works when the implementation has 64-bit
event counters, provided fewer than `2^32` events occur in one measured
window.

## Important limitations

- `kernel.perf_user_access=1` is required on kernels that expose this sysctl.
  It broadens user-space PMU access globally; restore it with
  `sudo sysctl -w kernel.perf_user_access=0` after experiments if that was the
  original setting.
- Linux perf does not reserve counters 4/5 for this out-of-tree module. Do not
  run `perf`, `perf_event_open` workloads, or system PMU monitoring on the
  selected CPU while the module is loaded.
- Pin the benchmark to exactly the configured CPU. PMU event counters are
  per-CPU, not system-wide like `CNTVCT_EL0`.
- Prefer an isolated CPU. EL0 execution by another task between snapshots is
  also counted, although EL1/kernel events are excluded.
- The A725 `0x8285` event includes fetches attributed to hardware prefetch and
  software `PRFM`; do not execute `PRFM` inside the measured trigger window if
  the result is interpreted as hardware-only.
- If the Armv9 `ccr9` cycle-counter module is also needed, load `ccr9` first,
  then this module; unload in the reverse order. Both modify
  `PMUSERENR_EL0`.
- This module changes privileged per-CPU PMU state and is not suitable for
  production or multi-tenant systems.
