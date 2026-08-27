# prefetch-pmu-event

这是一个用于 ARM64 预取实验的内核模块。它在指定 CPU 上配置两个 PMU
计数器，并允许 EL0 用户程序通过 `MRS` 直接读取计数值：

| CPU | Counter 4 | Counter 5 |
| --- | --- | --- |
| Cortex-X925 | `0x8154`：L1D 硬件预取 | `0x8155`：L2D 硬件预取 |
| Cortex-A725 | `0x8154`：L1D 硬件预取 | `0x8285`：L2D 硬件/软件预取 |

计数器持续运行。测试程序可在 trigger 前后分别读取一次计数器，以两次
读数之差作为测量结果。这样，训练、cache flush、cache probe 和 PMU
系统调用都可以放在测量窗口之外。

## 编译

```sh
cd re-prefetcher/kernel-modules/prefetch-pmu-event
make
make probe
```

## 加载和测试

部分内核需要先允许 EL0 读取 PMU：

```sh
sudo sysctl -w kernel.perf_user_access=1
```

选择要配置的 CPU，并将测试程序固定到同一个 CPU。例如，在本仓库使用
CPU6 上的 Cortex-X925：

```sh
sudo ./module_load.sh --cpu 6
taskset -c 6 ./probe_prefetch_pmu
```

在 CPU4 上测试 Cortex-A725：

```sh
sudo ./module_load.sh --cpu 4
taskset -c 4 ./probe_prefetch_pmu
```

模块会读取目标 CPU 的 MIDR 并自动选择事件；不支持的 CPU 会拒绝加载。

## 在测试程序中读取

```c
int fd = open(PREFETCH_PMU_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
ioctl(fd, PREFETCH_PMU_IOC_REARM);  /* 每次实验开始时调用一次 */

struct prefetch_pmu_snapshot before = prefetch_pmu_read();
trigger();
struct prefetch_pmu_snapshot after = prefetch_pmu_read();

uint32_t l1 = prefetch_pmu_delta(after.l1_hwprf, before.l1_hwprf);
uint32_t l2 = prefetch_pmu_delta(after.l2_prefetch, before.l2_prefetch);
```

## 卸载

```sh
sudo ./module_unload.sh
sudo sysctl -w kernel.perf_user_access=0   # 如果实验前的值为 0
```

## 注意事项

- 测试程序必须固定到加载模块时指定的 CPU。
- 不要在目标 CPU 上同时运行 `perf` 或其他 PMU 程序，否则计数器配置可能被覆盖。
- 最好使用隔离 CPU；测量窗口内其他进程的 EL0 事件也会被计入。
- Cortex-A725 的 `0x8285` 同时包含硬件预取和软件 `PRFM`。
- 该模块直接修改 CPU PMU 状态，仅适合受控实验环境。
