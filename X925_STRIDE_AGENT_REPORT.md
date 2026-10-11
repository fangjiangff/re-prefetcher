# X925 Load/Store Stride Prefetcher Agent Report

Date: 2026-08-27  
Target: Cortex-X925, Linux CPU 6, AArch64  
Branch: `prefetchAgent`

## Agent implementation

The single-agent campaign is a resumable state machine:

```text
existence/minimum accesses -> stride range -> degree -> index sensitivity
-> entry pressure/replacement -> context lifetime
```

It only tests Load and Store. It invokes the existing C/Python microbenchmarks,
records every command, stdout/stderr, generated artifact hash, and derived
verdict in `campaign.json`. PMU existence uses three repetitions plus an
independent cache-probe gate. Structural conclusions require their positive and
negative controls to pass.

Use the correctness-gated entry point:

```sh
python3 -m prefetch_agent.stride_campaign_v2 \
  --output prefetch-agent-results/stride-agent-x925 \
  --execute
```

Resume selected phases with `--resume --phases index capacity context`.

## X925 observations

All discovery experiments use a stride of three 64-byte cache lines.

| Property | Load | Store |
|---|---:|---:|
| First cache-observed activation | 20 accesses | 3 accesses |
| First PMU majority activation | 20 | 3 |
| First PMU activation in all 3 repetitions | 20 | 5 |
| Conservative follow-up sequence | 22 | 7 |
| PMU-observed maximum stride | at least 4096 lines | 18 lines |
| First unsupported stride | not found | 19 lines |
| Consecutive cache-observed degree | not established | 15 candidates |

The Store PMU result at three accesses is non-stationary
(`0.15, 7.20, 0.97` events/round in this campaign), while the cache probe is
positive. Five accesses are the first point positive in all three PMU runs.
For Load, activation is sharp: access 19 has median 0.04 and access 20 has
median 1.01 events/round.

The Load degree run cached positions 24 and 25 but missed the first expected
candidate, so it does not establish a consecutive degree. The Store run cached
all 15 tested consecutive candidates; 15 is therefore a lower bound imposed by
the probe window, not a proven hardware maximum.

## Index/key evidence

Load passes T0 controls and loses the signal when PC equality (T1) or VA
equality (T2) is broken. It also fails the current PA-related T3/T4 cases. This
supports PC- and VA-sensitive matching candidates, but does not yet isolate a
complete index function.

Two Load PC scans consistently identify bits 3-15 as sensitive. A later scan
also flagged bits 24 and 47, while the earlier scan did not; those two points
are treated as unreplicated noise. The existing PC benchmark lacks two-bit XOR
cases, so no PC hash function is claimed.

Store remains positive across T1, T2, and T4: same PC and same VA are not
required in these cases, and PA sufficiency is a candidate. T3 is negative, but
T3 changes both PA and process context, so it cannot independently prove PA
indexing.

## Entry pressure and replacement

The Store victim remains positive with 0-7 competing streams and first fails
with 8. This is compatible with eight live entries including the victim, but is
not an exact capacity claim until all competitors are validated as distinct
entries/colliders under the recovered key function.

The replacement test is inconclusive: the extra competitor did not reliably
evict the victim, so FIFO versus LRU cannot yet be distinguished. The Load
zero-competitor positive control failed; no Load capacity conclusion is valid.

## Context lifetime

For Store, the positive/negative baselines pass for cross-thread, EL0/EL1, and
cross-core tests. In all three, state neither survives the tested switch nor is
shared with the trigger context/core. The cross-process Store baseline fails,
so that case is inconclusive.

All Load context variants fail at least one baseline and are inconclusive. An
earlier raw summary labeled one Load cross-core case positive solely from its
target latency; the v2 Agent correctly rejects it because the no-trigger
baseline was also a cache hit.

TrustZone was intentionally skipped because OP-TEE availability was not
established.

## Remaining work before a full reverse-engineered model

1. Repeat every PC bit case three times and promote bits by majority/stability,
   then add pairwise XOR over the stable 3-15 range.
2. Run controlled VA and privileged PA one-bit and pairwise scans with validated
   mappings; remove the process-context confound from T3.
3. Construct capacity competitors from the recovered key/hash rather than raw
   page pressure, then retry FIFO/LRU patterns with enough confirmed colliders.
4. Repair the Load degree/context positive sequences before drawing Load entry
   or lifetime conclusions.
5. Add page-contained and page-crossing range variants; the current maximum
   stride phase is PMU-observed and conflates stride magnitude with page effects.

The complete machine-readable campaign contains 67 recorded command runs at
`prefetch-agent-results/stride-agent-x925-20260827/campaign.json`.
