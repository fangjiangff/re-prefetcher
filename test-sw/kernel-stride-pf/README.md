# Kernel-VA Stride Prefetch Probe

这个简化版实验对应 `triggerThreshold-x86.cc`，但把 `array2` 放在 kernel module 里。

固定参数：

- `stride = 11 lines = 704 bytes`
- `train_step = 15`
- `probe_lines = 200`
- 每轮只 probe 一个位置：`line = round % 200`

测试组做的事情：

1. 从 `/dev/kernel_stride_pf_probe` 获取内核 `array2` 的 kernel VA。
2. 每轮让内核 flush 整个 `array2`。
3. 用户态对 `array2 + step * stride` 执行软件预取，`step = 0..14`，重复 5 次。
   - x86: `prefetcht0(kernel_va)`
   - ARM64: `prfm pldl1keep, [kernel_va]`
4. 等待一小段时间。
5. 让内核只测一个 line 的访问时间。

内核态做的事情：

- 分配 `array2`
- flush `array2`
- 测指定 line 的 load latency
- 额外提供一个正对照：内核态按同样的 `step * stride` load 访问 `array2`

## Build

```bash
cd /home/jiangfang/workspace/re-prefetcher/test-sw/kernel-stride-pf
make
```

同一套源码支持 `x86` 和 `ARM64`。在 ARM64 机器上直接用该机器的内核 headers 编译即可：

```bash
cd /home/jiangfang/workspace/re-prefetcher/test-sw/kernel-stride-pf
make
```

## Run

```bash
sudo insmod kernel_stride_pf_probe.ko
./kernel_stride_pf_user 4000
sudo rmmod kernel_stride_pf_probe
```

如果你刚刚重新编译过模块，先卸载旧模块再加载新模块：

```bash
sudo rmmod kernel_stride_pf_probe
sudo insmod kernel_stride_pf_probe.ko
```

也可以用 Python 脚本跑完整流程，包括编译、装载模块、固定核心、运行测试、绘图和卸载模块：

```bash
./run_kernel_stride_pf.py --core 0 --rounds 4000
```

脚本结束时会自动执行 `make clean`。如果需要保留 `.ko` 和用户态二进制用于调试：

```bash
./run_kernel_stride_pf.py --core 0 --rounds 4000 --keep-build
```

绘图时，脚本默认把非 `train_input` 且低于 `150` ticks/cycles 的 line 标为 `prefetched`。可以用下面参数调整阈值：

```bash
./run_kernel_stride_pf.py --core 0 --rounds 4000 --hit-threshold-cycles 120
```

脚本会生成：

- `res/kernel-stride-pf-core0-rounds4000.txt`
- `res/kernel-stride-pf-core0-rounds4000.png`

柱状图颜色：

- 橘色：显式访问过的 `train_input`
- 蓝色：非显式访问、但低于 hit threshold 的 `prefetched`
- 灰色：其余 cache miss

纵坐标固定为 `0..300` cycles。

输出字段：

```text
line    offset_bytes    avg_cycles    role
```

程序会输出两张表：

- `test: user prefetcht0(kernel_va)`：用户态对 kernel VA 发软件预取。
- ARM64 上这一项会显示为 `user prfm pldl1keep(kernel_va)`。
- `control: kernel load(array2 + step * stride)`：内核态直接 load 同一批 stride 输入，作为硬件预取器正对照。

`role` 含义：

- `train_input`：训练输入 line。测试组中是用户态显式发软件预取的 line；控制组中是内核态显式 load 的 line。
- `stride_prediction`：如果硬件 stride prefetcher 被这些 kernel VA 输入训练/触发，最可能变快的后续 stride line。
- `probe`：普通背景 line。

如果控制组里 `stride_prediction` 相比普通 `probe` 明显更快，说明这个内核 `array2` 和固定参数足以触发硬件 stride prefetcher。

如果测试组也出现 `stride_prediction` 明显更快，说明用户态对 kernel VA 的软件预取可能训练或触发了硬件预取器，并导致内核地址附近额外 cache line 被预取。

如果测试组的 `train_input` 和 `stride_prediction` 都接近普通 `probe`，则说明当前系统上用户态对 kernel VA 的软件预取既没有直接填充这些 line，也没有可观察地触发硬件 stride prefetch。

## ARM64 Notes

- 用户态软件预取使用 `prfm pldl1keep, [addr]`。
- 内核态 flush 使用 `dc civac`。
- 内核态计时使用 `cntvct_el0`。因此输出列仍叫 `avg_cycles`，但在 ARM64 上更准确地说是 virtual counter ticks；不同平台上需要重新校准 `--hit-threshold-cycles`。
- 如果系统禁止用户态访问 counter，不影响本实验，因为实际计时在内核模块里完成。
