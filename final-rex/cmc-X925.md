# Cortex-X925 CMC 预取器逆向结果

本文总结截至 2026-08-17 在 Cortex-X925 上对 CMC（Temporal/Correlation Memory Cache）预取行为的逆向结果。文中将结论分为“直接验证”“规律推断”和“尚未确认”，避免把一次运行中的瞬态行为解释为固定硬件参数。

## 1. 实验设置

主要存在性实验为 `cmc/test-cmc.c` 和 `cmc/test-cmc.py`，目标归因使用 `cmc/tmp-x925-lookahead.c` 和 `cmc/tmp-x925-lookahead.py`。

- CPU：Cortex-X925，CPU 6。
- 时间序列：128 个 node。
- 每个 node 位于随机页面中的随机 cache line；node 编号表示访问的时间顺序，不表示地址连续关系。
- 每轮开始执行 `CPP RCTX`。
- 训练一次时，按 `node[0] ... node[127]` 的顺序访问完整序列一次。
- 训练前会 flush node，因此一次 replay 包含约 128 次训练 demand miss。
- 训练结束后再次 flush 全部 node，只保留预取器学习到的关系。
- 随后访问一个 trigger node，等待 1000 个 NOP，再读取 PMU 或探测一个目标 cache line。
- PMU 和 cache probe 在正式测试中分别运行，避免 probe 本身污染 PMU 窗口。
- X925 PMU：事件 `0x8154` 用于 L1D hardware prefetch，事件 `0x8155` 用于 L2D hardware prefetch。

本文重点讨论以下基本条件：

```text
training_replays = 1
prefix_misses    = 0
```

`prefix_misses=0` 只表示训练后的最终 flush 与 trigger 之间没有插入独立 dummy miss；并不表示本轮训练前没有 demand miss。

## 2. CMC 存在性

X925 上存在能够学习随机跨页 miss 时间序列的预取行为。一次完整序列训练已经足以在部分 trigger node 上产生明显 PMU 增量和目标 cache hit。

正式测试的一次典型结果为：

```text
untrained: L1 PMU = 0, L2 PMU = 0
trained:   L1 PMU > 0, L2 PMU > 0
```

例如在一次 `training_replays=1` 运行中，整体增量约为：

```text
L1D hardware prefetch: 0.3248 / round
L2D hardware prefetch: 0.0558 / round
```

非零事件只集中在少数 node，而不是均匀分布。这说明 X925 并非为训练序列中的每个 miss 都建立独立预测。

## 3. 可确认的触发节点和目标

在 `training_replays=1、prefix_misses=0` 的全 trigger、全后继目标扫描中，以以下条件作为可靠 cache 归因标准：

```text
trained - untrained 命中率 >= 100 / 1000
trained - untrained 平均延迟 <= -10 ns
```

得到以下直接验证的关系：

```text
node  1 -> node  2,  3, 17
node  2 -> node 18, 19, 33
node  3 -> node 34, 35, 49
node 18 -> node 50, 51, 65
node 34 -> node 66, 67, 81
node 50 -> node 82, 83, 97
node 66 -> node 98, 99, 113
```

对应的 cache 命中率增量如下：

| Trigger | Target | 距离 | 命中率增量/1000 |
|---:|---:|---:|---:|
| 1 | 2 | +1 | 1000 |
| 1 | 3 | +2 | 750 |
| 1 | 17 | +16 | 325 |
| 2 | 18 | +16 | 1000 |
| 2 | 19 | +17 | 650 |
| 2 | 33 | +31 | 325 |
| 3 | 34 | +31 | 1000 |
| 3 | 35 | +32 | 650 |
| 3 | 49 | +46 | 350 |
| 18 | 50 | +32 | 1000 |
| 18 | 51 | +33 | 625 |
| 18 | 65 | +47 | 275 |
| 34 | 66 | +32 | 1000 |
| 34 | 67 | +33 | 675 |
| 34 | 81 | +47 | 350 |
| 50 | 82 | +32 | 950 |
| 50 | 83 | +33 | 550 |
| 50 | 97 | +47 | 500 |
| 66 | 98 | +32 | 1000 |
| 66 | 99 | +33 | 575 |
| 66 | 113 | +47 | 450 |

## 4. 16-node 周期

进入稳定阶段后，强触发节点为：

