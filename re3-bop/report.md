# Best-Offset Prefetcher 训练阈值与 offset 范围测试报告

## 结论

在 Cortex-X925 的 CPU 6 上，使用 `+16 cache lines` 作为训练 offset 时，没有观察到一个完全无噪声的单点阈值，但可以得到以下实用边界：

| 判定标准 | 所需训练量 |
| --- | ---: |
| 开始明显触发预取 | 约 450～500 对训练访问 |
| 预取命中率达到 50% | 约 535 对训练访问 |
| 预取命中率超过 90% | 约 600 对训练访问 |
| 接近饱和（约 99%） | 约 700 对训练访问 |

当前处理器实测能够学习的最大正向 offset 是：

```text
+32 cache lines = +2048 bytes
```

`+32` 的预取命中率接近 100%，而从 `+33` 开始命中率立即下降到未训练基线。这个结果说明 `+32/+33` 之间存在清晰的硬边界。

需要注意，“最大 offset 值”和“硬件内部候选 offset 数量”不是同一个概念。当前实验确认 `+1～+32` 均可学习，即至少支持 32 个正向 offset 值；但仅凭该实验无法确定硬件内部一共有多少个正负候选槽位，也无法证明候选的内部保存方式。后续的双 offset 竞争测试表明，两个预取目标可以在 cache 可观测层面同时有效，因此不能把硬件行为简化为一个可直接观测的单一 best-offset 寄存器。

## 实验环境

- 处理器核心：Cortex-X925，CPU 6
- cache line 大小：64 bytes
- cache hit 判定阈值：80 ns
- 测试轮数：100
- 每轮 trigger 数量：64
- 默认测试 offset：`+16 cache lines`
- 测试程序：[`test-bop.c`](test-bop.c)
- 驱动脚本：[`test-bop.py`](test-bop.py)

一次“训练对”先访问地址 `X`，再访问地址 `X+offset`。因此，535 对训练包含约 1070 次相关 load。为了避免候选测试与训练序列发生固定相位关系，程序还会每 7 对插入一次噪声访问。

## 训练次数扫描

原始粗粒度扫描结果如下：

| train pairs | fixed-offset hits/1000 | 现象 |
| ---: | ---: | --- |
| 400 | 10 | 基本没有预取 |
| 500 | 210 | 已经开始触发 |
| 600 | 910 | 稳定触发 |
| 700 | 995 | 接近饱和 |
| 800 | 1000 | 饱和 |

原始数据保存在 [`res/bop-X925-cpu6-thr80-train-pairs-sweep.tsv`](res/bop-X925-cpu6-thr80-train-pairs-sweep.tsv)。

为了定位 50% 临界点，又对 530～550 对训练进行了步长为 2、每个点重复 5 次的细粒度扫描：

| train pairs | 平均 hits/1000 | 最小值 | 最大值 |
| ---: | ---: | ---: | ---: |
| 530 | 467.6 | 400 | 550 |
| 532 | 467.6 | 450 | 490 |
| 534 | 493.2 | 450 | 549 |
| 536 | 539.6 | 511 | 589 |
| 538 | 535.2 | 499 | 599 |
| 540 | 545.4 | 509 | 599 |
| 542 | 576.8 | 548 | 599 |
| 544 | 583.4 | 499 | 648 |
| 546 | 575.2 | 490 | 659 |
| 548 | 601.4 | 550 | 649 |
| 550 | 682.8 | 629 | 729 |

`534` 对时平均命中率为 49.32%，`536` 对时为 53.96%，所以按“50% 的 probe 被预取”定义，临界训练量约为 **535 对**。

该数值只适用于本实验中的 `+16`。offset 会影响收敛速度：现有扫描中 `+1`、`+7`、`+8` 大约需要 1000～1100 对训练才能接近饱和。因此，不应把 535 对解释成所有 offset 共用的固定硬件计数器阈值。

## 各 offset 的最少训练次数（直接测试）

为避免把不同 offset 的收敛速度混为一个阈值，使用 `test-bop.c` 独立训练每个 `+1…+32`。每个点均为 100 轮 × 64 个 endpoint probe，hit 阈值为 80 ns。先以 100 对粗扫，再以 20 对细扫；本节的“最少”严格指**20 对网格中，第一个满足五次中至少四次达到 900 hits/1000、且五次平均也不低于 900 的点**，不是硬件精确到单一训练对的内部阈值。

| offset | 最少训练对 | offset | 最少训练对 | offset | 最少训练对 | offset | 最少训练对 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| +1 | 880 | +9 | 820 | +17 | 580 | +25 | 580 |
| +2 | 900 | +10 | 820 | +18 | 580 | +26 | 580 |
| +3 | 860 | +11 | 800 | +19 | 620 | +27 | 600 |
| +4 | 880 | +12 | 760 | +20 | 580 | +28 | 580 |
| +5 | 860 | +13 | 740 | +21 | 600 | +29 | 580 |
| +6 | 860 | +14 | 720 | +22 | 600 | +30 | 620 |
| +7 | 860 | +15 | 700 | +23 | 600 | +31 | 580 |
| +8 | 840 | +16 | 620 | +24 | 580 | +32 | 680 |

因此，在这台 X925 / CPU 6 上，`+2` 最慢（本测量为 900 对），`+17、+18、+20、+24～+26、+28、+29、+31` 最快（580 对）。距离较小并不必然收敛更快，说明这里测到的是硬件候选选择与训练流相互作用的结果，不能简化成“offset 越小越容易学”。五次原始读数在 [`res/bop-X925-cpu6-min-training-by-offset.tsv`](res/bop-X925-cpu6-min-training-by-offset.tsv)。

## 多 offset 竞争、改变与失效（直接测试）

新增 [`test-bop-competition.c`](test-bop-competition.c)，其中每个训练/测量 round 都先执行 `cpp_rctx()`，再在随机且不重叠的页面上完成训练；同一个 trigger 页同时测量 `+8` 和 `+16` 两个目标。测量时总是先 probe 较大的 `+16`，防止 probe 顺序额外构造 `+16→+8` 的训练对。

首先把两种 offset 交错训练：仅训练 `+8` 时，`+8/+16` 命中为 `997.34/0.78` hits/1000；仅训练 `+16` 时为 `0.31/994.53`。均衡交错 1500:1500 时为 `840.62/959.53`（A 先）和 `849.38/920.94`（B 先），大多数 round 两个目标均 active。也就是说，此实验不能支持“硬件在任何时刻只能对一个 offset 预取”的说法；至少在 cache 可观测层面，两个 offset 可以同时有效。将比例改为 3:1 后，优势 offset 基本完全主导：`+8:+16=2250:750` 得 `997.34/0.16`，反向 `750:2250` 得 `19.38/998.44`。

随后进行顺序替换实验：每轮固定先训练 3000 对 `A=+8`，再训练 N 对 `B=+16`，二者使用不重叠页面。它回答“旧 offset 何时改变”，而不是把两者同时混合。5 次重复的关键结果如下：

| 后续 B 训练对 N | A=+8（五次范围，hits/1000） | B=+16（五次范围，hits/1000） | 判定 |
| ---: | ---: | ---: | --- |
| 420 | 958～994 | 75～134 | A 保持 |
| 480 | 783～832 | 289～345 | 过渡开始 |
| 500 | 602～682 | 418～586 | 强竞争区 |
| 520 | 366～444 | 662～800 | B 已占优势、未稳定替换 |
| 560 | 184～371 | 835～942 | 接近替换，仍有波动 |
| 580 | 149～206 | 928～967 | B 稳定替换 A |

