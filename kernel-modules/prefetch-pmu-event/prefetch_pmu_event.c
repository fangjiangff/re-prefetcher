// SPDX-License-Identifier: GPL-2.0
/*
 * Expose two continuously-running prefetch PMU event counters to EL0.
 *
 * Counter 4 is the architecture-specific L1 prefetch event and counter 5 is
 * the architecture-specific L2 prefetch event.  This is a research module:
 * Linux perf does not know these counters have been borrowed, so perf must not
 * be used on the target CPU while the module is loaded.
 */

#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/perf/arm_pmuv3.h>

#include <asm/arm_pmuv3.h>
#include <asm/cputype.h>
#include <asm/sysreg.h>

#define CORTEX_X925_PARTNUM 0xd85

#include "prefetch_pmu_ioctl.h"
#define CORTEX_A725_PARTNUM 0xd87

#ifndef ARMV8_PMU_USERENR_UEN
#define ARMV8_PMU_USERENR_UEN BIT_ULL(4)
#endif

#define L1_COUNTER_INDEX 4
#define L2_COUNTER_INDEX 5
#define L1_COUNTER_BIT BIT_ULL(L1_COUNTER_INDEX)
#define L2_COUNTER_BIT BIT_ULL(L2_COUNTER_INDEX)
#define COUNTER_MASK (L1_COUNTER_BIT | L2_COUNTER_BIT)

struct prefetch_event_config {
	u16 partnum;
	u16 l1_event;
	u16 l2_event;
	const char *name;
	const char *l2_semantics;
};

static const struct prefetch_event_config event_configs[] = {
	{
		.partnum = CORTEX_X925_PARTNUM,
		.l1_event = 0x8154,
		.l2_event = 0x8155,
		.name = "Cortex-X925",
		.l2_semantics = "L2D_CACHE_HWPRF",
	},
	{
		.partnum = CORTEX_A725_PARTNUM,
		.l1_event = 0x8154,
		.l2_event = 0x8285,
		.name = "Cortex-A725",
		.l2_semantics = "L2D_CACHE_HWPRF_OR_PRFM",
	},
};

struct prefetch_pmu_cpu_state {
	u64 original_userenr;
	u64 original_counter_enable;
	u64 original_interrupt_enable;
	u64 original_overflow;
	u64 original_l1_type;
	u64 original_l2_type;
	u64 original_l1_value;
	u64 original_l2_value;
	u16 active_l1_event;
	u16 active_l2_event;
	bool original_pmcr_enabled;
	bool configured;
};

static DEFINE_PER_CPU(struct prefetch_pmu_cpu_state, prefetch_pmu_state);
static int prefetch_pmu_hp_state = -1;

static int target_cpu = -1;
module_param(target_cpu, int, 0444);
MODULE_PARM_DESC(target_cpu, "CPU to configure (required)");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force,
	"overwrite counters 4/5 even when already enabled (unsafe, default false)");

static void prefetch_pmu_enable_el0_reads(void)
{
	u64 userenr = read_sysreg(pmuserenr_el0);

	write_sysreg((userenr | ARMV8_PMU_USERENR_ER) &
		     ~ARMV8_PMU_USERENR_UEN, pmuserenr_el0);
	isb();
}

static long prefetch_pmu_ioctl(struct file *file, unsigned int command,
			       unsigned long argument)
{
	struct prefetch_pmu_cpu_state *state;
	u64 event_filter, pmcr;
	int cpu;

	(void)file;
	(void)argument;
	if (command != PREFETCH_PMU_IOC_REARM)
		return -ENOTTY;

	cpu = get_cpu();
	state = this_cpu_ptr(&prefetch_pmu_state);
	if (cpu != target_cpu || !state->configured) {
		put_cpu();
		return -EXDEV;
	}
	event_filter = ARMV8_PMU_EXCLUDE_EL1;
	write_sysreg((u64)state->active_l1_event | event_filter,
		     pmevtyper4_el0);
	write_sysreg((u64)state->active_l2_event | event_filter,
		     pmevtyper5_el0);
	write_sysreg(COUNTER_MASK, pmintenclr_el1);
	pmcr = read_sysreg(pmcr_el0);
	write_sysreg(pmcr | ARMV8_PMU_PMCR_E, pmcr_el0);
	write_sysreg(COUNTER_MASK, pmcntenset_el0);
	prefetch_pmu_enable_el0_reads();
	put_cpu();
	return 0;
}