```text
2, 18, 34, 50, 66, 82, ...
```

它们满足：

```text
trigger = 2 + 16k
trigger mod 16 = 2
```

其中 `node82` 在无 prefix 的全目标扫描中只有较弱 PMU 信号，尚未得到与前面节点同等强度的 cache 归因，因此应视为周期延伸候选，而不是同等可靠的已确认点。

对已经进入稳态的强触发节点 `i >= 18`，目标关系一致：

```text
node[i] -> node[i+32], node[i+33], node[i+47]
```

换成 16-node 窗口表示：

- `i+32`：两个窗口后的相位 2。
- `i+33`：两个窗口后的相位 3。
- `i+47`：三个窗口后的相位 1。

三个目标的模 16 相位分别为：

```text
2, 3, 1
```

第一个目标 `i+32` 经常又成为后续 trigger：

```text
18 -> 50 -> 82
34 -> 66 -> 98
```

这更像经过采样或压缩的稀疏时间关联图，而不是简单的“当前 node 预取下一个 node”。当前结果强烈暗示内部存在约 16-node 的采样、索引或压缩周期，但尚不能把 16 宣称为所有序列长度和地址映射下都固定的硬件常量。

## 5. 次级触发相位

正式 PMU 测试还反复观察到另一组较弱触发位置：

```text
1, 17, 33, 49, 65, 81, ...
```

满足：

```text
trigger = 1 + 16k
trigger mod 16 = 1
```

`node1` 的目标已经由 cache probe 直接确认：

```text
node 1 -> node 2, 3, 17
```

根据 16-node 周期，可以提出以下强规律推断：

```text
node  1 -> node  2,  3, 17
node 17 -> node 18, 19, 33
node 33 -> node 34, 35, 49
node 49 -> node 50, 51, 65
node 65 -> node 66, 67, 81
node 81 -> node 82, 83, 97
```

统一写为：

```text
node[i] -> node[i+1], node[i+2], node[i+16]
其中 i mod 16 = 1
```

但除 `node1` 外，这组具体目标目前主要是根据周期推断。正式 `test-cmc` 只探测 `node[i+1]`，尚未在 `node17、33、49、65、81` 上得到稳定 cache hit，因此这些目标不能标记为已经直接确认。

## 6. 序列开头的启动行为

`node1、node2、node3` 不完全符合稳态的 `+32、+33、+47` 模式：

```text
node 1 -> +1,  +2,  +16
node 2 -> +16, +17, +31
node 3 -> +31, +32, +46
```

它们应视为时间历史尚未填满时的启动边界行为。`node0` 在部分正式测试中产生过 PMU 和 `node1` cache hit，但没有在全目标扫描中稳定复现，因此目前只归类为启动瞬态候选。

## 7. L1 与 L2 目标归因

无 prefix 的大范围扫描证明了上述目标会进入 cache，但该运行中的 L2 PMU 事件较稀疏且具有明显状态依赖，不能仅凭事件总数把每个目标稳定分成 L1 或 L2。

在后续固定 `node50`、加入 40 个独立 prefix miss 的归因实验中，两次独立运行得到：

```text
node50 -> node82: 主要与 L1 PMU 活跃高度相关
node50 -> node83: L1 目标；L2 活跃样本中没有对应命中
node50 -> node97: L2 目标；L2 活跃样本中分别为 33/33、15/15 命中
```

因此对 `node50`，当前最强的层级归因是：

```text
L1: node82, node83
L2: node97
```

`node82` 在 L2 活跃样本中也会命中，但它在绝大多数 L1 活跃样本中已经命中，现有数据无法判断它是否同时也是 L2 目标。

PMU 事件数不等于唯一预取 cache line 数：一个 trigger 可能产生多个 PMU 事件，也可能因预取合并、层级迁移或计数语义导致事件数与命中目标数不同。

## 8. 训练次数与 prefix miss

### 训练次数

一次完整时间序列 replay 已经足以产生预取：

```text
training_replays = 1
```

这里的“一次”指完整访问 128-node 序列一次，不是只训练一条边，也不是链条长度为 1。当前能得出的结论是“一次完整 replay 足够”，而不是“CMC 只观察一次单独转移就完成学习”。

### Prefix miss

`prefix_misses=0` 时已经观测到明确预取，因此额外 prefix miss 不是 X925 CMC 的必要触发条件。