按保守判据“5 次都满足 B ≥ 900 且 A ≤ 250 hits/1000”，在本实验中，已经饱和的 `+8` 被连续的 `+16` 证据稳定替换需要 **580 对**。`420` 时旧 offset 仍稳定，`480～560` 是两个 offset 同时或交替可观测的过渡带。因此不存在一个无噪声的单次切换点；报告的 580 是当前分辨率与判据下的稳定上界。原始五次数据在 [`res/bop-X925-cpu6-off8-to-off16-transition.tsv`](res/bop-X925-cpu6-off8-to-off16-transition.tsv)。

“失效”则使用下节的无关 trigger 实验定义：不再为旧 offset 提供成对证据、也不读取其预测目标时，`+16` 约在 960 个无关 trigger 后开始明显衰减，3072 个后基本失效。它与顺序替换是两个机制：前者是自然遗忘/重学习，后者是被相反的结构化证据覆盖。

## 同时可保持多少个 offset

前面的双 offset 结果还不能回答容量问题，因此新增 [`test-bop-capacity.c`](test-bop-capacity.c)。它允许一次传入最多 32 个 offset；每个候选使用相同数量的交错训练对，同一批 trigger 页同时生成所有目标，probe 按 offset 从大到小执行。每个候选在一轮中命中率至少 75% 才算 active；原始数据见 [`res/bop-X925-cpu6-capacity.tsv`](res/bop-X925-cpu6-capacity.tsv)。

容量判定采用更严格的整体命中标准：所有候选的总命中率都必须达到 900/1000。结果如下：

| 同时训练候选数 | 训练对/候选 | 代表 offset | 各候选命中率（hits/1000） | 严格结论 |
| ---: | ---: | --- | --- | --- |
| 1 | 3000 | `+4` | 998 | 1 个稳定 |
| 2 | 3000 | `+4,+8` | 727, 718 | 不能同时达到 90% |
| 2 | 3000 | `+8,+16` | 575, 874 | 不能同时达到 90% |
| 3 | 3000 | `+4,+8,+16` | 50, 44, 92 | 不能同时保持 |
| 3 | 3000 | `+4,+12,+20` | 40, 50, 129 | 不能同时保持 |
| 4～32 | 700 | 多组组合 | 至少一个候选接近 0，其余受竞争影响 | 不能同时保持 |

在本实验定义下，X925 能稳定同时维持的**有效 best offset 数量为 1 个**。2 个候选并不是完全没有预取，而是进入竞争态：不同 offset 组合和历史状态下会出现两个候选各有部分命中，但没有观察到两个候选都稳定达到 90%。3 个及以上时，候选之间的竞争更加明显，全部 active 的轮数为 0。

这里的“1 个”是从 cache probe 观察到的有效预取行为，不等价于硬件内部一定只有 1 个 score 槽位。硬件可能保存多个候选或正在评分，但当前测试表明同一访问流下只有一个 offset 能稳定作为 best offset 输出。要精确测内部候选槽位，仍需要硬件 trace 或可读的实现状态寄存器。

## `train-left-offset` 访问形式实验

为了判断 BOP 学习的究竟是“固定 stride”还是“多个页面上的 `X→X+D` 训练对”，新增 [`test-bop-left-offset.c`](test-bop-left-offset.c) 和 [`test-bop-left-offset.py`](test-bop-left-offset.py)。每轮先用原有多 PC、多页面方式训练 300 对，再追加 300 对阶段 B 训练，最后用 64 个新 trigger probe `X+D`。四种阶段 B 分别为：不追加训练、单 PC stride、多 PC stride、多页面 pair。

X925 CPU 6 上三次重复结果如下（每个点 1920 probes）：

| 阶段 B 模式 | 第 1 次 | 第 2 次 | 第 3 次 | 结论 |
| --- | ---: | ---: | ---: | --- |
| `none` | 0.31% | 0.05% | 0.16% | 300 对原始训练不足 |
| `single_pc_stride` | 0.31% | 0.16% | 0.26% | 此配置下单 PC stride 的追加效果很弱 |
| `multi_pc_stride` | 62.45% | 23.02% | 16.67% | 多 PC stride 能提供部分训练证据，但波动较大 |
| `multi_page_pair` | 95.78% | 95.99% | 96.09% | 多页面 `X→X+D`、多 PC 方式稳定有效 |

原始数据保存在 [`res/bop-X925-cpu6-left-offset-repeats.tsv`](res/bop-X925-cpu6-left-offset-repeats.tsv)。注意：原始列是 `hits_per_1000`，换算百分比必须除以 10；此前低命中率的百分比误写已更正。旧代码的 stride 阶段 B 只有 300 次 load，而 pair 有 600 次；两者的 flush 时机、屏障也不同。因此这些结果不能证明“BOP 要求跨页面”或“必须多 PC”。下面的新实验控制这些变量。

### 固定单个 PC 的 `Xi→Xi+D`

上面的 `single_pc_stride` 是同一个 PC 连续访问 `X, X+D, X+2D...`，并不是“同一个 PC 在多个页面上执行多个 `Xi→Xi+D`”。为区分这两种情况，增加 `single_pc_page_pair`：阶段 B 中始终使用同一个训练 PC，但在不同页面上重复 `Xi→Xi+D`；其余参数仍为原始训练 300 对、阶段 B 300 对、30 轮、每轮 64 个 trigger。

X925 CPU 6 的三次独立重复（每点 1920 probes）为：

| 阶段 B 模式 | 第 1 次 | 第 2 次 | 第 3 次 |
| --- | ---: | ---: | ---: |
| `single_pc_page_pair` | 92.45% | 96.15% | 95.94% |
| `multi_page_pair`（同期对照） | 95.94% | 95.78% | 96.04% |

这些结果说明，在已经有 300 对多 PC 初始训练的前提下，单 PC pair 可以完成追加训练；它们没有证明从零开始只使用一个 PC 的训练阈值，也没有证明跨页面是必要条件。单次原始结果保存在 [`res/bop-X925-cpu6-left-offset-single-pc.tsv`](res/bop-X925-cpu6-left-offset-single-pc.tsv)，三次重复保存在 [`res/bop-X925-cpu6-single-pc-page-pair-repeats.tsv`](res/bop-X925-cpu6-single-pc-page-pair-repeats.tsv)。

## 学习模式与其他预取器的相互影响（进一步对照）

新增 [`test-bop-learning.c`](test-bop-learning.c) 和 [`test-bop-learning.py`](test-bop-learning.py)。本节使用最终版本的真实运行数据，修正前文“必须跨页面”和“单 PC stride 无效”的过度推断。

### 问题与控制变量

需要区分三个假说：

1. BOP 的学习必须跨页面，或必须由多个 PC 提供证据。
2. 其他预取活动使后续 load 在较近的 cache 命中，减少能送达 BOP 的合格训练事件。
3. BOP 已经学习，但选择了其他 offset，或者预测请求没有在探测目标上体现出来。

实验固定 X925 CPU6、4KB 页面、64B cache line、cache-hit 阈值 80ns。主要测试 `D=16`，并对 `D=13` 复测关键配置。每个配置运行三个独立进程，seed 分别为 20260912、20260913、20260914，每进程 30 轮、每轮 64 个新 trigger，共 5760 个 endpoint probes。各配置按 seed 打乱运行顺序，串行运行。

每轮顺序为：准备并写入私有内存页面、生成地址/PC 序列、清理训练与探测地址、执行 `CPP RCTX`、阶段 A 训练、阶段 B 训练、独立 trigger PC 访问新页面、探测目标。内存分配和序列准备在训练窗口之前；阶段 A/B/trigger 使用不重叠的地址区间。`CPP RCTX` 不被假设为能完全复位 BOP，另设无训练基线检验残留。

默认阶段 A 是多 PC 的 300 对 `X→X+D`，阶段 B 统一为 **600 次 demand load**。pair 有 300 个显式地址对；连续 stride 有 599 个相邻 `+D` 差分，不能把两者称为相同数量的 pair。关键因果对照在同一条 stride 地址序列上进行，改变 PC 分配不会改变地址序列。

