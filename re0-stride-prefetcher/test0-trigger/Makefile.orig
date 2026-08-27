CC ?= gcc

ARCH ?= A725
BIN_DIR ?= ../bin
TARGET ?= $(BIN_DIR)/test0-exist
SRC := test0-exist.c
UTIL_SRC := ../until.c

CORE ?=
PMU_DEVICE ?=
RUN_PREFIX ?=

STRIDE_BYTES ?= 320
TRAIN_STEP ?= 3
ROUNDS ?= 40000
PROBE_POSITIONS ?= 100
SINGLE_PROBE ?= 0
SINGLE_PROBE_POSITION ?= $(shell expr $(TRAIN_STEP) \* $(STRIDE_BYTES) / 64)
DUMMY_BUFFER_PAGES ?= 10
DUMMY_PRFM_PER_PAGE ?= 64
ENABLE_DUMMY_ACCESSES ?= 1
ENABLE_FLUSH_BARRIER ?= 0
RCTX_AFTER_FLUSH ?= 0
NO_TRIGGER ?= 0
CONTEXT_SWITCH_BEFORE_TRIGGER ?= 0
CONTEXT_SWITCH_YIELDS ?= 1

ACCESS ?= store

ifeq ($(ACCESS),load)
TRAIN_ACCESS_LOAD := 1
TRAIN_ACCESS_PREFETCH := 0
else ifeq ($(ACCESS),prefetch)
TRAIN_ACCESS_LOAD := 0
TRAIN_ACCESS_PREFETCH := 1
else ifeq ($(ACCESS),store)
TRAIN_ACCESS_LOAD := 0
TRAIN_ACCESS_PREFETCH := 0
else
$(error ACCESS must be store, load, or prefetch)
endif

ifeq ($(ARCH),X925)
DEFAULT_CORE := 6
ARCH_CFLAGS := -march=armv8.5-a+predres
ENABLE_CPP_RCTX := 1
PMU_CORE_X925 := 1
else ifeq ($(ARCH),A725)
DEFAULT_CORE := 4
ARCH_CFLAGS :=
ENABLE_CPP_RCTX := 0
PMU_CORE_X925 := 0
else
$(error ARCH must be A725 or X925)
endif

ifeq ($(strip $(CORE)),)
CORE := $(DEFAULT_CORE)
endif

CPPFLAGS += \
	-DSTRIDE_BYTES=$(STRIDE_BYTES) \
	-DTRAIN_STEP=$(TRAIN_STEP) \
	-DROUNDS=$(ROUNDS) \
	-DPROBE_POSITIONS=$(PROBE_POSITIONS) \
	-DSINGLE_PROBE=$(SINGLE_PROBE) \
	-DSINGLE_PROBE_POSITION=$(SINGLE_PROBE_POSITION) \
	-DENABLE_CPP_RCTX=$(ENABLE_CPP_RCTX) \
	-DPMU_CORE_X925=$(PMU_CORE_X925) \
	-DTRAIN_ACCESS_LOAD=$(TRAIN_ACCESS_LOAD) \
	-DTRAIN_ACCESS_PREFETCH=$(TRAIN_ACCESS_PREFETCH) \
	-DDUMMY_BUFFER_PAGES=$(DUMMY_BUFFER_PAGES) \
	-DDUMMY_PRFM_PER_PAGE=$(DUMMY_PRFM_PER_PAGE) \
	-DENABLE_DUMMY_ACCESSES=$(ENABLE_DUMMY_ACCESSES) \
	-DENABLE_FLUSH_BARRIER=$(ENABLE_FLUSH_BARRIER) \
	-DRCTX_AFTER_FLUSH=$(RCTX_AFTER_FLUSH) \
	-DNO_TRIGGER=$(NO_TRIGGER) \
	-DCONTEXT_SWITCH_BEFORE_TRIGGER=$(CONTEXT_SWITCH_BEFORE_TRIGGER) \
	-DCONTEXT_SWITCH_YIELDS=$(CONTEXT_SWITCH_YIELDS)

CFLAGS ?= -O0
CFLAGS += -std=gnu11 -static $(ARCH_CFLAGS)

.PHONY: all build run compare-dummy compare-flush-barrier clean help FORCE

all: build

build: $(TARGET)

$(TARGET): FORCE $(SRC) $(UTIL_SRC) ../until.h ../pmu.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(UTIL_SRC)

FORCE:

$(BIN_DIR):
	mkdir -p $@

run: build
	$(RUN_PREFIX) env PMU_DEVICE="$(PMU_DEVICE)" taskset -c $(CORE) $(TARGET)

compare-dummy:
	@echo "=== dummyAccesses disabled ==="
	$(MAKE) run ENABLE_DUMMY_ACCESSES=0
	@echo "=== dummyAccesses enabled ==="
	$(MAKE) run ENABLE_DUMMY_ACCESSES=1

compare-flush-barrier:
	@echo "=== flush barrier disabled ==="
	$(MAKE) run ENABLE_FLUSH_BARRIER=0
	@echo "=== flush barrier enabled (DSB SY + ISB) ==="
	$(MAKE) run ENABLE_FLUSH_BARRIER=1

clean:
	rm -f $(TARGET)

help:
	@echo "Build: make [ARCH=A725|X925] [ACCESS=store|load|prefetch]"
	@echo "Run:   make run [ARCH=A725|X925] [CORE=N] [PMU_DEVICE=name]"
	@echo "Compare dummy off/on: make compare-dummy ARCH=X925"
	@echo "Compare flush barrier off/on: make compare-flush-barrier ARCH=X925"
	@echo "Dummy PRFM density: make run DUMMY_BUFFER_PAGES=16 DUMMY_PRFM_PER_PAGE=32"
	@echo "Move CPP RCTX after flush: make run ARCH=X925 RCTX_AFTER_FLUSH=1 ENABLE_FLUSH_BARRIER=0"
	@echo "PMU permissions: make run RUN_PREFIX=sudo"
	@echo "Example: make run ARCH=X925 CORE=6 PMU_DEVICE=armv8_pmuv3_1 RUN_PREFIX=sudo"
