# BOP 测试

该目录用于测试 Best-Offset Prefetcher（BOP）的存在性。严格模式包含四个 case：

- `untrained`：不训练 BOP，作为基线。
- `random_delta_control`：等量随机配对训练，移除固定 offset 关系。
- `fixed_offset_expected`：使用固定偏移训练，然后检测相同偏移位置。
- `trained_wrong_target`：固定偏移训练后检测相邻的错误目标。

## 运行环境

- AArch64 Linux
- 支持的核心配置：`A725`、`X925`
- 使用 PMU 时需要 `sudo` 权限，以加载 `prefetch_pmu_event` 内核模块
- 绘图需要 Python `matplotlib`

以下命令均在仓库工作目录下运行。

## 单次运行

在 Cortex-A725 的 CPU4 上同时运行 PMU 和 cache probe：

```bash
python3 re3-bop/test-bop.py \
    --arch A725 \
    --core 4 \
    --rounds 100 \
    --train-pairs 1000
```

脚本会把测试进程和 PMU 都绑定到 `--core`。如果
`prefetch_pmu_event` 已加载但绑定在其他 CPU，脚本会先卸载它，再绑定到
CPU4；本次运行结束后会卸载该模块。若模块已经绑定 CPU4，则直接复用，
并保留模块加载状态。

### 低干扰严格训练模式

`test-bop.py` 当前默认使用严格训练模式，目的是减少其他预取器、地址布局和
额外访存对 BOP 结果的干扰：

- 每个训练对的两次 load 都分别执行 `DC CIVAC -> DSB/ISB -> load`。
  因此访问 `X` 后，即使 next-line/spatial prefetcher 提前供应了 `X+D`，
  `X+D` 也会在正式 demand load 前再次失效。
- 每轮不再插入“每7对一次”的额外 noise load；`train-pairs=N` 精确表示
  N 个训练对和 2N 次正式训练 load。
- trigger 从地址池尾部的保留 slot 产生，不再使用
  `train_pairs + noise_count(train_pairs)` 定位。改变训练量不会系统性平移
  trigger slot。
- 每轮只有一个 trigger；训练使用16个轮转 load PC，最终 trigger 使用独立
  load PC。

每次运行包含四个 case：

| case | 训练 | probe | 用途 |
| --- | --- | --- | --- |
| `untrained` | 无 | `+D` | 无训练基线 |
| `random_delta_control` | 相同数量的随机 delta pair | `+D` | 匹配训练访存量、PC、flush 和 barrier，但移除固定 offset 关系 |
| `fixed_offset_expected` | 固定 `+D` | `+D` | 预期目标 |
| `trained_wrong_target` | 固定 `+D` | 另一 offset | 检查 stream/spatial 扇出；当 `D=1` 时 probe `+2` |

`trained_wrong_target` 的 PMU 仍会统计 trigger 后产生的所有硬件预取请求，不能
判断请求落在 `+D` 还是错误目标；该 case 应以 cache probe 是否命中错误目标为主。

随机对照保持与固定训练相同的 mate 页类别和相近的 line 取值范围；日志中
`train_offset_lines=-` 表示“没有固定训练 offset”，不是 `+0` 训练。
当 offset 为 63 或 127 时，可用 base line 只有一个，不可能同时保持相同的
单点 line 分布并移除固定 delta；日志会标为
`random_control_match=partial_singleton_line_range`，这两个点的随机对照只能
视为部分匹配。

`offset=0` 是退化负控制点：trigger 与 target 是同一条 cache line，trigger
demand load 必然使 target 命中，因此不能用于判断预取。offset sweep 将其
标记为 `interpretation=degenerate_same_address`，并把 `observed` 记为 `-`。

程序日志中的下列元数据可用于确认结果来自严格模式：

```text
# strict_training=1 training_flush=each training_noise_loads=0 trigger_slot_policy=fixed_tail
```

旧结果使用 pair 开始前统一 flush、额外 noise load 或多个 trigger 时，应视为
legacy 结果，不能与当前严格模式的训练阈值直接合并。

仅运行 cache probe：

```bash
python3 re3-bop/test-bop.py \
    --arch A725 \
    --train-pairs 1000 \
    --no-pmu
```

仅运行 PMU：

```bash
python3 re3-bop/test-bop.py \
    --arch A725 \
    --train-pairs 1000 \
    --no-cache-probe
```

## 批量扫描 train-pairs

