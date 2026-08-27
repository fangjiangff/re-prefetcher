# 本周工作总结：基于 PMU 的预取器逆向

时间范围：2026-08-10—2026-08-16

## 第 1 页｜本周工作主线

**目标**

1. 将 PMU 从 `perf/perf_event_open` 改为非 perf 形式，为预取器逆向提供低扰动观测手段。
2. 以 PMU 事件为主要 oracle，逆向 region prefetcher 的触发、PC 相关性和容量特征。
3. 以 PMU 事件为主要 oracle，逆向 CMC 的存在性及 load-PC 相关性。

**本周产出**

- 完成 Cortex-X925、Cortex-A725 的 direct-MRS PMU 工具链。
- 仅通过 PMU 即确认：X925/A725 存在可训练的 region 类预取行为。
- 仅通过 PMU 即确认：A725 CMC 训练后会产生强 L2 预取活动，且该活动对 load PC 变化不敏感。
- X925 CMC 已观察到训练相关 PMU 信号，但具体预测落点仍待确认。
- cache probe 作为第二阶段工具，仅用于确认 PMU 所代表的预取落点和命中效果。

**一句话总结**

本周的关键进展不是“用 cache timing 猜预取器”，而是建立了可在极短窗口内直接统计预取事件的 PMU oracle，并开始用事件数量反推预取器结构。

---

## 第 2 页｜PMU 改为非 perf：逆向基础设施

**perf 方案的问题**

- `perf_event_open` 的启停和读取经过内核路径，容易扩大 trigger window。
- 每轮只有 1～数个预取事件时，系统调用、计数器调度和 multiplexing 会显著干扰结果。
- 难以保证 flush、训练、probe 等操作完全位于计数窗口之外。

**新方案**

- 内核模块在指定 CPU 上固定配置 PMUv3 counter 4/5。
- 用户态直接执行 `MRS PMEVCNTR4_EL0/PMEVCNTR5_EL0` 获取前后快照。
- 计数窗口严格限制为 `trigger + wait`。
- flush、训练、cache probe、ioctl 全部放在窗口外。
- 进程固定到目标核；计数器按实验 rearm，不在每轮重新配置。

| 平台 | Counter 4 | Counter 5 |
| --- | --- | --- |
| Cortex-X925 | `0x8154` L1D HW prefetch | `0x8155` L2D HW prefetch |
| Cortex-A725 | `0x8154` L1D HW prefetch | `0x8285` L2D HW/SW prefetch fetch |

**直接收益**

- 可以稳定分辨每轮约 2 个 L2 事件。
- 不必先知道预取落点，只需比较实验变量改变前后的事件增量。
- 适合做 PC bit、训练次数、表项压力、上下文等大规模参数扫描。

---

## 第 3 页｜如何用 PMU 逆向预取器

**统一实验范式**

`清状态 → 构造训练历史 → PMU before → 单次 trigger → wait → PMU after`

| 改变的变量 | PMU 可回答的问题 |
| --- | --- |
| 有训练 / 无训练 | 是否存在可学习的硬件预取机制 |
| 训练 PC / 未训练 PC | 触发是否与指令 PC 相关 |
| 逐位翻转 PC | 哪些 PC 位可能进入索引或 tag |
| footprint 长度/形状 | 训练内容与预取事件数的关系 |
| trigger 位置 | 触发条件、事件数量是否与位置有关 |
| 插入不同 PC/地址模式 | 表项是否冲突、是否发生淘汰 |
| replay 次数 | 置信度形成阈值 |
| 上下文或执行域变化 | 状态是否共享、是否被切换清除 |

**PMU 的优势**

- 不需要逐行扫描 cache，适合快速搜索触发条件。
- 事件数接近预取器实际动作，比单点 latency 更直接。
- 能先定位“有信号”的参数区域，再用 cache probe 做少量定点确认。

**PMU 单独不能回答**

- 具体哪条 cache line 被预取。
- 预取最终落在 L1、L2 还是更低层级。
- 某个 PMU event 是否对应唯一 cache line。

---

## 第 4 页｜Region prefetcher：PMU 逆向方法

**存在性实验**

