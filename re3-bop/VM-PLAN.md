# BOP Cross-VM Implementation Plan

> For agentic workers: execute task by task with verification checkpoints; the user approved inline implementation and testing in this session.

**Goal:** Test whether learned +16 offset survives KVM exit/re-entry and transfer between two independent same-core VMs.

**Architecture:** One pinned host thread runs two independent KVM VM/vCPU objects. Each VM has private 64MiB RAM and the same freestanding EL1 payload. Guest commands prepare, scrub/train, probe, and exit by MMIO; no disks, networking, passthrough, or kernel changes.

**Tech Stack:** AArch64 GNU C/assembler/linker, Linux KVM UAPI, Python unittest/CSV runner.

**Spec:** User-approved design in this conversation: minimal guest first; same-VM positive and exit controls before cross-VM interpretation; bounded memory/CPU and timeout; report in re3-bop/report.md.

## Global Constraints

- CPU6 X925 by default; optional CPU16 replication. Both VMs use the same host thread/core.
- Guest EL1, Normal Write-Back RAM, stage-1 identity mappings, independent stage-2 mappings.
- No prediction restriction between training and probe. Prefault RAM, guest code/stack and stage-2 mappings before rounds.
- No real-time policy, host safety mitigations disabled, device passthrough, persistent VM registration, or additional kernel modules.
- Default cache measurement; PMU is auxiliary and must not borrow the existing module's counters.
- Preserve unrelated workspace files; no commits required for this experiment.

## Task 1: Safe guest execution and lifecycle

Files: `vm-bop/abi.h`, `vm-bop/guest.S`, `vm-bop/guest.c`, `vm-bop/guest.ld`, `vm-bop/Makefile`, `test-bop-vm.c`, `tests/test_bop_vm.py`.

- [ ] Write tests for invalid bounds, case ownership description, and opt-in `--smoke` guest execution. Missing source must fail first.
- [ ] Implement KVM capability checks, private RAM, vCPU initialization, mmap run structure, bounded signal-interruptible KVM_RUN, and explicit cleanup.
- [ ] Use a shared ABI command/result page at guest IPA 0x40020000; MMIO doorbell 0x10000000. Guest reports CurrentEL and SCTLR, and traps exceptions to a diagnostic exit.
- [ ] Compile with warnings as errors; run `sudo timeout 15 ... --smoke`, verify two distinct VM objects execute commands and return, no VM persists after exit.

## Task 2: Validated BOP retention cases

- [ ] Implement random per-page Xi/Xi+D pairs with per-load DC CIVAC, same training PC, separate trigger PC, separate endpoint pool; 64 probes/round.
- [ ] Host stage sequences: untrained, local, same_exit, other_idle_return, cross_probe, cross_no_trigger, peer_local, untrained_peer. Both A→B and B→A are configurable.
- [ ] Prepare both guests first, then scrub+train source last. For local, scrub/train/probe in one guest entry; same_exit separates train/probe; cross_probe probes the other guest without its training.
- [ ] Validate cached/cold latency and guest-local positive before interpreting negative cross-VM results. Start train-pairs=1500, offset=16; use 30 rounds and three seeds for final data.
- [ ] Record probe counts, faults, exits, training count, guest EL/cache enable, context direction and observed physical CPU. Abort on guest exception, timeout or unexpected exit.

## Task 3: Batch testing and report

Files: `test-bop-vm.py`, `re3-bop/res/bop-vm-*`, `README.md`, `report.md`.

- [ ] Runner builds artifacts, randomizes independent case processes, preserves raw data/metadata hashes, aggregates without mixing directions/cases.
- [ ] Run CPU6 suite; repeat CPU16 if guest positive controls are sound. No concurrent hardware experiments.
- [ ] Request read-only implementation review while analyzing results; address substantive findings.
- [ ] Verify row completeness, bounds, artifacts/hashes, original PMU module state, unit tests; report observed retention separately from mechanism inference.