下面的命令会依次测试 `train-pairs=0,100,200,...,3000`，其中 `0` 是完全不执行
训练对的基线，并生成汇总表和折线图：

```bash
python3 re3-bop/test-bop.py \
    --arch A725 \
    --rounds 100 \
    --sweep-train-pairs
```

默认输出到 `re-prefetcher/re3-bop/res/`：

```text
bop-A725-cpu4-thr100-train-pairs-sweep.tsv
bop-A725-cpu4-thr100-train-pairs-sweep.png
```

折线图包含 cache hits、L1 PMU events 和 L2 PMU events 随 `train-pairs` 的变化。

## 批量扫描 offset

### `train-left-offset` 访问形式

使用 `test-bop-left-offset.py` 可在原始训练后比较五种追加训练形式：

```sh
taskset -c 6 python3 test-bop-left-offset.py --cpu 6 \
  --offset-lines 16 --train-pairs 300 --left-pairs 300 \
  --rounds 30 --output res/bop-X925-cpu6-left-offset.tsv
```

`none`、`single_pc_stride`、`multi_pc_stride`、`single_pc_page_pair` 和
`multi_page_pair` 会分别在独立进程中运行。其中 `single_pc_page_pair` 表示
固定同一个 PC，但在多个页面上重复 `Xi -> Xi+offset`。

进一步控制 load 数量、PC、页面数量和 flush 时机的实验使用
`test-bop-learning.py`。它还分别运行训练期预取 PMU、cache probe、训练
load 计时和 demand miss PMU，详细设置及结果见 [`report.md`](report.md)。

```sh
python3 re3-bop/test-bop-learning.py --build --cpu 6 --offset 16 \
  --output re3-bop/res/bop-learning-new-run.tsv
```

此命令从仓库根目录运行，需要 AArch64 GCC 和 perf 原始事件权限。
默认 pass 为 `prefetch,cache,timing,demand,access`，由 Linux perf 管理计数器；
运行前必须卸载占用计数器的 `prefetch_pmu_event` 模块，否则程序拒绝计数。
旧 `--measure pmu` 仍只用于已加载该模块的独立实验，不能与 perf 混用。
仅测试 cache 可加 `--measures cache`。默认 49 个配置、三个 seed，每个
cache 配置每次运行 30 轮 × 64 probes，输出 `hit_pct` 为百分比。
可用 `--cases stride_batch,stride_each` 缩小范围；再加 `--probe-scan`
即可在独立进程中扫描 `+1..+32`。输出路径已存在时会拒绝覆盖。

研究 L1 miss/L2 hit 和 prefetched hit 使用 `--suite residency`（66 个配置）：

```sh
python3 re3-bop/test-bop-learning.py --build --suite residency --cpu 6 \
  --cases cold,demand,swl2,hw-offset,hw-offset_skip,hw-offset-consumed \
  --measures cache,demand,access --seed 20260913 \
  --output re3-bop/res/bop-residency-new-run.tsv
```

底层仍为 `test-bop-learning.c`。`--resident demand/swl1/swl2` 随机预热目标，
`--evict-kb 256` 用独立随机链驱逐 L1；`hw-stream` 用目标前八条 line 的访问
尝试供应目标，`hw-offset` 先训练 +8，再访问 `T-8` 供应目标 T。
`hw-offset-consumed` 在正式训练前额外随机读取目标一遍；
`hw-offset-demand` 保留 +8 初始训练，但以 demand load 供应目标。
`--skip-b` 保留准备过程、跳过追加训练；`--b-limit N` 只执行前 N 次追加 load；
`--shuffle-b` 打乱追加访问顺序。所有驻留状态都必须结合实际 PMU 结果判断。
`access` 的 `event0/event1` 为 0x8148/0x814C；`prefetch` 为 0x8154/0x8155，
均统计阶段 B。`supply` 使用后一组事件，但只统计准备阶段。
原始 `event*_count` 是总数，`event*_per_load` 按执行 load 数归一化；
`supply` 按准备目标数归一化，不能解释为每条实际准备指令的事件率。

### 跨核心训练（全部在 EL0）

`test-bop-cross-core.c` 使用两个固定绑核的 pthread：A 训练 N 对，B 训练
M 对，再由 A 在全新地址上 trigger/probe。线程不在核心之间迁移，也不调用
EL0/EL1 训练模块。CPU6/7 同属8MB L3组，CPU6/16 位于不同L3组；本机
X925 为 CPU5～9、CPU15～19，每核有独立2MB L2。