1. 在多个随机 4 KiB region 中，用同一条 load PC 重复访问 footprint `{4, 1, 19}`。
2. 切换到未参与训练的新 region，以同一 PC 访问 line 28。
3. PMU 只统计该 trigger 及等待阶段产生的 L1/L2 prefetch events。
4. 对照组保持相同训练过程，但在新 region 不使用已训练 PC 触发。

**变量设计**

- 训练状态：trained / untrained。
- 触发 PC：same PC / different PC / PC bit flip。
- 条目压力：插入大量不同 PC key 和 region pattern 后重新触发原条目。
- 所有 cache maintenance 均在 PMU window 外，避免把清 cache 和 probe 计入结果。

---

## 第 5 页｜Region prefetcher：PMU 获得的结果

**1. 存在性与事件层级**

| 平台 | 未训练 L1 | 未训练 L2 | 训练后 L1 | 训练后 L2 |
| --- | ---: | ---: | ---: | ---: |
| X925 | 0.00 | 0.00 | 0.00 | 2.42 / round |
| A725 | 0.00 | 0.00 | 0.00 | 2.00 / round |

由 PMU 可以直接得到：

- 训练历史使单次新 region trigger 产生稳定的额外预取事件。
- 当前可观测信号主要体现在 L2 prefetch event，L1 event 接近 0。
- 事件强度稳定在约 2/round；但 PMU event 是实现定义事件，不能仅凭计数把它解释为两条唯一 cache line。

**2. PC 相关性**

- 当前已确认：使用训练 PC 在新 region 触发时产生约 2 个 L2 events。
- 现有 control 是“不执行新 region trigger”的 training-only 对照，不等价于“用未训练 PC 执行同一 trigger”。
- 因此现有 PMU 结果尚不能单独证明 PC 相关性。
- 已完成“未训练 PC 同访问”和逐位翻转 `PC[3..47]` 的测试框架；完整结果产出后再判断 PC 是否参与索引或 tag。

**3. 条目压力实验**

- 插入 8192 个不同 PC key、累计构造 212,992 个 region 后，原训练 PC 仍产生 `2.00` 个 L2 events。
- PMU 没有观察到原条目被淘汰。
- 当前只能得出“尚未找到简单容量边界”；不能直接宣称硬件至少有 8192 entries，因为还需证明所有插入模式都建立了有效竞争条目。

---

## 第 6 页｜CMC：PMU 逆向方法

**基本实验**

1. 构造跨随机 page、随机 cache line 的地址链，消除 stride 和相邻行模式。
2. 重复执行 node0→node1→… 的链式访问，形成 CMC 训练历史。
3. 清 cache 后仅执行一个 trigger 或一个短 prefix。
4. PMU 统计 trigger window 内产生的 L1/L2 prefetch events。

**PC 相关性实验**

- 所有 case 使用相同地址链和相同训练次数。
- 只改变执行 load 的 PC 组织方式：同一 PC、单个不同 PC、2 个 PC 交替、8 个 PC round-robin、每轮 replay 更换 PC。
- 如果 CMC 以 load PC 为关键索引，PC 变化应显著降低 PMU event 数。

---

## 第 7 页｜CMC：PMU 获得的结果

### A725

| 训练方式 | L1 PMU / round | L2 PMU / round |
| --- | ---: | ---: |
| 未训练 | 0.00 | 0.00 |
| 同一 PC | 0.00 | 35.18 |
| 单个不同 PC | 0.00 | 34.13 |
| 两个 PC 交替 | 0.00 | 34.83 |
| 8 个 PC round-robin | 0.00 | 34.48 |
| 每次 replay 轮换 PC | 0.00 | 34.65 |

**PMU 结论**

- 未训练时没有事件，训练后单次 trigger window 出现约 34～35 个 L2 events，确认存在强链式预取活动。
- PC 从 1 个变为 8 个、训练 PC 与触发 PC 不同，L2 event 数基本不变，波动小于约 3%。
- 因此 A725 CMC 基本不依赖 load PC 进行索引/匹配，更符合基于地址或地址转移关系学习的模型。
- L1 event 始终为 0；当前信号集中在 A725 的 L2 相关 event。

### X925

- 训练后 node trigger window 中观察到 L1 `0.65/round`、L2 `0.43/round`。
- 对照条件下相关 PMU 活动接近 0。
- PMU 可以确认训练会触发硬件预取动作，但事件数量明显弱于 A725，且尚不能仅凭 PMU 判断其预测目标。