static const struct file_operations prefetch_pmu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = prefetch_pmu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = prefetch_pmu_ioctl,
#endif
};

static struct miscdevice prefetch_pmu_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "prefetch_pmu_event",
	.fops = &prefetch_pmu_fops,
	.mode = 0600,
};

static const struct prefetch_event_config *find_event_config(unsigned int part)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(event_configs); i++) {
		if (event_configs[i].partnum == part)
			return &event_configs[i];
	}
	return NULL;
}

static int prefetch_pmu_cpu_online(unsigned int cpu)
{
	struct prefetch_pmu_cpu_state *state;
	const struct prefetch_event_config *config;
	u64 midr, pmcr, enabled, event_filter;
	unsigned int part, counters;

	if (cpu != target_cpu)
		return 0;

	state = this_cpu_ptr(&prefetch_pmu_state);
	if (state->configured)
		return 0;

	midr = read_sysreg(midr_el1);
	part = MIDR_PARTNUM(midr);
	config = find_event_config(part);
	if (!config) {
		pr_err("prefetch_pmu_event: CPU%u unsupported MIDR part %#x\n",
		       cpu, part);
		return -ENODEV;
	}

	pmcr = read_sysreg(pmcr_el0);
	counters = (pmcr & ARMV8_PMU_PMCR_N) >> 11;
	if (counters <= L2_COUNTER_INDEX) {
		pr_err("prefetch_pmu_event: CPU%u exposes only %u event counters\n",
		       cpu, counters);
		return -EOPNOTSUPP;
	}

	enabled = read_sysreg(pmcntenset_el0);
	if ((enabled & COUNTER_MASK) && !force) {
		pr_err("prefetch_pmu_event: CPU%u counters 4/5 already enabled (%#llx); use force=1 only on an isolated research CPU\n",
		       cpu, (unsigned long long)(enabled & COUNTER_MASK));
		return -EBUSY;
	}

	state->original_userenr = read_sysreg(pmuserenr_el0);
	state->original_counter_enable = enabled & COUNTER_MASK;
	state->original_interrupt_enable =
		read_sysreg(pmintenset_el1) & COUNTER_MASK;
	state->original_overflow = read_sysreg(pmovsclr_el0) & COUNTER_MASK;
	state->original_l1_type = read_sysreg(pmevtyper4_el0);
	state->original_l2_type = read_sysreg(pmevtyper5_el0);
	state->original_l1_value = read_sysreg(pmevcntr4_el0);
	state->original_l2_value = read_sysreg(pmevcntr5_el0);
	state->original_pmcr_enabled = !!(pmcr & ARMV8_PMU_PMCR_E);
	state->active_l1_event = config->l1_event;
	state->active_l2_event = config->l2_event;

	write_sysreg(COUNTER_MASK, pmcntenclr_el0);
	write_sysreg(COUNTER_MASK, pmintenclr_el1);
	isb();

	/* Count EL0 and exclude EL1. INCLUDE_EL2 remains clear. */
	event_filter = ARMV8_PMU_EXCLUDE_EL1;
	write_sysreg((u64)config->l1_event | event_filter, pmevtyper4_el0);
	write_sysreg((u64)config->l2_event | event_filter, pmevtyper5_el0);
	write_sysreg(0, pmevcntr4_el0);
	write_sysreg(0, pmevcntr5_el0);
	write_sysreg(COUNTER_MASK, pmovsclr_el0);

	write_sysreg(pmcr | ARMV8_PMU_PMCR_E, pmcr_el0);
	write_sysreg(COUNTER_MASK, pmcntenset_el0);

	/*
	 * UEN selects PMUv3p9 fine-grained access through PMUACR_EL1. Use the
	 * legacy ER read-only path while loaded; restore the complete original
	 * PMUSERENR_EL0 value during teardown.
	 */
	prefetch_pmu_enable_el0_reads();

	state->configured = true;
	pr_info("prefetch_pmu_event: CPU%u %s part=%#x counter4=0x%04x counter5=0x%04x (%s), EL0 reads enabled\n",
		cpu, config->name, part, config->l1_event, config->l2_event,
		config->l2_semantics);
	return 0;
}

