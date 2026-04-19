#pragma once

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
// #include <cassert>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <random>
#include <memory>
#include <fstream>


using std::string;
using std::map;
using std::vector;


/**
 * Helper function that returns a cpu_set_t with a cpu affinity mask that
 * limits execution to the single (logical) CPU core.
 *
 * @param[in]  cpu   The number of the target CPU core
 *
 * @return     The cpuset.
 */
static inline cpu_set_t build_cpuset(int cpu) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	return cpuset;
}

/**
 * Set affinity mask of the given process so that the process is executed
 * on a specific (logical) core.
 *
 * @param[in]  pid   The pid of the process to move/pin (0 = self)
 * @param[in]  cpu   The number of the target CPU core
 *
 * @return     Return value of sched_setaffinity (in glibc: 0 on success,
 *             -1 otherwise)
 */
static inline int pin_process_to_cpu(pid_t pid, int cpu) {
	cpu_set_t cpuset = build_cpuset(cpu);
	return sched_setaffinity(pid, sizeof(cpu_set_t), &cpuset);
}

#define USE_CURRENT_CPU (-1)

static inline string msr_file_path(int cpu) {
	if (cpu == USE_CURRENT_CPU) {
		cpu = sched_getcpu();
	}
	return "/dev/cpu/" + std::to_string(cpu) + "/msr";
}

static inline uint64_t rdmsr(int cpu, uint32_t msr_reg) {
	uint64_t msr_value;
	int fd = open(msr_file_path(cpu).c_str(), O_RDONLY);
	if (fd == -1) {
		printf("RDMSR open error: %s\n", strerror(errno));
		exit(1);
	}
	if (pread(fd, &msr_value, sizeof(msr_value), msr_reg) != sizeof(msr_value)) {
		printf("RDMSR pread error: %s\n", strerror(errno));
		exit(1);
	}
	close(fd);

	return msr_value;
}

static inline void wrmsr(int cpu, uint32_t msr_reg, uint64_t msr_value) {
	int fd = open(msr_file_path(cpu).c_str(), O_WRONLY);
	if (fd == -1) {
		printf("WRMSR open error: %s\n", strerror(errno));
		exit(1);
	}
	if (pwrite(fd, &msr_value, sizeof(msr_value), msr_reg) != sizeof(msr_value)) {
		printf("WRMSR pwrite error: %s\n", strerror(errno));
		exit(1);
	}
	close(fd);
}


#define INTEL_MSR_MISC_FEATURE_CONTROL (0x1A4)
/**
 * Enum to keep the bit masks to select the prefetcher we want to control
 * via MSR_MISC_FEATURE_CONTROL register on Intel CPUs.
 */
enum intel_prefetcher_t {
	INTEL_L2_HW_PREFETCHER          = 0b0001ULL,
	INTEL_L2_ADJACENT_CL_PREFETCHER = 0b0010ULL,
	INTEL_DCU_PREFETCHER            = 0b0100ULL,
	INTEL_DCU_IP_PREFETCHER         = 0b1000ULL,
  INTEL_AMP                       = 0b100000ULL,
};

/**
 * Enables or disables a specific prefetcher on Intel CPUs.
 *
 * @param[in]  cpu         The processor ID
 * @param[in]  prefetcher  The prefetcher to enable/disable.
 * @param[in]  enabled     true to enable, false to disable the prefetcher
 */
static inline void set_intel_prefetcher(int cpu, intel_prefetcher_t prefetcher, bool enabled) {
	uint32_t msr_reg = INTEL_MSR_MISC_FEATURE_CONTROL;
	uint64_t msr_value = rdmsr(cpu, msr_reg);
	if (!enabled) {
		msr_value |= prefetcher;
	} else {
		msr_value &= ~(prefetcher);
	}
	// printf("set_intel_prefetcher: Writing 0x%lx into 0x%x.\n", msr_value, msr_reg);
	wrmsr(cpu, msr_reg, msr_value);
	// assert(rdmsr(cpu, msr_reg) == msr_value);
}

void prepare(int cpu_id){
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu_id, &mask);  // 设置为只在核心1上运行

  if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {//0 is pid, self
    printf("Error!! Pin at Core %d\n", cpu_id);
    // assert(0,"Error!! sched_setaffinity.");
    exit(1);
    return;
  }else{
    printf("Success. Pin at Core %d.\n", cpu_id);
  }
  uint32_t msr_reg = INTEL_MSR_MISC_FEATURE_CONTROL;
  uint64_t msr_value = rdmsr(cpu_id, msr_reg);
  printf("Reading msr_value 0x%lx.\n", msr_value);

  set_intel_prefetcher(cpu_id, INTEL_L2_HW_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_L2_ADJACENT_CL_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_IP_PREFETCHER, true);
  // set_intel_prefetcher(cpu_id, INTEL_AMP, false);
  
  msr_value = rdmsr(cpu_id, msr_reg);
  printf("Now msr_value 0x%lx.\n", msr_value);

}
void PinCore(int cpu_id){
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu_id, &mask);  // 设置为只在核心1上运行

  if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {//0 is pid, self
    printf("sched_setaffinity");
    return;
  }else{
    // printf("Pin at Core %d.\n", cpu_id);
  }
}

void EnableStride(int cpu_id){
  uint32_t msr_reg = INTEL_MSR_MISC_FEATURE_CONTROL;
  uint64_t msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Enable Stride....Reading msr_value 0x%lx.\n", msr_value);
  set_intel_prefetcher(cpu_id, INTEL_L2_HW_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_L2_ADJACENT_CL_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_IP_PREFETCHER, true);
  msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Now msr_value 0x%lx.\n", msr_value);
}

void DisableStride(int cpu_id){
  uint32_t msr_reg = INTEL_MSR_MISC_FEATURE_CONTROL;
  uint64_t msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Disable Stride....Reading msr_value 0x%lx.\n", msr_value);
  set_intel_prefetcher(cpu_id, INTEL_L2_HW_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_L2_ADJACENT_CL_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_PREFETCHER, false);
  set_intel_prefetcher(cpu_id, INTEL_DCU_IP_PREFETCHER, false);
  msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Now msr_value 0x%lx.\n", msr_value);
}

void EnableALL(int cpu_id){
  uint32_t msr_reg = INTEL_MSR_MISC_FEATURE_CONTROL;
  uint64_t msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Enable Stride....Reading msr_value 0x%lx.\n", msr_value);
  set_intel_prefetcher(cpu_id, INTEL_L2_HW_PREFETCHER, true);
  set_intel_prefetcher(cpu_id, INTEL_L2_ADJACENT_CL_PREFETCHER, true);
  set_intel_prefetcher(cpu_id, INTEL_DCU_PREFETCHER, true);
  set_intel_prefetcher(cpu_id, INTEL_DCU_IP_PREFETCHER, true);
  msr_value = rdmsr(cpu_id, msr_reg);
  // printf("Now msr_value 0x%lx.\n", msr_value);
}

