# Software Prefetch Kernel-VA Cache Proof

这里有两个实验程序。

- `prefetch_any_va_demo.c`：证明用户态 `prefetcht0` 对 `PROT_NONE`、`unmapped`、kernel-half canonical VA 不触发 fault。
- `prefetch_kernel_cache_user.c` + `prefetch_kcache_probe.c`：证明用户态对一个真实内核 VA 执行 `prefetcht0` 后，内核侧读取同一条 cache line 会变快。

第二个实验才是更强的证明：它测的不是正常用户映射页，而是 kernel module 自己分配的内核 buffer。

## 原理

用户态不能直接读取内核 VA，所以不能在用户态用普通 `load` timing 证明内核 cache line 被带热。

这里让 kernel module 做观测点：

1. kernel module 分配一条内核 buffer，并把它的 kernel virtual address 通过 ioctl 返回给用户态。
2. 每轮实验先由 kernel module 对这条 cache line 执行 `clflush`。
3. baseline：kernel module 立刻计时读取这条内核 cache line，得到 cold load latency。
4. prefetch case：用户态对这个 kernel VA 执行 `prefetcht0`，然后 kernel module 计时读取同一条内核 cache line。
5. 如果 prefetch case 明显更快，就说明用户态软件预取确实把这个内核地址对应的 cache line 带近了 CPU。

同时，用户态程序还会尝试普通读取这个 kernel VA，预期结果是 `FAULT`，用来说明这个地址不是普通用户映射。

## 编译

```bash
cd /home/jiangfang/workspace/re-prefetcher/test-sw
make users
make module
```

`make users` 会生成：

- `prefetch_any_va_demo`
- `prefetch_kernel_cache_user`

`make module` 会生成：

- `prefetch_kcache_probe.ko`

## 运行强证明实验

```bash
cd /home/jiangfang/workspace/re-prefetcher/test-sw
sudo insmod prefetch_kcache_probe.ko
./prefetch_kernel_cache_user 1000
sudo rmmod prefetch_kcache_probe
```

典型输出应类似：

```text
Kernel-address software prefetch cache proof
kernel probe VA      : 0xffff...
rounds               : 1000
user load kernel VA  : FAULT

kernel load after flush   median=...
kernel hot load           median=...
after kernel prefetch     median=...
after user prefetch       median=...
```

关键现象：

- `user load kernel VA : FAULT`
- `kernel hot load` 明显低于 `kernel load after flush`
- `after kernel prefetch` 明显低于 `kernel load after flush`
- `after user prefetch` 的 median 明显低于 `kernel load after flush`

这说明用户态不能普通读取该内核地址，但用户态 `prefetcht0` 仍然让内核侧读取同一地址变快。

如果 `kernel hot load` 和 `after kernel prefetch` 明显变快，但 `after user prefetch` 没有明显变快，说明测量路径是有效的，但用户态 prefetch 没有成功把这个 kernel VA 翻译并填入 cache。优先检查当前系统是否开启了 KPTI/PTI，或者 CPU/内核配置是否阻止用户态 prefetch supervisor mapping。开启 KPTI 时，用户态 CR3 下通常没有完整内核映射，用户态 prefetch 这个 kernel VA 可能无法翻译到 kernel module 的物理 cache line。这个实验更适合在关闭 KPTI 的 Linux、实验内核、或者保留 supervisor mapping 的 gem5 环境中运行。

如果输出类似下面这样：

```text
kernel load after flush  median=308
kernel hot load          median=72
after kernel prefetch    median=72
after user prefetch      median=308
```

这不是成功证明，而是一个清晰的反例：内核侧观测链路能看到 cache 热/冷差异，但用户态对该 kernel VA 的 `prefetcht0` 没有把对应 cache line 带热。

修改并重新编译 module 后，要重新加载：

```bash
sudo rmmod prefetch_kcache_probe
sudo insmod prefetch_kcache_probe.ko
```

## 运行 non-fault 基础实验

```bash
./prefetch_any_va_demo
```

这个程序主要证明 `prefetcht0` 对不可访问 canonical VA 不触发异常。它里面的 cache timing sanity check 只针对正常用户映射页，不作为内核 VA cache-fill 证明。

## 注意

- 这个实验针对 `x86-64` 的 `prefetcht0`。
- 这里讨论的是 canonical virtual address。non-canonical 地址在 `x86-64` 上可能仍然触发异常。
- 加载 kernel module 需要 root 权限，并且会临时创建 `/dev/prefetch_kcache_probe`。
- 如果目标是 gem5，请确保用户态执行 prefetch 时的页表里仍有该 kernel VA 的 supervisor mapping；否则它只能证明“prefetch 不 fault”，不能证明 cache line 被带热。