static int prefetch_pmu_cpu_offline(unsigned int cpu)
{
	struct prefetch_pmu_cpu_state *state;
	u64 pmcr, enabled;

	if (cpu != target_cpu)
		return 0;

	state = this_cpu_ptr(&prefetch_pmu_state);
	if (!state->configured)
		return 0;

	write_sysreg(COUNTER_MASK, pmcntenclr_el0);
	write_sysreg(COUNTER_MASK, pmintenclr_el1);
	isb();

	write_sysreg(state->original_l1_type, pmevtyper4_el0);
	write_sysreg(state->original_l2_type, pmevtyper5_el0);
	write_sysreg(state->original_l1_value, pmevcntr4_el0);
	write_sysreg(state->original_l2_value, pmevcntr5_el0);
	write_sysreg(COUNTER_MASK, pmovsclr_el0);
	if (state->original_overflow)
		write_sysreg(state->original_overflow, pmovsset_el0);
	if (state->original_interrupt_enable)
		write_sysreg(state->original_interrupt_enable, pmintenset_el1);
	if (state->original_counter_enable)
		write_sysreg(state->original_counter_enable, pmcntenset_el0);

	write_sysreg(state->original_userenr, pmuserenr_el0);

	/* Do not disable the global PMU while another counter is active. */
	if (!state->original_pmcr_enabled) {
		enabled = read_sysreg(pmcntenset_el0);
		if (!(enabled & ~COUNTER_MASK)) {
			pmcr = read_sysreg(pmcr_el0);
			write_sysreg(pmcr & ~ARMV8_PMU_PMCR_E, pmcr_el0);
		}
	}
	isb();

	state->configured = false;
	pr_info("prefetch_pmu_event: CPU%u restored counters 4/5 and EL0 PMU permissions\n",
		cpu);
	return 0;
}

static int __init prefetch_pmu_init(void)
{
	int ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
	    !cpu_possible(target_cpu)) {
		pr_err("prefetch_pmu_event: target_cpu=<valid CPU> is required\n");
		return -EINVAL;
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"arm64/prefetch_pmu_event:online",
				prefetch_pmu_cpu_online,
				prefetch_pmu_cpu_offline);
	if (ret < 0)
		return ret;

	prefetch_pmu_hp_state = ret;
	ret = misc_register(&prefetch_pmu_miscdev);
	if (ret) {
		cpuhp_remove_state(prefetch_pmu_hp_state);
		prefetch_pmu_hp_state = -1;
		return ret;
	}
	pr_info("prefetch_pmu_event: loaded for CPU%d; counters 4/5 are reserved by convention only\n",
		target_cpu);
	return 0;
}

static void __exit prefetch_pmu_exit(void)
{
	misc_deregister(&prefetch_pmu_miscdev);
	if (prefetch_pmu_hp_state >= 0)
		cpuhp_remove_state(prefetch_pmu_hp_state);
	pr_info("prefetch_pmu_event: unloaded\n");
}

module_init(prefetch_pmu_init);
module_exit(prefetch_pmu_exit);

MODULE_AUTHOR("re-prefetcher contributors");
MODULE_DESCRIPTION("Expose architecture-specific prefetch PMU counters to AArch64 EL0");
MODULE_LICENSE("GPL");