```sh
python3 re3-bop/test-bop-cross-core.py --build --directions 6:7,6:16 \
  --train-a-pairs 300 --train-b-pairs 300 --measures cache \
  --output re3-bop/res/bop-cross-core-new-run.tsv
```

默认方向为 `6:7,7:6,6:16,16:6`；默认 M 为 `300,1500,3000`。各方向分别
执行 `untrained/local_n/local_nm/remote_same/remote_other/remote_only/no_trigger`。
本地阳性 `local_nm` 的 N+M 对都由 A 执行，B 仅参与同步与等待。
每个 case 独立进程，三个 seed，每个进程30轮，每轮64个 probe。

两核先分别执行 `CPP RCTX` 和默认4096次无关冷访问，再进入正式阶段；
不假设这些操作能保证硬件状态完全复位，仍需检查未训练与本地不足对照。
阶段 A 结束后的 B 阶段默认至少保持2000us，各 case 使用相同的等待窗口。
`remote_overruns/local_overruns` 记录训练超过该窗口的次数，比较前应检查它们。
`--hold-us` 和 `--scrub-loads` 可调整这些参数。

需要 PMU 验证时使用 `--measures prefetch,cache,demand`，先协调卸载
`prefetch_pmu_event`，结束后恢复；程序不会自动更改模块状态。两线程各自在
自己的训练窗口计数，`demand` 的 event0/event1 为 0x8144/0x814C，
`prefetch` 为 0x8154/0x8155；cache pass 不插入 PMU syscall。
原始 TSV 保存实际绑核、各核训练 load 数、计数与耗时；JSON 保存缓存拓扑、
源文件和二进制哈希。结果解释见 [`report.md`](report.md)。

### 同核跨线程 / 跨进程训练（全部在 EL0）

`test-bop-cross-thread.c` 和 `test-bop-cross-process.c` 共用
`bop-cross-context.h` 的训练、同步和测量逻辑。两边固定在选定的 X925/A725 核心；
前者为同进程的两个 pthread，后者为 fork 后的两个不同地址空间。
每个执行方独立分配并写入自己的私有训练/探测内存，仅控制信息与最终统计共享。
fork 保留相同代码地址，因此此版本没有混入不同 PC/独立 exec 的影响。

```sh
python3 re3-bop/test-bop-cross-context.py --build --cpu 6 \
  --train-a-pairs 300 --train-b-pairs 300 --full-pairs 1500 \
  --measures cache
```

可用 `--arch X925`（默认）或 `--arch A725` 选择处理器；对应默认CPU/命中阈值为
CPU6/80ns和CPU4/100ns，`--cpu`、`--threshold-ns` 可覆盖默认值。未指定
`--output` 时，结果写入
`re3-bop/res/bop-{arch}-cpu{cpu}-off{offset}-cross-context.tsv`；若已存在则自动
追加 `-run2`、`-run3`。显式指定的输出文件不会被覆盖。

默认两种模式、15个 case、3个 seed，每次30轮，每轮64个 probe。通过
`--modes thread` 或 `--modes process` 单独选择；`--cases` 选择逗号分隔的 case。
`--probe-batch` 可把每轮的trigger/target数量设为1～64，默认64；设为1时每轮只
执行一个trigger并探测一个对应target。

| 关键case | 正式训练与探测顺序 |
| --- | --- |
| `local_n` / `local_nm` | A训练N / N+M，A直接探测 |
| `switch_none` / `switch_local_nm` | A训练N / N+M，B不训练，再回A探测 |
| `split_same` / `split_other` | A训练N，B追加同D / 另一D的M对，回A探测 |
| `b_only` | A不训练，B训练M，回A探测 |
| `split_peer` | A训练N，B追加M，直接在B探测 |
| `retain_local` / `retain_switch` | A充分训练，直接探测 / 经B空转后回A探测 |
| `retain_peer` / `peer_local` | A充分训练后B探测 / B自行充分训练并探测 |
| `untrained` / `untrained_peer` | 不训练，在A / B探测 |
| `no_trigger` | A+B训练，但只测target、不访问trigger |