| 可控因素 | 配置 |
| --- | --- |
| 地址形式 | 随机起点的 `Xi→Xi+D` pair；连续虚拟地址 stride；每页内短 stride 链；随机地址负对照 |
| PC | 同一个 load PC；16 个 PC 轮转；16 个 PC 随机分配 |
| flush | `batch`：训练前统一 flush；`pair`：每两个 load 前 flush 两个地址；`each`：每个 load 前 flush 当前地址 |
| 屏障对照 | `fence`：统一 flush，再在每个 load 前执行与 `each` 相同的屏障，但不追加 flush |
| 页面数量 | pair 的阶段 B 限定于 1、4、16、64 或 4096 页的地址池；默认 300 对选择 300 个不同页面 |
| 训练量 | 阶段 A 为 0/300/1000 对；阶段 B 为 0/300/600/1200/2000/2400 次 load，按不同配置选用 |

四类观测分别在独立进程进行，避免训练期逐次计时影响正式 endpoint probe：

- `pmu`：仅阶段 B 窗口，读取已有模块的 `L1D_CACHE_HWPRF (0x8154)`、`L2D_CACHE_HWPRF (0x8155)`。
- `cache`：不在训练期读计数器或插入逐次计时；最后探测新页面的目标。
- `timing`：阶段 B 对同一个指定训练 PC 的 load 逐次计时；这会增加屏障和延迟，只能作为辅助对照，不能将其命中率视为 cache pass 中的精确值。
- `demand`：使用两个 perf 原始事件，记录阶段 B 的 `L1D_CACHE_MISS (0x8144)` 和 `L2D_CACHE_MISS (0x814C)`；排除 EL1，拒绝 multiplexed 计数。开启/关闭计数的 syscall 在训练窗口边界，这个 pass 也不用于 endpoint probe。