在一次固定 `node50` 的密集归因运行中，无 prefix 的信号几乎消失；加入约 40 个随机跨页独立 demand miss 后，信号重新稳定出现。因此当前正确表述为：

> X925 的 CMC 行为具有运行状态或调度依赖；约 40 个独立 miss 曾在固定锚点实验中恢复预取信号，但 40 不是已证明的固定最小值，也不是训练 replay 数量。

可能涉及历史窗口、置信度、节流、采样相位或上下文状态，但内部原因尚未确定。

## 9. 与地址、PC 和其他预取器的关系

### 地址关系

node 分散在随机页面和随机 cache line，因此 `+16、+32、+47` 等数字是时间序列索引距离，不是地址差。结果不支持普通固定地址 stride 对这些目标的解释。

### Region prefetcher 干扰

每个随机页面/region 在训练序列中只选择一个目标 cache line。已有 Region prefetcher 实验显示，X925 上需要显著更多的同 region 训练访问才能稳定看到 region replay。因此当前 CMC 结果不符合已知 Region prefetcher 的触发条件。

### Stride prefetcher 干扰

相邻 node 的页面和行号由固定随机映射产生，没有稳定地址步长；X925 stride prefetcher 还需要较长的连续 stride 访问才能进入稳定预取。因此周期性 node 关系不能由普通 stride 模式直接解释。

### Load PC

A725 上已经验证训练和触发可以使用不同 load PC，但这一结论不能自动移植到 X925。X925 的训练 PC、触发 PC 是否必须相同，目前没有同等完整的 PMU 加 cache 归因结果，仍属于待验证项。

## 10. `CPP RCTX` 的解释限制

测试在每个 sample 前执行 `CPP RCTX`，但目前只能确认它改变或重置了部分 context-prefetcher 状态。不能从软件侧证明它清除了所有学习表、置信度、节流状态或全局历史。

因此代码中的“每轮为空状态”应视为实验假设，而不是已经得到硬件文档保证的事实。不同循环顺序下出现的非平稳行为也提示 `CPP RCTX` 可能没有清除所有影响 CMC 的内部状态。

## 11. 当前结论

在 X925 上，CMC 能够从随机跨页 demand-miss 时间序列中学习稀疏关联。对当前 128-node 映射：

1. 一次完整训练 replay 已足以产生预取。
2. 无额外 prefix miss 也能触发预取。
3. 稳态强 trigger 主要满足 `node mod 16 = 2`。
4. 稳态目标满足 `i -> i+32, i+33, i+47`。
5. 还存在较弱的 `node mod 16 = 1` 触发相位；其完整目标模式尚需 cache probe 确认。
6. 对 `node50` 的稳定归因是 L1 预取 `node82、node83`，L2 预取 `node97`。
7. 该行为与 temporal node 位置相关，而不是与随机地址之间的空间距离相关。
8. 16-node 周期、PC 依赖和 `CPP RCTX` 的精确语义仍需进一步实验验证。

## 12. 相关代码与数据

```text
cmc/test-cmc.c
cmc/test-cmc.py
cmc/test-index-pc.c
cmc/test-index-pc.py
cmc/tmp-x925-lookahead.c
cmc/tmp-x925-lookahead.py
cmc/tmp-x925-prefix.c
cmc/tmp-x925-prefix.py

cmc/res/cmc-basic-X925-cpu6-thr80.txt
cmc/res/cmc-basic-X925-cpu6-thr80-pmu.txt
cmc/res/cmc-basic-X925-cpu6-thr80-cache.txt
cmc/res/tmp-x925-lookahead-127-r1.txt
cmc/res/tmp-x925-lookahead-127-r1-node50.txt
cmc/res/tmp-x925-cyclic-node50-prefix40-r1.txt
cmc/res/tmp-x925-cyclic-node50-prefix40-r1-repeat.txt
```

基本存在性测试：

```bash
cd /home/jiangfang/workspace/re-prefetcher/cmc
python3 test-cmc.py --arch X925 --training-replays 1
```

全目标临时扫描：

```bash
python3 tmp-x925-lookahead.py \
  --arch X925 \
  --training-replays 1 \
  --samples 40 \
  --max-lookahead 127 \
  --trigger-node -1 \
  --prefix-misses 0
```
