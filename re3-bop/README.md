# BOP 测试

该目录用于测试 Best-Offset Prefetcher（BOP）的存在性。实验包含两个 case：

- `untrained`：不训练 BOP，作为基线。
- `fixed_offset_expected`：使用固定的 `+16 cache lines` 偏移进行训练，然后检测相同偏移位置是否被预取。

## 运行环境

- AArch64 Linux
- 支持的核心配置：`A725`、`X925`
- 使用 PMU 时需要 `sudo` 权限，以加载 `prefetch_pmu_event` 内核模块
- 绘图需要 Python `matplotlib`

以下命令均在仓库工作目录下运行。

## 单次运行

在 Cortex-A725 上同时运行 PMU 和 cache probe：

```bash
python3 re-prefetcher/re3-bop/test-bop.py \
    --arch A725 \
    --rounds 100 \
    --train-pairs 1000
```

仅运行 cache probe：

```bash
python3 re-prefetcher/re3-bop/test-bop.py \
    --arch A725 \
    --train-pairs 1000 \
    --no-pmu
```

仅运行 PMU：

```bash
python3 re-prefetcher/re3-bop/test-bop.py \
    --arch A725 \
    --train-pairs 1000 \
    --no-cache-probe
```

## 批量扫描 train-pairs

下面的命令会依次测试 `train-pairs=100,200,...,3000`，并生成汇总表和折线图：

```bash
python3 re-prefetcher/re3-bop/test-bop.py \
    --arch A725 \
    --rounds 100 \
    --sweep-train-pairs
```

默认输出到 `re-prefetcher/re3-bop/res/`：

```text
bop-A725-cpu4-thr120-train-pairs-sweep.tsv
bop-A725-cpu4-thr120-train-pairs-sweep.png
```

折线图包含 cache hits、L1 PMU events 和 L2 PMU events 随 `train-pairs` 的变化。

## 常用参数

- `--arch {A725,X925}`：选择处理器配置。
- `--core N`：覆盖默认绑定的 CPU 编号。
- `--rounds N`：每个 case 的实验重复次数，默认 `100`。
- `--train-pairs N`：每个 round 的 BOP 训练对数量。
- `--trigger-batch N`：每个 round 的 trigger/target 数量，默认 `64`。
- `--threshold-ns N`：cache hit 延迟阈值。
- `--output PATH`：指定结果表路径。
- `--plot PATH`：指定图片路径。
- `--no-plot`：不生成图片。
- `--no-run`：只编译，不运行。

查看所有参数：

```bash
python3 re-prefetcher/re3-bop/test-bop.py --help
```
