# Load/Store Stride Prefetcher Agent

`prefetch_agent.stride_agent` implements a resumable, single-agent campaign for
Cortex-X925 CPU6. It reuses the audited microbenchmarks under
`re0-stride-prefetcher` and keeps raw execution separate from scientific
evidence promotion.

The state machine is:

```text
existence/minimum accesses -> stride range -> degree -> index mode/bit scans
-> entry pressure/replacement -> cross-context lifetime
```

Only Load and Store are considered. PMU experiments use the existing perf
event path and therefore run through `sudo`; cache-only experiments do not.
Planning is the default:

```sh
python3 -m prefetch_agent.stride_agent \
  --output prefetch-agent-results/stride-x925-campaign
```

Execute or resume a campaign:

```sh
python3 -m prefetch_agent.stride_agent \
  --output prefetch-agent-results/stride-x925-campaign --execute

python3 -m prefetch_agent.stride_agent \
  --output prefetch-agent-results/stride-x925-campaign --execute --resume \
  --phases index capacity context
```

`campaign.json` is updated after every command. A failed T0 sanity gate blocks
bit and capacity claims. A bit that changes behavior is described as a
match/index/tag/hash input candidate; the agent does not call it an index bit
without additional evidence. The current PC benchmark provides single-bit but
not pairwise-PC cases, which is recorded as a capability gap rather than
silently inferred.