事件含义来自 [Arm Cortex-X925 TRM](https://documentation-service.arm.com/static/68374ea93f2acc596c2d9d93)。计数包含窗口内地址表等辅助 load，按指定的训练 load 数归一化，不是逐条训练指令的精确 miss 概率。L1/L2 预取事件也不区分具体预取器，可能包括重复请求；大于 1 event/load 不等于同等数量的有效预取。

### `D=16` 的关键结果

下表均为三次运行的平均值；除注明外阶段 A 为 300 对、阶段 B 为 600 次 load。同一 PC 的前四行使用相同的训练地址序列。

| 阶段 B | 最后 `X+16` 命中率 | L1 预取事件/load | L2 demand miss/load |
| --- | ---: | ---: | ---: |
| 单 PC stride，统一 flush | 0.12% | 1.024 | 0.044 |
| 单 PC stride，统一 flush + 每次屏障 | 0.10% | 0.502 | 0.170 |
| 单 PC stride，每两个 load 前 flush | 99.83% | 1.020 | 0.799 |
| 单 PC stride，每个 load 前 flush | 99.93% | 0.342 | 1.001 |
| 16 PC 轮转 stride，统一 flush | 95.50% | 0.469 | 0.525 |
| 16 PC 随机 stride，统一 flush | 69.91% | 0.430 | 0.403 |
| 单 PC、随机页面 pair，统一 flush | 99.93% | 0.078 | 0.896 |
| 单 PC、只在一个页面内 pair，每对 flush | 99.77% | 0.109 | 0.931 |

辅助计时中，统一 flush 的单 PC stride 有 76.05% 的训练 load 在 80ns 内完成；逐次 flush 降为 0.16%。但两次观测的计时开销不同于正式 cache pass，因此主要证据是 matched-address flush 对照、独立 demand miss 计数和 endpoint probe 的共同变化。

其他控制结果：

- 仅阶段 A 300 对：0.12%；完全不训练：0.09%；阶段 B 改成随机访问：0.10%；阶段 A 1000 对阳性对照：99.90%。
- 训练 pair 后不执行 trigger：0.17%；逐次 flush 的 stride 训练后不执行 trigger：0.21%，说明训练本身没有把 endpoint 目标直接预取进 cache。
- 单 PC stride 统一 flush，阶段 B 增至 1200/2400 次 load：仍仅 0.12%/0.09%。仅增加总 load 数没有解决问题。
- 完全跳过阶段 A，单 PC stride 训练 2000 次：统一 flush 为 0.14%，逐次 flush 为 96.08%。因此不依赖阶段 A 的多 PC 训练，单 PC stride 自身也可以建立可迁移到新地址、新 PC 的 offset 行为。
- 阶段 B 只用一个页面可以补足学习，但从零开始、一个页面、2000 次 load 的 pair 仅 0.23%；用 4/16/64 页时分别为 99.71%/99.86%/99.84%。所以“页面间切换是每条学习证据的必要条件”不成立；“从零开始重复一个页面总能稳定学到 D”也没有得到支持。
- 页内短链统一 flush：单 PC 为 51.23%，16 PC 轮转为 99.67%。这里每条 `+D` 链在页面内完成，跨页时重新起链。

### 是否学成了其他 offset，以及 `D=13` 复测

对 `D=16` 的单 PC stride，额外把最后的 probe offset 从 `+1` 扫描到 `+32`，每个目标独立运行，避免同轮多目标探测相互污染：

- 统一 flush：所有 offset 的平均命中率均不超过 0.30%，没有观察到另一正向 offset 的稳定输出。
- 逐次 flush：`+16` 平均为 99.81%，其他 offset 均不超过 0.51%。

扫描只能排除本次检测到的 `+1..+32` 稳定输出，不能读取内部 score，也不排除暂存但未生效的候选。

`D=13` 的三个重复得到相同的关键方向：

| 阶段 B | 最后 `X+13` 命中率 | L2 demand miss/load |
| --- | ---: | ---: |
| 单 PC stride，统一 flush | 0.10% | 0.046 |
| 单 PC stride，统一 flush + 每次屏障 | 0.23% | 0.141 |
| 单 PC stride，每两个 load 前 flush | 2.43% | 0.597 |
| 单 PC stride，每个 load 前 flush | 99.88% | 1.000 |
| 无阶段 A，2000 次 stride，统一 flush | 0.10% | 0.014 |
| 无阶段 A，2000 次 stride，逐次 flush | 98.30% | 1.000 |

但 `D=13` 的普通 pair 统一 flush 只有 0.16%，1000 对阶段 A 的阳性参考也只有 81.79%。因此不能把 `D=16` 的训练阈值或“每对 flush 足够”的结论直接推广到其他 D；这里的证据是改变 flush 后效果恢复，而不是所有 offset 共享一个阈值。

### 如何解释“它们应该同时学习”

预取器可以并行工作，但并行不代表观察同一条完整的 demand-load 序列。如果较近层的预取已经让某次 demand load 命中，那么较远层收到的 demand miss、prefetched-hit 或 fill 事件会改变；其训练事件也可能因访问来源、预取标记和时机而筛选。经典 [A Best-Offset Prefetcher](https://comparch-conf.gatech.edu/dpc2/resource/dpc2_michaud.pdf) 本身以符合条件的 L2 miss/prefetched hit 参与训练，而非把每条 CPU load 都算一次训练。该论文不能被当作 X925 的实现说明。

本次最有力的实测链条是：同样的单 PC stride 地址序列，在统一 flush 时已有明显 L1 预取活动、L2 demand miss 很少、没有新 PC 上的 `+D` 输出；逐次 flush 使 demand miss 恢复，并使 `+D` 输出恢复。仅增加屏障或总访问量不能产生同样效果。这支持**其他预取活动改变了学习器可见的训练事件**这一解释。

尚不能证明存在“另一个预取器学到了 D，所以 BOP 主动忽略 D”的互斥规则。flush 同时改变 cache 驻留、请求时序、fill 和可能的预取标记；现有接口也没有 BOP score 和请求来源标记。一个页面从零训练失败而多页面成功还说明，重复地址、历史表覆盖、其他候选竞争等因素值得继续区分，不能把 L2 miss 数量当成充分条件。

若要进一步把“事件过滤”与“显式仲裁”分开，需要确认可单独控制的 L1/stride-prefetcher 开关，在完全相同地址、PC、flush、间隔下比较开/关，同时记录来源可区分的 L2 请求。当前没有使用未经确认的控制寄存器，也没有把 L2 总预取计数标成 BOP 专属计数。

最终数据与复现：

- [`D=16 原始结果`](res/bop-learning-X925-off16-final.tsv)、[`汇总`](res/bop-learning-X925-off16-final-summary.tsv)：49 个配置 × 4 个独立观测 pass × 3 次重复。
- [`D=13 原始结果`](res/bop-learning-X925-off13-final.tsv)、[`汇总`](res/bop-learning-X925-off13-final-summary.tsv)：15 个配置 × 4 个 pass × 3 次重复。
- [`+1..+32 扫描`](res/bop-learning-X925-off16-probe-scan.tsv)、[`扫描汇总`](res/bop-learning-X925-off16-probe-scan-summary.tsv)：2 种训练配置 × 32 个 probe offset × 3 次重复。
- 同名 `.json` 保存参数、case 定义、源文件及可执行文件 SHA256。`v1`/`v2` 文件为开发期数据，本节数值以 `final` 和 `probe-scan` 为准。

```sh
python3 re3-bop/test-bop-learning.py --build --cpu 6 --offset 16 \
  --output re3-bop/res/bop-learning-new-run.tsv
python3 re3-bop/test-bop-learning.py --cpu 6 --offset 16 \
  --cases stride_batch,stride_each --probe-scan --measures cache \
  --output re3-bop/res/bop-learning-new-scan.tsv
```

## L1 miss/L2 hit 与 prefetched hit 的学习实验（2026-09-13）

继续修改的是 [`test-bop-learning.c`](test-bop-learning.c)，运行器增加 `--suite residency`。本轮发现不能把“L2 hit”视为统一类型：普通驻留命中与 offset 预取供应后的首次 demand 使用，表现明显不同。

### PMU 修正与实验顺序

检查发现旧 `prefetch_pmu_event` 模块直接借用计数器 4/5，而 Linux perf 不知道这些计数器已被借用。上一轮虽然把直接读计数器与 perf 分为独立进程，但模块仍驻留，存在计数器所有权冲突风险。因此上一轮 perf 的精确数值应作为历史参考；本轮先确认设备无人使用，临时卸载模块，所有新 PMU pass 均由 perf 管理，结束后恢复原来的 CPU6 配置和设备权限。程序现在检测并拒绝这种共存；原来的 cache 观测不依赖 perf 计数值。

PMU 事件按 [X925 TRM](https://documentation-service.arm.com/static/68374ea93f2acc596c2d9d93) 选择：`demand` 测 0x8144/0x814C（L1/L2 demand miss），`access` 测 0x8148/0x814C（L2 demand access/miss），`prefetch` 测 0x8154/0x8155（L1/L2 HW prefetch）。`supply` 用同一预取事件组，只测目标供应过程。每个组只使用两个计数器，排除 EL1/EL2，检查 enabled/running 相等，拒绝 multiplexing。

固定 X925 CPU6、D=16、单个阶段 B load PC、阶段 A 300 对、阶段 B 600 次 load（300 对）。每项三个 seed（20260913～20260915），每个 seed 30 轮，每轮 64 个新 PC、新页面的 endpoint probes。四个主要步骤为：

1. 清理目标，然后按随机顺序供应阶段 B 的地址，避免准备阶段直接呈现 `X→X+D` 顺序。
2. 执行 `CPP RCTX` 和阶段 A 的 300 对训练。
3. 访问独立的 256KB 随机指针链，驱逐 64KB L1；随后阶段 B 不再 flush。L2 为 2MB，但不假设容量关系足以保证驻留，仍用计数验证。
4. 正式执行阶段 B，再访问新 trigger 并探测 `+D`。PMU 和 cache probe 分别在不同进程中运行。

普通供应为随机 demand load；软件供应为随机 `PRFM PLDL1KEEP` 或 `PLDL2KEEP`，等待后再进入阶段 A。每项都有保留相同准备过程但 `--skip-b` 的对照，以及准备后改成逐次 flush 的 rescue 对照。另扫描驱逐链 0/128/256/512KB、追加 2000 次 load，以及从零训练。

硬件供应分为两种，不能混称同一预取器：

- `hw-stream`：对每个目标 T 访问其前八条连续 line，但不 demand 访问 T，等待硬件预取。若供应地址恰好等于任一正式训练目标则跳过，防止直接把目标读热。这组只证明由硬件活动供应，不能唯一指认具体预取器。
- `hw-offset`：先在独立训练区训练 1500 对 `+8`，然后打乱目标顺序，对每个 T flush 并访问 `T-8 lines`，等待已学到的 offset 路径供应 T。供应阶段不 demand 访问任何阶段 B 目标。之后仍执行相同的 `CPP RCTX`、阶段 A 和 L1 驱逐。

`hw-offset` 是基于先前验证的 offset 行为构造的供应路径，并不是读取了“此请求由 BOP 发出”的内部标签。供应期所有初始化和 +8 训练都属于准备过程，**不计入报告中的阶段 B miss 窗口**；在 `supply` pass 中则会计入准备期 HW 请求数。

### D=16：命中层级相似，学习效果不同

| 阶段 B 目标来源 | L1 miss/load | L2 miss/load | 最后 `+16` 命中率 |
| --- | ---: | ---: | ---: |
| 冷地址，训练前统一 flush | 1.026 | 0.898 | 99.84% |
| 普通 demand 预热，再驱逐 L1 | 1.021 | 0.032 | 0.16% |
| `PRFM PLDL1KEEP` 预热，再驱逐 L1 | 1.034 | 0.065 | 0.36% |
| `PRFM PLDL2KEEP` 预热，再驱逐 L1 | 1.023 | 0.020 | 0.28% |
| `hw-stream` 供应，再驱逐 L1 | 1.037 | 0.098 | 1.72% |
| **`hw-offset` 供应，再驱逐 L1，首次 demand 使用** | **1.025** | **0.016** | **99.88%** |
| 相同 +8 初始训练，但目标改为 demand 预热 | 1.024 | 0.066 | 0.89% |
| 相同 offset 供应，但先随机 demand 消费一遍目标 | 1.024 | 0.055 | 0.30% |

三个 seed 下 `hw-offset` 的命中率分别为 99.84%、99.90%、99.90%。其 L2 miss 只有 0.011～0.022/load，远低于冷访问对照，说明不能以“只有 L2 miss 才能学习”解释这些结果。其他组的 L1 miss/load 也接近 1，但没有产生同等的 offset 学习。

`access` 独立 pass 同样测到：`hw-offset` 的 L2 demand access 为 1.645/load、miss 为 0.024/load；普通 demand 供应分别为 1.601/load、0.061/load。窗口包含辅助访问和可能的重复事件，这些比值不是逐条训练 load 的严格命中概率，也不能将 access-minus-miss 解释成精确的唯一目标命中数。

### 排除已有训练、残余 miss 和直接预热 endpoint

| 对照（保留 hw-offset 的完整准备过程） | 最后 `+16` 命中率 |
| --- | ---: |
| 完全跳过阶段 B | 0.33% |
| 执行阶段 B，但不访问最终 trigger | 0.28% |
| 同样600次阶段 B访问，打乱地址顺序 | 3.58% |
| 只执行16次阶段 B，每次先 flush | 0.30% |
| 只执行32次阶段 B，每次先 flush | 0.23% |
| 只执行64次阶段 B，每次先 flush | 0.28% |
| 只执行128次阶段 B，每次先 flush | 0.24% |

完整 warm 阶段 B 平均只有约 `600 × 0.0163 = 9.8` 个 L2 miss/轮；上述16～128次冷访问对照有更多 L2 miss，却没有建立目标 offset。因此“只有残余的约10次 L2 miss在训练”不足以解释99.88%的结果。冷访问取前N个地址，不能精确复制硬件实际 miss 的位置和时序，这仍是有力对照而非对内部评分机制的数学证明。

普通 demand 预热和软件 L2 预热后，把同样600次追加访问改成逐次 flush，命中率分别恢复到99.72%和99.74%；因此这些准备过程本身没有让实验永久失效。将普通/软件 L2 驻留的追加访问增加到2000次，仍分别只有0.54%和0.33%。

`hw-stream` 的600次追加访问有约0.098 L2 miss/load，不能称为纯 prefetched-hit 流。扩展到2000次时命中率升至75.75%，同时 miss/load 升至0.231（约462 miss/轮）；这组阳性不能单独归因于 prefetched hit，故不据此宣称所有硬件预取命中都能训练。

### D=13 交叉验证

| 供应与训练方式 | L2 miss/load | 最后 `+13` 命中率 |
| --- | ---: | ---: |
| 普通 demand 预热 | 0.033 | 2.40% |
| 软件 L2 预热 | 0.019 | 0.16% |
| hw-offset 首次成对 demand 使用 | 0.027 | 98.72% |
| 相同 +8 初始训练，目标改为 demand 预热 | 0.040 | 1.09% |
| hw-offset 供应后先随机消费一次 | 0.075 | 0.21% |
| hw-offset，跳过阶段 B | — | 1.46% |
| hw-offset，阶段 B 地址打乱 | 0.029 | 5.12% |
| hw-offset，只执行128次冷访问 | 1.004 | 2.43% |

与 D=16 相同，首次使用 offset 路径供应的目标有很强学习效果；普通命中、先消费一次和少量冷访问没有同等效果。D=13 的普通冷地址统一 flush 对照本身只有0.69%，不能当作充分训练的阳性基准，因此另保存逐次 flush 的阳性/rescue 复测：冷地址逐次 flush 为99.91%，软件 L2 预热后逐次 flush 为99.86%，hw-offset 供应后逐次 flush 为99.32%。不将 D=16 的训练阈值直接套用到 D=13。

### 能确认什么

1. **L1 miss、L2 hit 并不自动成为有效训练。**普通 demand 或软件预热得到的 L2 驻留命中，在当前参数下没有稳定完成目标 offset 的学习；未观测到效果不等于证明每次内部 score 增量严格为零。
2. **某类 prefetched 首次使用能够有效参与学习。**由已经学到的 +8 offset 路径供应目标，再以新 D 的地址对首次 demand 使用，即使 L2 miss 很少，也能稳定建立新 D。提前随机消费一次后效果消失，支持与预取历史、首次使用或关联历史表状态有关的解释。
3. **不能推广为所有预取命中都能训练。**软件预取和另一硬件供应模式没有同样的结果；不应把 PRFM、任意 HW prefetch 和 offset 路径自己的预取一概而论。

公开 TRM 中查到的 `L1I_CACHE_HIT_RD_FHWPRF` 是**指令缓存**事件，不能拿它测这里的数据访问。本轮没有直接读到数据行的 prefetched/first-use 标志或 BOP score。因此“first-use 标记被清除”“只有本预取器的预取才计分”仍是对对照结果的机制解释，尚未直接证实；历史表变化、请求来源筛选也可能产生相似现象。经典 BOP 的 L2 miss/prefetched-hit 学习条件与这些现象相容，但不能据此认定 X925 完全实现了论文算法。

最终结果（开发期 `v1` 不作为本节数字来源）：

- [`D=16 原始数据`](res/bop-residency-X925-off16-final.tsv) 与 [`汇总`](res/bop-residency-X925-off16-final-summary.tsv)：66配置 × 6独立pass × 3个seed。
- [`D=13 原始数据`](res/bop-residency-X925-off13-final.tsv) 与 [`汇总`](res/bop-residency-X925-off13-final-summary.tsv)：12配置 × 3独立pass × 3个seed。
- [`D=13 逐次flush复测`](res/bop-residency-X925-off13-rescue.tsv) 与 [`汇总`](res/bop-residency-X925-off13-rescue-summary.tsv)：3配置 × 3独立pass × 3个seed。
- 同名 `.json` 保存参数、源文件/二进制 SHA256，并记录本轮 `pmu_module_loaded=false`。raw TSV 的 `executed_loads` 区分准备的目标数与实际执行的追加访问数。

复现前先协调并停止占用 PMU 的模块；只有 cache pass 不需要此步骤。示例：

```sh
python3 re3-bop/test-bop-learning.py --build --suite residency --cpu 6 \
  --cases cold,demand,swl2,hw-offset,hw-offset_skip,hw-offset-consumed \
  --measures cache,demand,access --seed 20260913 \
  --output re3-bop/res/bop-residency-new-run.tsv
```

## 跨核心训练：全部在 EL0（2026-09-13）

本轮不再切换 EL 等级，不调用 EL1 训练模块。新增 [`test-bop-cross-core.c`](test-bop-cross-core.c) 和批量运行器 [`test-bop-cross-core.py`](test-bop-cross-core.py)。结果是：**在测试的核心组合和参数下，未观察到稳定的跨核心 offset 学习累积或远端已训练状态对本核的可用性；同核阳性对照约99%命中。**这与核心本地维护学习状态的解释相容，但不能据此证明内部所有表都完全私有。

### 核心选择与访问流程

本机 sysfs 的 MIDR 显示，X925（`0x410fd851`，part `0xd85`）对应 **CPU 5–9、15–19**；其余 CPU 0–4、10–14 为 A725。每个 X925 的 L2 为私有2MiB，所选组合为：

| 训练方向 A→B→A | L3 拓扑 |
| --- | --- |
| 6→7→6、7→6→7 | 同一8MiB L3，共享列表0–9 |
| 6→16→6、16→6→16 | 不同L3；CPU16属于10–19共享的16MiB L3 |

这里的箭头表示执行阶段交接，**不是线程迁移**。两个 pthread 分别固定在 A、B 核心；A 执行前 N 对训练，通知 B 执行 M 对，B 完成后 A 访问新地址 X，测量 X+D。训练、trigger、probe 的内存访问全部在 EL0；线程建立、绑核和 PMU 控制仍需要普通系统调用，但不在 EL1 生成训练对。

- `D=16 cache lines=1024 bytes`；`N=300`，`M=300、1500、3000`。每对训练为 `Xi→Xi+D`，即两次 demand load。
- 两核调用同一个训练 load PC。训练页随机排列，每页选择随机合法起点，一对地址不跨页；**每次 load 前分别 flush**，再执行访问。A、B 使用互不重叠的训练地址池。
- 内存预先建立私有物理页，避免训练途中首次分配页面。probe 地址池与训练地址池分离，B 不访问 A 最终的 trigger/target；最终 trigger PC 与训练 PC 不同。
- 每轮两个核心都先执行 `CPP RCTX` 和4096次随机行冷访问，双方完成后才开始 A 的 N 对训练。不把这些操作视为已证实的 BOP 完全复位；是否存在明显残余状态由阴性对照判断。
- A 的 N 对结束后，所有 case 都保留相同的2毫秒远端阶段：B 不训练的对照也等待；同核追加的 M 对也在这段时间执行。双方无训练阶段超时记录。本核阳性对照同时检查这段等待是否足以让有效状态消失。
- 每配置3个 seed（20260913–20260915），每次30轮，每轮64个新地址 trigger 和乱序 target probe，合计5760次 probe。命中阈值80ns。每个 case/seed/pass 单独启动进程，case 顺序打乱；独立进程不等于硬件状态自动复位。

### 七组对照

| case | A训练 | B训练 | 最后A触发并probe |
| --- | --- | --- | --- |
| `untrained` | 无 | 无 | 是 |
| `local_n` | N对D | 无 | 是 |
| `local_nm` | N+M对D | 无 | 是 |
| `remote_same` | N对D | M对D | 是 |
| `remote_other` | N对D | M对+13 | 是 |
| `remote_only` | 无 | M对D | 是 |
| `no_trigger` | N对D | M对D | 只probe，不trigger |

`local_nm` 的追加 M 对使用与远端 case 相同的第二地址池，只改变执行核心。因此核心比较不依赖于换用另一种训练序列。

### Cache probe 结果

下表均为三次运行的平均命中率；A 为最初训练及最终探测核心。

| A→B→A | 仅本地N（M=300对照） | 本地N+300 | 跨核N+300 | 跨核N+1500 | 跨核N+3000 | 仅远端3000 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 6→7→6 | 0.17% | 99.86% | 0.23% | 0.16% | 0.24% | 0.30% |
| 7→6→7 | 0.09% | 99.83% | 1.28% | 0.07% | 0.24% | 0.10% |
| 6→16→6 | 0.14% | 98.75% | 0.17% | 0.10% | 0.14% | 0.19% |
| 16→6→16 | 0.07% | 99.90% | 0.19% | 0.10% | 0.14% | 0.12% |

所有方向和 M 配置的 `local_nm` 平均命中率为98.75%–99.93%。未训练对照为0.12%–0.33%，不trigger对照为0.03%–0.21%。`remote_same` 没有随着远端训练量扩大而增强。

不能把所有低命中都称为严格零：`7→6→7, M=300, remote_same` 有一次运行达到3.49%，使其三次均值成为1.28%；`6→7→6, M=1500, local_n` 和 `6→16→6, M=300, remote_other` 也各有一次3.49%的波动。这些孤立结果没有构成可重复、offset 特异、随 M 增强的跨核效应。5760次 probe 来自多个共享训练状态的批次，不应当作5760个独立训练样本来夸大统计把握。

### PMU 核验

先运行独立 prefetch pass，再运行 cache pass，最后运行 demand pass。两个线程各自建立 Linux perf per-thread 两事件 group，绑定到各自核心；仅在实际训练窗口内计数，不将等待、scrub、trigger/probe 算入训练统计。要求 group 全程调度，拒绝 multiplex。PMU pass 不执行最后的 trigger/probe，因此它用于核验训练访问，不是最终预取命中的替代指标。

- demand：`0x8144`、`0x814c`，分别统计 L1、L2 demand miss。
- prefetch：`0x8154`、`0x8155`，分别统计 L1、L2 硬件预取请求。
- 在 `remote_same` 的所有独立 demand 运行中，A 的 L2 demand misses/训练load为1.0016–1.0073，B 为1.0010–1.0142，支持两核训练访问都保持冷访问，而不是 B 仅命中共享缓存。

下表是 `remote_same, M=3000` 的三次均值：

| A→B | A L2 demand miss/load | B L2 demand miss/load | B L2 HW prefetch/load |
| --- | ---: | ---: | ---: |
| 6→7 | 1.0040 | 1.0011 | 1.7965 |
| 7→6 | 1.0024 | 1.0013 | 1.7682 |
| 6→16 | 1.0034 | 1.0014 | 1.7457 |
| 16→6 | 1.0042 | 1.0013 | 1.8500 |

分母为实际执行的训练对数×2；窗口内指针/循环等辅助访问也可能产生事件，所以略大于1不是超过100%的逐指令 miss 概率。硬件预取事件不是 BOP 专属，也不能直接证明 B 的内部 best-offset 值；反向方向的本地阳性对照证明每个所测核心能够用该序列稳定训练，但不替代同一轮在 B 上直接探测其状态。

实验前临时卸载了占用硬件计数器的 `prefetch_pmu_event`，三类 pass 全部结束后已恢复为 CPU6，设备权限恢复为原用户可访问。最终数据共756行（4方向×3种M×7case×3seed×3pass），本地及远端训练阶段超时数均为0，所记录 CPU 与指定绑核一致。

### 结论边界与复现

本轮支持“**本核不足量训练不能由另一核心的同offset训练稳定补足**”，并且远端单独训练3000对也没有让本核获得稳定的同offset预取。同L3与跨L3都如此，不能把共享L3的数据可见性当作共享BOP训练状态。结论限于 D=+16、所测方向、当前训练形式和时序；没有覆盖全部10个X925，也没有排除其他索引条件下的共享或短暂作用。

结果文件：[`最终原始数据`](res/bop-cross-core-off16-final.tsv)、[`最终汇总`](res/bop-cross-core-off16-final-summary.tsv)、[`参数/拓扑/代码SHA256`](res/bop-cross-core-off16-final.json)。此前的 [`300+300校准`](res/bop-cross-core-off16-calibration-summary.tsv) 独立保存，不混入最终均值。

仅复现 cache 对照，无需卸载 PMU 模块；从仓库根目录执行，并选择未使用过的输出路径：

```sh
python3 re3-bop/test-bop-cross-core.py --build \
  --directions 6:7,7:6,6:16,16:6 \
  --train-a-pairs 300 --train-b-pairs 300,1500,3000 \
  --measures cache --output re3-bop/res/bop-cross-core-new-run.tsv
```

完整 PMU 复现需先确认计数器模块无人使用，卸载该模块，再把 `--measures` 改为 `prefetch,cache,demand`；结束后执行 `sudo kernel-modules/prefetch-pmu-event/module_load.sh --cpu 6` 恢复。程序会拒绝在该模块仍加载时进行 perf 测量。

## 同核跨线程、跨进程训练（2026-09-13）

**结论：本轮在CPU6和CPU16上均观察到，同一个核心上的BOP可观测学习状态能够跨线程、跨fork进程保留，并能累积两个上下文的训练。**这与上一节“没有稳定跨核心累积”的结果不同；两组实验不能混为一谈。

### 文件与实验条件

- [`test-bop-cross-thread.c`](test-bop-cross-thread.c)：同一进程、两个不同线程，均固定同一CPU。
- [`test-bop-cross-process.c`](test-bop-cross-process.c)：fork出的两个进程、不同地址空间，均固定同一CPU。
- 两个入口共用 [`bop-cross-context.h`](bop-cross-context.h)，避免两种模式的训练/probe算法不同。
- [`test-bop-cross-context.py`](test-bop-cross-context.py)：编译、独立进程运行、随机排列case、记录参数/代码SHA256并汇总。

CPU6、CPU16分别执行整套测试，**不是A在CPU6、B在CPU16**。每次运行A/B都在同一个核心，程序记录实际CPU、PID、TID。跨线程PID相同、TID不同；跨进程PID和TID均不同。

固定 `D=+16 cache lines=1024 bytes`，不足量训练 `N=M=300对`，充分训练 `K=1500对`，另一个offset为+13。每对是同一load PC上的 `Xi→Xi+D`，两次load前分别flush；页随机排列、页内起点随机，一对不跨页。最终trigger使用不同于训练的PC。

每个执行方在创建线程/fork之后独立分配私有训练及probe映射，并写入每页建立真实物理页；不是依赖共享零页，也不是共享训练目标。进程之间仅共享控制配置和最终统计，probe地址不被另一方访问。fork保留相同代码虚拟地址，这有利于控制PC变量，但意味着结果不等价于任意独立exec程序之间的共享。

每轮先准备双方地址及flush probe目标，B阻塞等待；然后A执行 `CPP RCTX` 和4096次无关冷访问，再进入正式训练。**从A正式训练开始到最后probe之间不执行预测器清理。**准备操作不被假定为硬件完全复位，依靠未训练、单边不足量等对照检查残留影响。

交接使用阻塞pipe，让同核两个任务轮流执行。同步和调度包含正常系统调用，但所有训练、trigger与probe访存都在EL0，没有内核训练模块。无切换对照中B一直等到A完成probe后才继续；最后一轮也有完成确认，防止B提前退出/释放内存影响A的测量。

每配置3个独立seed（20260913–20260915），每seed运行30轮、每轮64个target probe，共5760个probe。命中阈值80ns。训练轮次内probe共享学习状态，因此不能把5760个probe当成5760个独立学习样本。

### Cache probe 结果

下面均为三次运行合计命中率。A/B指线程或进程，不是核心；“切换”均发生在同一个核心上。

| case / 访问顺序 | CPU6跨线程 | CPU6跨进程 | CPU16跨线程 | CPU16跨进程 |
| --- | ---: | ---: | ---: | ---: |
| `untrained`：不训练，切换后A探测 | 0.05% | 0.24% | 0.12% | 0.24% |
| `local_n`：仅A训练300，A探测 | 0.05% | 0.23% | 0.05% | 0.31% |
| `switch_none`：A训练300，B不训练，回A探测 | 0.05% | 0.23% | 0.07% | 0.26% |
| `b_only`：仅B训练300，回A探测 | 0.19% | 0.23% | 0.07% | 0.21% |
| `local_nm`：A训练600，A直接探测 | 99.88% | 99.79% | 99.84% | 99.81% |
| `switch_local_nm`：A训练600，B不训练，回A探测 | 99.81% | 99.79% | 99.51% | 99.79% |
| `split_same`：A训练300，B同D训练300，回A探测 | **99.76%** | **99.18%** | **99.72%** | **99.90%** |
| `split_other`：A训练300，B以+13训练300，回A测+16 | 0.09% | 0.28% | 0.03% | 0.40% |
| `no_trigger`：A+B各训练300，但不执行trigger | 0.07% | 0.23% | 0.05% | 0.23% |
| `retain_local`：A训练1500，A直接探测 | 99.88% | 99.90% | 99.83% | 99.93% |
| `retain_switch`：A训练1500，B不训练，回A探测 | 99.81% | 99.88% | 99.76% | 99.84% |
| `retain_peer`：A训练1500，B直接探测 | **99.86%** | **99.84%** | **99.86%** | **99.91%** |
| `peer_local`：B自行训练1500，B探测 | 99.79% | 99.88% | 99.86% | 99.88% |
| `split_peer`：A训练300，B追加300并直接探测 | 99.84% | 99.90% | 99.90% | 99.91% |
| `untrained_peer`：双方不训练，B探测 | 0.17% | 0.24% | 0.19% | 0.07% |

两个关键判断：

1. **不是仅靠某一边就完成了训练。**A单独300对、B单独300对都不能稳定触发；合起来600对则达到99%左右。B换成+13不能补足+16，支持offset特异的跨上下文训练累积，而不只是切换或增加访存次数的作用。
2. **已学状态也能供另一上下文使用。**A充分训练后，B不训练就能在自己的新地址上触发，说明效果不限于“回到A恢复A自己的状态”。不trigger和未训练B对照均很低，排除了仅由目标提前驻留造成高命中的简单解释。

### PMU 与缺页核验

分别运行独立 `prefetch→cache→demand` pass。PMU由每个执行方自己的per-thread perf group统计，只覆盖自己的训练窗口，排除EL1/EL2事件并拒绝multiplex；cache pass不插入perf控制调用。事件为L1/L2 demand miss `0x8144/0x814c`、L1/L2 HW prefetch `0x8154/0x8155`。PMU pass不执行末尾trigger/probe。

`split_same` 的三次均值如下：

| CPU / 模式 | A L2 demand miss/训练load | B L2 demand miss/训练load | A L2 HW prefetch/训练load | B L2 HW prefetch/训练load |
| --- | ---: | ---: | ---: | ---: |
| CPU6 / 线程 | 1.0076 | 1.0038 | 1.0955 | 1.3172 |
| CPU6 / 进程 | 1.0076 | 1.0071 | 1.0235 | 1.4103 |
| CPU16 / 线程 | 1.0085 | 1.0039 | 1.1099 | 1.2972 |
| CPU16 / 进程 | 1.0047 | 1.0066 | 1.1090 | 1.2373 |

两边都有接近每个训练load一次的L2 demand miss，支持训练访问确实为冷访问。略大于1来自窗口内辅助访问等，不能解释为超过100%的单指令miss概率；硬件预取事件也不是BOP专属。共享学习结论主要来自cache对照，而不是直接读取了内部score。

最终两套数据各270行，共540行（2核×2模式×15case×3seed×3pass），测量区间 `critical_faults_a/b` 全部为0，实际CPU/PID/TID关系均正确，计数归一化和记录的代码/二进制SHA256已核对。CPU6上原先占用计数器的 `prefetch_pmu_event` 在完整测量期间临时卸载，结束后已恢复至CPU6和原设备访问权限。

开发期发现两类fork栈写时复制干扰：第一次 `getrusage` 把统计写到未私有化的栈页；编译器内联同步函数后，在另一栈位置首次写入ready字节。minor-fault采样分别定位到了 `__arch_copy_to_user→getrusage` 和ready字节store。修正为统计函数/同步发送函数不内联，并在统计前实际写入完整rusage缓冲区。256种环境长度/ASLR栈布局回归由失败转为通过。脚本遇到任何非零缺页记录会保留原始行并停止，不扣减“预期缺页”，也不将异常运行混入正常汇总。

带 `v1` 的早期整套数据和 `v2-incomplete` 的中止数据保留用于排查，不作为上表数字来源；校准数据也不混入最终均值。

### 能推广到哪里

本轮证明的是**当前系统、当前代码PC条件下，同核线程/fork进程间存在稳定可观测的offset训练累积与已学状态传递**。结合跨核阴性结果，更符合核心本地、但未在这些上下文切换中完全隔离的学习状态。

不能据此断言所有内部表都不使用ASID/PC索引，不能推广到任意独立exec程序、所有安全域或虚拟机，也没有测出最长保留时间。`handoff_us` 包含B执行工作和往返交接，部分case还包含B的probe，不能当作纯调度延迟。经典BOP内部状态的具体实现仍未直接观测。

结果与复现：

- CPU6：[`原始数据`](res/bop-cross-context-cpu6-off16-final.tsv)、[`汇总`](res/bop-cross-context-cpu6-off16-final-summary.tsv)、[`参数与SHA256`](res/bop-cross-context-cpu6-off16-final.json)。
- CPU16：[`原始数据`](res/bop-cross-context-cpu16-off16-final.tsv)、[`汇总`](res/bop-cross-context-cpu16-off16-final-summary.tsv)、[`参数与SHA256`](res/bop-cross-context-cpu16-off16-final.json)。

从仓库根目录运行cache对照，不需要更改PMU模块状态：

```sh
python3 re3-bop/test-bop-cross-context.py --build --cpu 6 \
  --train-a-pairs 300 --train-b-pairs 300 --full-pairs 1500 \
  --measures cache --output re3-bop/res/bop-context-new-run.tsv
```

完整PMU复现需要先协调卸载计数器模块，再改用 `--measures prefetch,cache,demand`，结束后恢复模块；具体操作见README。将 `--cpu 6` 改为 `--cpu 16` 即为第二个核心上的同核实验。

## 最大 offset 扫描

首先在 1024 对训练下扫描 `offset=0～128`。`+32` 仍然达到 1000 hits/1000，`+33` 则下降为 5 hits/1000，PMU 增量也从约 1 event/trigger 降为 0。原始结果保存在 [`res/bop-X925-cpu6-thr80-pairs1024-offset-sweep.tsv`](res/bop-X925-cpu6-thr80-pairs1024-offset-sweep.tsv)。

随后使用 3000 对训练，对边界附近的 `+28～+40` 各重复测试 3 次：

| offset（cache lines） | 平均 hits/1000 | 三次结果 |
| ---: | ---: | --- |
| +28 | 998.0 | 998, 998, 998 |
| +29 | 996.0 | 998, 997, 993 |
| +30 | 998.3 | 998, 998, 999 |
| +31 | 998.0 | 997, 999, 998 |
| +32 | 998.3 | 998, 999, 998 |
| +33 | 1.0 | 1, 1, 1 |
| +34 | 1.3 | 2, 1, 1 |
| +35 | 0.7 | 1, 1, 0 |
| +36 | 1.0 | 1, 1, 1 |
| +37 | 0.3 | 1, 0, 0 |
| +38 | 1.3 | 1, 1, 2 |
| +39 | 0.7 | 0, 1, 1 |
| +40 | 1.3 | 1, 1, 2 |

充分增加训练量并不能使 `+33` 及以上的 offset 生效，排除了“较大 offset 只是需要更多训练”的解释。因此，X925 上该机制的最大正向 offset 可以确定为 `+32 cache lines`，即 2048 bytes。

## 与经典 BOP 算法的区别

Pierre Michaud 提出的经典 BOP 使用一张设计时固定的 offset 列表。每次符合条件的 L2 miss 或 prefetched hit 只测试一个候选；测试完整张列表称为一轮。论文中的 DPC2 配置为：

- 46 个候选 offset：23 个正向、23 个负向；
- `SCORE_MAX = 31`；
- `ROUND_MAX = 100`；
- 一轮测试全部 46 个候选；
- 学习阶段结束时选择 score 最高的候选作为下一阶段的 best offset。

因此，如果某个候选每轮都能加 1 分，最快需要：

```text
31 rounds × 46 candidates = 1426 次符合条件的 L2 访问
```

如果没有候选达到 31 分，则最晚在：

```text
100 rounds × 46 candidates = 4600 次符合条件的 L2 访问
```

后结束学习阶段。论文还使用 `BAD_SCORE` 决定下一阶段是否关闭预取：1 MB LLC 配置为 1，256 KB LLC 配置为 10。

论文参数来自 [A Best-Offset Prefetcher](https://comparch-conf.gatech.edu/dpc2/resource/dpc2_michaud.pdf)。X925 的实现没有公开，因此不能假设它与 DPC2 版本具有相同的候选表、score 阈值或轮次计数方式。实验中的“训练对”也不等价于论文中的“符合条件的 L2 访问”。

## Best offset 能保持多少个 trigger

为了测量学到 `best offset=+16` 后还能支持多少个 trigger，新增了 [`test-bop-retention.c`](test-bop-retention.c) 和 [`test-bop-retention.py`](test-bop-retention.py)。实验采用以下顺序：

1. 使用 3000 对 `X → X+16` 访问完成训练。
2. 发出 N 个地址随机、页面互不重叠的 decay trigger。
3. decay 阶段不读取任何 `X+16` 预测目标，避免继续强化旧 best offset。
4. 最后发出 64 个全新 endpoint trigger，再统一测量其 `X+16` 目标。
5. 每个 N 重复 20 次；先独立运行 PMU pass，再独立运行 cache-probe pass。

关键结果如下。完整数据保存在 [`res/bop-X925-cpu6-off16-retention.tsv`](res/bop-X925-cpu6-off16-retention.tsv)。

| 无关 trigger 数 N | L2 PMU events/trigger | `X+16` hits/1000 | 平均延迟（ns） |
| ---: | ---: | ---: | ---: |
| 0 | 1.095312 | 997 | 29.14 |
| 512 | 1.103125 | 999 | 28.69 |
| 768 | 1.354688 | 959 | 33.81 |
| 832 | 1.549219 | 899 | 39.39 |
| 896 | 1.293750 | 957 | 34.35 |
| 960 | 1.211719 | 751 | 58.33 |
| 1024 | 1.262500 | 628 | 74.20 |
| 1088 | 0.796094 | 517 | 87.34 |
| 1152 | 0.835938 | 416 | 96.70 |
| 1216 | 0.657813 | 239 | 117.61 |
| 1280 | 0.583594 | 348 | 106.71 |
| 1536 | 0.842187 | 150 | 130.34 |
| 2048 | 0.534375 | 199 | 126.72 |
| 2304 | 0.855469 | 200 | 124.79 |
| 3072 | 0.550000 | 1 | 147.51 |
| 4096 | 0.473438 | 0 | 149.68 |

从 cache probe 可以得到分层结论：

- **可靠保持范围约为 900 个 trigger**：经过 896 个不强化 +16 的 trigger 后，下一批 probe 仍有 95.7% 命中；832 点的 89.9% 也说明边界附近存在波动。
- **主要衰减从约 960 个 trigger 开始**：960 时降为 75.1%，1024 时为 62.8%，1088 时为 51.7%。
- **960～2304 是非单调衰减区**：命中率会因学习阶段边界、内部表项别名或其他硬件状态发生波动，不能把保持时间解释为一个精确 TTL。
- **约 3072 个 trigger 后 +16 基本失效**：3072 时只剩 1 hit/1000，4096 时为 0。

PMU 只能回答“是否产生了某种硬件预取”，不能确认预取地址仍是 `X+16`。例如 N=3072 时，L2 PMU 仍有 0.550000 event/trigger，但 `X+16` cache hit 只剩 1/1000。这说明旧 best offset 已基本失效，而 PMU 仍在统计其他 offset 或其他预取机制产生的事件。因此，本实验用 PMU 定位活动变化，用 cache probe 最终确定 +16 是否仍被保存。

以上数字描述的是访问模式停止后的**非强化保持时间**。如果后续 trigger 的预取目标持续被 demand access，这些访问可能继续给 +16 增加 score，使 best offset 被刷新并保持更久，甚至在稳定访问模式下长期有效。

## 结果限制

1. 硬件没有提供直接读取 BOP best offset、score 或候选表的接口，结论来自 cache latency 和 PMU 的间接观测。
2. 当前测试只覆盖非负 offset，因此 `+32` 是最大**正向**距离，不能据此推断最小负向 offset。
3. 预取器状态无法被实验程序可靠地完全复位，临界区域会受到历史状态、调度和 cache 噪声影响。因此报告采用区间和多次重复平均值，而不是声称存在精确到单次访问的硬阈值。
4. “至少支持 32 个正向 offset 值”不等于“内部同时保存 32 个候选”。若要确定竞争容量，需要让不同数量的 offset 同时竞争并观察候选淘汰行为。
5. best offset 保持时间依赖后续访问序列。报告中的约 900/3072 trigger 边界只适用于随机、互不相关且不访问预测目标的 decay stream。

## 总结

- 对 `offset=+16`，约 450～500 对训练开始触发预取，约 535 对达到 50% 命中率，600 对以上较为稳定，700 对左右接近饱和。
- X925 实测最大正向 offset 为 `+32 cache lines`，即 2048 bytes；`+33` 及以上无法通过增加训练量激活。
- 经典 BOP 有 46 个候选 offset，但这不能直接用于推断 X925 的硬件候选数量。
- 当前能够确认的是 `+1～+32` 均可学习；均衡竞争时 `+8/+16` 可同时有效，3:1 比例时优势 offset 主导。
- 多候选容量实验显示，按“所有候选均达到 90% 命中”的严格标准，稳定同时有效的 best offset 数量为 1；两个候选可出现部分共存，但不能同时稳定达到该标准。
- 在先饱和训练 `+8`、再顺序训练 `+16` 的直接测试中，`+16` 约 580 对后稳定替换 `+8`；480～560 对为竞争过渡区。
- 对已经学到的 `+16`，在不再强化该 offset 时可可靠支持约 900 个后续 trigger，约 1088 个 trigger 后只剩约一半命中率，3072 个 trigger 后基本失效。