每个执行方使用一个4096-page训练池和独立的256-page trigger/probe区；`local_nm`
在同一训练池中一次生成N+M对。每轮B先准备地址并阻塞，A执行一次 `CPP RCTX`
后立即正式训练；不再执行额外的scrub访问。从A训练开始到最终probe之间不再执行
`CPP RCTX`。阻塞pipe交接迫使同核两个任务轮流运行。
普通调度/同步系统调用仍存在，但没有EL1训练。`handoff_us` 包含B的工作与
往返交接，若在B探测还包含probe，不能解释成纯上下文切换耗时。

每个训练对与 `test-bop.c` 使用相同的flush顺序：先依次flush X和Y并完成屏障，
再访问X、Y；X访问触发的Y预取不会在Y访问前被再次flush。

PMU单独运行：先协调卸载 `prefetch_pmu_event`，再使用
`--measures prefetch,cache,demand`；结束后用
`sudo kernel-modules/prefetch-pmu-event/module_load.sh --cpu 6` 恢复原配置。
两个执行方各自用per-thread perf group测训练，事件与跨核测试相同；cache pass
不启用这些计数器。脚本记录实际PID/TID/CPU、训练量、缺页数、源文件及二进制SHA256。
发现测量区间缺页会保留原始行并停止，不生成正常汇总；需先排查再重测。

单个C程序也可直接运行，例如：

```sh
./re3-bop/test-bop-cross-process --cpu 6 --case split_same \
  --train-a-pairs 300 --train-b-pairs 300 --rounds 30
```

逻辑/编译测试：`python3 -m unittest tests.test_bop_cross_context`。
仅在本机X925 CPU6上启用实际硬件测试及256种fork栈布局回归：
`BOP_HARDWARE_TESTS=1 python3 -m unittest tests.test_bop_cross_context`。
实测结论和限制见 [`report.md`](report.md)。

下面的命令会固定 `train-pairs=1000`，依次测试 `offset=0,1,...,128`：

```bash
python3 re3-bop/test-bop.py \
    --arch X925 \
    --rounds 100 \
    --train-pairs 1000 \
    --trigger-batch 1 \
    --sweep-offsets \
    --no-pmu \
    --no-plot
```

默认输出到 `re3-bop/res/`：

```text
bop-X925-cpu6-thr80-pairs1000-offset-sweep.tsv
```

## 常用参数

- `--arch {A725,X925}`：选择处理器配置。
- `--core N`：覆盖默认绑定的 CPU 编号。
- `--rounds N`：每个 case 的实验重复次数，默认 `100`。
- `--train-pairs N`：每个 round 的 BOP 训练对数量。
- `--offset N` / `--offset-lines N`：指定 BOP 训练和 probe 的偏移，单位为 cache line，默认 `16`。
- `--sweep-offsets`：扫描 `offset=0..128` 并生成汇总表。
- `--trigger-batch N`：每个 round 的 trigger/target 位置数量，默认 `1`；
  每个 round 会换用新的地址。仅在需要多位置实验时显式指定更大的值。
- `--pre-trigger-action {none,syscall,yield,nanosleep,cpp_rctx,process}`：训练和 trigger 准备完成后，在 trigger load 之前执行的动作，用于测试进入内核、调度、显式 `cpp_rctx` 或切到预创建子进程后 BOP 状态是否保留。
- `--threshold-ns N`：cache hit 延迟阈值；A725 默认 `100 ns`，X925 默认 `80 ns`。
- `--output PATH`：指定结果表路径。
- `--plot PATH`：指定图片路径。
- `--no-plot`：不生成图片。
- `--no-run`：只编译，不运行。

查看所有参数：

```bash
python3 re3-bop/test-bop.py --help
```

## 已有 best offset 被新 offset 替换

`test-bop-transition.py` 用两个互质 offset 做顺序训练。默认先用 3000 个
`+13` 地址对建立旧 best offset，再把 `+16` 的训练对数从 0 扫描到 1200。
每个扫描点和每次 repeat 都在独立进程中重新训练 `+13`，以覆盖前一实验
可能残留的核级 BOP 状态；进程切换本身不视为能够清除该状态。

```bash
python3 re-prefetcher/re3-bop/test-bop-transition.py \
    --arch X925 \
    --offset-a 13 \
    --offset-b 16 \
    --train-a-pairs 3000 \
    --coarse-start 0 \
    --coarse-stop 1200 \
    --coarse-step 100 \
    --fine-step 20 \
    --repeats 5 \
    --rounds 100
```

