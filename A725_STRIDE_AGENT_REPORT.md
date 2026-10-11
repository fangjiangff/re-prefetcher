# A725 Load/Store Stride Prefetcher Agent Report

Date: 2026-08-27  
Target: Cortex-A725, Linux CPU 4, AArch64  
Branch: `prefetchAgent`

The campaign uses a three-cache-line discovery stride, three PMU repetitions,
the A725 L1 hardware-prefetch event `0x8154`, and a 120 ns cache threshold.
The final structure phases reconcile activation as the maximum of the first
cache observation and first PMU-majority observation.

## Main observations

| Property | Load | Store |
|---|---:|---:|
| First cache observation | 3 accesses | 6 accesses |
| First PMU majority activation | 6 accesses | 6 accesses |
| First PMU activation in all repetitions | 6 accesses | 6 accesses |
| Sequence used for structural tests | 6 accesses | 6 accesses |
| Conservative range/degree sequence | 8 accesses | 8 accesses |
| PMU-observed maximum stride | at least 4096 lines | 32 lines |
| First unsupported stride | not found | 33 lines |
| Consecutive cache-observed degree | 6 | 4 |

Load PMU activation at six accesses is stable (`3.30, 3.21, 3.65`
events/round). Store also first activates at six (`2.56, 2.38, 0.72`). The
early Load cache observation at three has no matching PMU signal and is not
used as the structural trigger sequence.

The Load degree result covers all six consecutive candidates tested by the
effective window. Store covers four consecutive candidates; one later
non-consecutive candidate also hits. These are observed window results, not
proven maximum hardware degrees.

## Index/key evidence

Both Load and Store pass their T0 negative/positive controls with six accesses.
T1 stays positive for both, so equal trigger PC is not required and no PC-bit
scan is warranted by this protocol. T2 is negative for both, making VA equality
a match/index/tag candidate.

Load is negative in T3 and T4. Store is positive in T3 but negative in T4. T3
also changes process context and PA, so this does not establish a complete VA
or PA index function. VA one-bit and pairwise XOR scans remain necessary.

## Entry pressure and replacement

Load remains positive for every tested pressure point from 0 through 64
competitors, so no boundary is observed.

Store first misses at 17 competitors but becomes positive again at later
points, followed by increasingly frequent misses above roughly 35. The curve
is non-monotonic; 17 is not a defensible exact entry count. Competitors must be
constructed from the recovered VA key/hash before associativity can be claimed.

Replacement policy is inconclusive because the filled and baseline positive
controls hit in only one of three repetitions. FIFO/LRU is not determined.

## Context lifetime

For Store, cross-thread and cross-core positive/negative baselines pass. In both
cases the tested state neither survives the switch nor is shared with the other
thread/core. Cross-process and EL0/EL1 Store baselines fail and are
inconclusive.

All Load context variants fail at least one baseline and are inconclusive.
TrustZone is skipped because OP-TEE availability was not established.

## Implementation note

The original A725 trigger sweep requested the obsolete name `l2d_cache_prf`,
while `pmu.h` exposes `l1d_cache_hwprf` and `l2d_cache_hwprf`. The A725 Agent
adapter selects `l1d_cache_hwprf`, matching the A725 index benchmark's verdict
logic, without changing the access sequence or PMU measurement window.

Reproduce or resume with:

```sh
python3 -m prefetch_agent.stride_campaign_a725_v4 \
  --output prefetch-agent-results/stride-agent-a725-20260827 \
  --arch A725 --execute --resume
```

The machine-readable campaign contains 65 recorded commands at
`prefetch-agent-results/stride-agent-a725-20260827/campaign.json`.
