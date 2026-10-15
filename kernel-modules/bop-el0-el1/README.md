# BOP EL0/EL1 split-training test

This test splits one fixed `X→X+D` training sequence of `N=M+K` pairs
between EL0 and EL1. EL0 trains logical pair indexes `0…M-1`; the module
then trains `M…M+K-1` in the same 8192-page userspace mapping using
`get_user()`. Shared userspace pages are the default build mode. The EL0
trigger and probe remain in that mapping.

Each round performs the following sequence:

1. flush the complete userspace mapping; in optional private-memory mode,
   when `K>0`, also flush the module's complete training pool;
2. execute the M EL0 pairs;
3. for the EL-switch control, enter and return from EL1 without reading a
   training address, then execute the K post-M pairs in EL0;
4. otherwise, execute the K EL1 pairs through the training ioctl;
5. read PMU counters, trigger `X`, wait, read PMU counters again, then probe `X+D` in EL0.

The program permits `K=0` and requires `M+K ≤ 4095`. Both sections use the
same fixed shuffle seed, so all rounds use the same logical pair sequence.
The kernel segment starts at
`first_pair=M`; it does not repeat the first K EL0 pairs. EL0 training, EL1
training, and the final EL0 probe each use one fixed, distinct load PC.

Build on the target kernel:

```sh
make
make probe
```

Load the rebuilt module and run on an isolated CPU as root:

```sh
sudo insmod bop_el0_el1.ko
sudo taskset -c 6 ./test_bop_el0_el1 --case el0_only_user_trigger \
  --cpu 6 --offset-lines 16 --train-pairs 300 --rounds 30
sudo taskset -c 6 ./test_bop_el0_el1 --case split_train_user_trigger \
  --cpu 6 --offset-lines 16 --train-pairs 300 --kernel-train-pairs 300 \
  --rounds 30
sudo rmmod bop_el0_el1
```

The runner launches independent processes and writes a comparison table:

```sh
python3 run_bop_el0_el1.py --arch X925 --output el0-el1.tsv
```

The cases are:

- `untrained_user_trigger`: no training, then EL0 trigger/probe control;
- `el0_only_user_trigger`: M EL0 pairs, then EL0 trigger/probe;
- `el_switch_user_train`: M EL0 pairs, one EL0→EL1→EL0 ioctl with no
  training-address access, then K post-M pairs in EL0 and EL0 trigger/probe;
- `split_train_user_trigger`: M EL0 pairs followed by K EL1
  pairs, then EL0 trigger/probe; `K=0` executes the all-EL0 sequence.

`--arch X925` selects CPU6 and an 80ns hit threshold by default; `--arch
A725` selects CPU4 and 100ns. When `/dev/prefetch_pmu_event` is available, each
case reports the trigger-window L1/L2 hardware-prefetch PMU deltas. The runner validates the selected CPU's MIDR
before every run. Use a dedicated CPU and do not expose `/dev/bop_el0_el1` to
untrusted users.

默认执行 `make` 即构建共享用户页模式，无需额外编译参数。EL1 通过
`get_user()` 访问用户映射中的后 K 对训练地址；每轮由 EL0 flush 整个用户映射。

仅在需要私有页对照实验时显式使用：

```sh
make clean
make EXTRA_CFLAGS=-DBOP_EL0_EL1_SHARED_USER_PAGES=0
```

执行 `make clean && make` 恢复默认共享页模式。切换模式后须重新加载模块。

EL0 与 EL1 均在每次训练 load 后循环执行 100 次 NOP（base 后和 target 后）。
这用于对齐训练访问间隔，不保证硬件内存访问顺序。trigger 后原有的 2000 次 NOP 等待保持不变。

新增 `el0_train_kernel_trigger`：EL0 完成 `--train-pairs` 对训练，
EL1 通过独立 ioctl 对用户映射中的 trigger X 执行一次 `get_user()`，
返回后 EL0 探测 X+D。EL1 不访问 X+D，忽略 `--kernel-train-pairs`。
`untrained_kernel_trigger` 是同一 EL1 trigger 路径的无训练控制。
两者 PMU 窗口包含整个 trigger ioctl 和返回后的等待，不能把事件全部归因于 trigger load。

```sh
python3 run_bop_el0_el1.py --arch X925 --train-pairs 550 --kernel-train-pairs 0 --rounds 100
```
