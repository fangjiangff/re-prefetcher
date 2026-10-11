# BOP EL0/EL1 retention test

This module provides `/dev/bop_el0_el1`. Its ioctl performs exactly one
kernel-side load from a user-mapped address `X` and never loads `X+D`. The
user program then probes `X+D`, so a cache hit cannot be explained by the
kernel trigger directly reading the target.

Build on the target kernel:

```sh
make
make probe
```

Load and run on an isolated CPU as root:

```sh
sudo insmod bop_el0_el1.ko
sudo taskset -c 6 ./test_bop_el0_el1 --case user_trigger \
  --cpu 6 --offset-lines 16 --train-pairs 3000 --rounds 30
sudo taskset -c 6 ./test_bop_el0_el1 --case kernel_trigger \
  --cpu 6 --offset-lines 16 --train-pairs 3000 --rounds 30
sudo taskset -c 6 ./test_bop_el0_el1 --case kernel_train_user_trigger \
  --cpu 6 --offset-lines 16 --train-pairs 300 --kernel-train-pairs 300 --rounds 30
sudo taskset -c 6 ./test_bop_el0_el1 --case untrained_kernel \
  --cpu 6 --offset-lines 16 --train-pairs 3000 --rounds 30
sudo rmmod bop_el0_el1
```

The three independent runs can also be launched and summarized by:

```sh
python3 run_bop_el0_el1.py --arch X925 --output el0-el1.tsv
```

Use `--build` to rebuild the user program first, or `--no-sudo` when the
caller already has the required permissions.

`--arch X925` selects CPU6 and an 80ns hit threshold by default;
`--arch A725` selects CPU4 and a 100ns threshold. `--cpu` and
`--threshold-ns` override those defaults. Before running any case, the runner
checks the selected CPU's MIDR against the requested architecture. The output
TSV records the architecture in every row.

Each process prints one case. Run them as separate processes because
`cpp_rctx()` is not guaranteed to clear the hardware BOP state:

- `user_trigger`: userspace trains and userspace loads `X`;
- `kernel_trigger`: userspace trains, then the ioctl enters EL1 and loads `X`;
- `untrained_kernel`: no userspace training, then EL1 loads `X`.
- `kernel_train_user_trigger`: userspace trains, EL1 trains the same `X→X+D`
  pattern through a separate ioctl, then userspace triggers `X` and probes
  `X+D`.
- `kernel_stride_user_trigger`: userspace trains, then EL1 directly accesses
  `X, X+D, X+2D, ...` with stride `D`, before the EL0 trigger/probe.
- `el0_only_user_trigger`: only EL0 trains the offset before the user trigger;
  use this as the sub-threshold control when `--train-pairs` is small.

Pair training walks 4095 pages through a round-dependent permutation rather
than adjacent pages. Thus the requested `X→X+D` delta is the only repeatedly
small inter-load delta; the transition from one pair to the next is always
outside the ±63-cache-line range. EL0 and EL1 pair training use the same page
order in disjoint halves of the mapping, while the final trigger/probe page is
reserved. Because the per-round page seed is part of the ioctl request, changes
to this interface require rebuilding and reloading `bop_el0_el1.ko`.

The cross-EL result is supported when `kernel_trigger` has a substantially
higher `X+D` hit rate than `untrained_kernel`, while `user_trigger` confirms
that the training pattern itself works. The ioctl validates the userspace
range with `access_ok()` and returns the value read from `X` only for an
end-to-end sanity check.

This is a research module: use a dedicated CPU and do not expose the device
to untrusted users.