使用紧凑链式训练时增加 `--training-mode compact`；默认的
`--training-mode paired` 保持原有的每对两个 load 方式。
在 `+13/+16` 下，每页链分别承载 4/3 个 pair；以 3000 对为例，compact
训练约需 3601/3751 次训练 load，paired 约需 6000 次，实际可减少约
35%～40%。由于每页都会重新开始链，compact 结果应与 paired 结果对照，
不能视为完全等价的训练证据。

脚本先进行步长 100 的粗扫，再在观测到的转换区间中以步长 20 细扫，并生成
两个文件：

- `*-transition.tsv`：每次独立运行的原始数据和随机 seed。
- `*-transition-summary.tsv`：每个训练对数的聚合结果和三个边界。

为避免 probe 一个目标时通过其他空间预取机制影响另一个目标，底层程序使用
隔离 probe：每个 round 都重新完成 A→B 训练，但奇偶 round 分别只 probe A
或只 probe B。`--rounds` 因此必须是大于等于 2 的偶数。

边界含义如下：

- `crossover_pairs`：`+16` 的平均命中率首次超过 `+13`。
- `dominant_pairs`：该点的 5 次运行中，`+16` 命中率每次都高于 `+13`。
- `stable_replacement_pairs`：5 次运行均满足 `+16 >= 900/1000` 且
  `+13 <= 250/1000`，并且后续两个更大的测试点也满足同一条件。

汇总元数据中的 `old_best_established` 还要求在 `B=0` 时 5 次运行均满足
`+13 >= 900/1000` 且 `+16 <= 250/1000`。若这个前提不成立，脚本不会报告
任何替换边界，应先调整 cache-hit 阈值或增加 A 的训练量。

这些边界来自目标地址的 cache probe，是对实现行为的操作性推断，不是读取
处理器内部的 best-offset 寄存器。用 `--describe` 可只查看粗扫计划；用
`--no-run` 可只编译底层测试程序。

## BOP 条件训练（bop-cc）

`bop-cc.c` 基于 `test-bop.c` 的训练和 PMU 读取逻辑；每个 round 使用
可复现随机 bit 决定执行固定 offset 训练（bit=1）或跳过训练（bit=0），
随后使用独立 trigger PC 在训练区之后的新页面上探测目标地址。

```bash
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra \
    -march=armv8.5-a+predres -DENABLE_CPP_RCTX=1 \
    -DENABLE_PMU=1 -DENABLE_CACHE_PROBE=0 \
    re3-bop/bop-cc.c re0-stride-prefetcher/until.c -o bin/bop-cc
taskset -c 6 bin/bop-cc 100 80 1024 1 16 123
```

程序内部只根据每轮 L2 PMU event delta 推断条件：`L2 <= 0.2` 记为
`untrained`，`L2 > 0.2` 记为 `trained`，不再逐轮打印日志；随后将推断 bit
与实际随机 bit 比较并报告 `error_rate`。参数顺序为：
`rounds threshold_ns train_pairs trigger_batch` 以及可选的
`offset_lines random_seed`；`trigger_batch` 必须为 1。

程序把每个 round 抽象为 1 bit，统计整个 rounds 区间的
`bandwidth_bit_s/kbit_s`：`rounds / elapsed_seconds`。L2 PMU 只用于
判断该 round 是否触发预取，不参与带宽单位换算。

## 1..32 offset 推断（bop-cc-4bit）

`bop-cc-4bit.c` 每个 round 随机选择 `1..32` 的 offset，使用该 offset
训练 BOP；trigger 后在同一新页面的 `+1..+32` 位置进行 cache probe。
32 个目标的探测顺序每轮随机打乱，最后用最低 reload 延迟对应的 offset
作为推断值，并报告实际随机值与推断值之间的 `error_rate`。虽然文件名沿用
`4bit`，`1..32` 实际需要 5 bit 表示。

```bash
aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra \
    -march=armv8.5-a+predres -DENABLE_CPP_RCTX=1 \
    -DENABLE_PMU=0 -DENABLE_CACHE_PROBE=1 \
    re3-bop/bop-cc-4bit.c re0-stride-prefetcher/until.c -o bin/bop-cc-4bit
taskset -c 6 bin/bop-cc-4bit 1000 80 1024 123
```

也可以使用辅助运行器自动编译、绑定 CPU 并保存原始日志和 error-rate 汇总：

```bash
python3 re3-bop/bop-cc-4bit.py --arch X925 --rounds 1000 \
    --train-pairs 1024 --seed 123
```