---

## 第 8 页｜Cache probe：在 PMU 之后确认落点

cache probe 不作为主要搜索手段，只回答 PMU 无法回答的“预取到了哪里”。

### Region prefetcher：确认相对 footprint

- 训练 footprint：`{4, 1, 19}`，首个访问 line 4。
- 新 region 以 line 28 触发。
- probe 定位到 line 25、43，命中率约 `983/1000`、`1000/1000`。
- `25 = 28 + (1−4)`，`43 = 28 + (19−4)`。
- 在 PMU 已确认“产生约 2 个预取事件”的基础上，cache probe 进一步确认：目标符合以首访问为锚点的相对 footprint 平移。

### A725 CMC：确认目标后继节点

| 训练方式 | node1 hit / 1000 | node1 平均延迟 |
| --- | ---: | ---: |
| 未训练 | 0 | 140.16 ns |
| 各训练 PC 模式 | 970～990 | 45.20～46.96 ns |

- PMU 先证明训练后产生约 35 个 L2 events，且与 PC 模式无关。
- cache probe 再确认这些事件中包含预期的 node1，且 node1 已进入快速访问层级。

### X925 CMC：尚未完成落点闭环

- 当前 cache probe 尚未稳定复现预期 next-node hit。
- 因此只保留 PMU 结论：“训练触发了预取活动”；不升级为“已确认预测到 next node”。

---

## 第 9 页｜本周逆向结论分级

**仅依靠 PMU 已确认**

- direct-MRS PMU 可在极短窗口稳定观测少量预取事件。
- X925/A725 的 region pattern 训练会使新 region trigger 产生约 2 个 L2 events。
- Region 在训练 PC 的新 region trigger 后会产生稳定 L2 events；PC 相关性仍待同访问对照和 bit sweep 确认。
- A725 CMC 训练会产生约 34～35 个 L2 events。
- A725 CMC 对 load PC 数量、组合以及训练/触发 PC 差异不敏感。
- X925 CMC 训练后存在额外 L1/L2 prefetch events。

**必须结合 cache probe 才能确认**

- Region 的具体预测行是 25、43。
- Region 保存的是以首访问为锚点的相对 footprint。
- A725 CMC 的 PMU 事件覆盖了预期后继 node1。
- 预取目标的实际 cache latency 与命中率。

**当前仍不能确认**

- Region 的具体 PC index bits 和真实表项容量。
- CMC 的内部表结构、容量、hash 和替换策略。
- X925 CMC 的具体预测落点。

---

## 第 10 页｜下周计划：继续以 PMU 为主线

1. 跑完 region PC bit sweep，以 L2 event 是否从约 2 降为 0 判定 alias/index 位。
2. 重构 region entries 实验：先用 PMU 验证每类插入 pattern 都能触发，再施加条目压力，寻找 PMU event 消失的淘汰点。
3. 扫描 footprint 长度、顺序和 trigger 位置，建立“训练内容—PMU event 数”的对应关系。
4. 在 A725 上扫描 CMC chain length、replay 次数和 prefix depth，用 PMU 定位置信度阈值及最大预取数量。
5. 在 X925 上先通过 PMU edge scan 找到强信号条件，再只对强信号 case 做 cache 落点扫描。
6. 将 cache probe 固定为第二阶段验证，减少全地址扫描带来的噪声和实验成本。

---

## 汇报口径

- 主线表述：`PMU 发现预取行为与结构特征 → cache probe 确认具体落点`。
- 可以说“PMU 已确认”：region 存在性、A725 CMC 存在性和训练 load-PC 不敏感性。
- 需要注明“由 cache probe 补充确认”：region 相对 footprint、CMC 命中后继节点。
- 不建议宣称：region 至少有 8192 entries、X925 CMC 已预测到 next node、A725 `0x8285` 中每个事件都对应唯一硬件预取 cache line。

## 建议插图

- PMU 测量框图：模块配置 counter → `MRS before` → trigger → `MRS after` → event delta。
- Region PMU 柱状图：untrained `0` vs trained `2.42/round`。
- CMC PMU 柱状图：不同 PC mode 的 L2 events 均约 35/round。
- 最后再放 cache probe 图，作为 PMU 结果的落点确认，而非逆向主结果。
