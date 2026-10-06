ARCH_CONFIG = {
    "KP920": {
        "core": 0,
        "timer": "cntvct",
        # test0-exist calibrates its cache-probe threshold on the local CPU.
        "threshold_ns": 5,
        "support_pmu": True,
        "support_cpp_rctx": False,
    },
    "X925": {
        "support_pmu": True,
        "support_cpp_rctx": True,
        "core": 6,
        "timer": "cntvct",
        "threshold_ns": 80,
        "cross_core": {
            "train_core": 6,
            "trigger_core": 7,
        },
        "accesses": {
            "store": 3,
            "load": 20,
        },
    },
    "A725": {
        "support_pmu": True,
        "support_cpp_rctx": True,
        "core": 4,
        "timer": "cntvct",
        "threshold_ns": 120,
        "cross_core": {
            "train_core": 4,
            "trigger_core": 10,
        },
        "accesses": {
            "store": 6,
            "load": 6,
        },
    },
    "A78": {
        "support_pmu": True,
        "support_cpp_rctx": False,
        "core": 4,
        "timer": "cntvct",
        "threshold_ns": 150,
        "cross_core": {
            "train_core": 4,
            "trigger_core": 5,
        },
        "accesses": {
            "store": 3,
            "load": 2,
        },
    },
    "A55": {
        "support_pmu": True,
        "support_cpp_rctx": False,
        "core": 1,
        "timer": "gettime",
        "threshold_ns": 120,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "accesses": {
            "store": 5,
            "load": 5,
        },
    },
    "A76": {
        "support_pmu": True,
        "support_cpp_rctx": False,
        "core": 1,
        "timer": "cntvct",
        "threshold_ns": 170,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "accesses": {
            "store": 4,
            "load": 3,
        },
    },
    "A53": {
        "support_pmu": True,
        "support_cpp_rctx": False,
        "core": 0,
        "timer": "cntvct",
        "threshold_ns": 170,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "accesses": {
            "store": 3,
            "load": 3,
        },
    },
    "Zen4": {
        "support_pmu": True,
        "support_cpp_rctx": False,
        "core": 0,
        "timer": "gettime",
        "threshold_ns": 170,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "accesses": {
            "store": 3,
            "load": 3,
        },
    },
}


def arch_choices():
    return ARCH_CONFIG.keys()


def is_x86_arch(arch):
    return arch in {"x86", "Zen4"}


def default_timer_for_arch(arch):
    configured_timer = ARCH_CONFIG[arch].get("timer")
    if configured_timer is not None:
        return configured_timer
    return "gettime" if is_x86_arch(arch) else "cntvct"


def timer_define_for_arch(arch, timer):
    if timer == "gettime":
        return "-DGETTIME=1"
    if timer == "rdtsc":
        return "-DRDTSC=1"
    if timer == "cntvct":
        return "-DCNTVCT=1"
    if timer == "pmccntr":
        return "-DPMCCNTR=1"
    return None


def timer_unit_for_arch(arch, timer):
    if timer in {"rdtsc", "pmccntr"}:
        return "cycles"
    if timer == "cntvct":
        return "ticks"
    return "ns"


def apply_timer_default(args):
    if args.timer is None:
        args.timer = default_timer_for_arch(args.arch)


def apply_single_core_defaults(args):
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]


def apply_access_defaults(args):
    if args.accesses is None:
        args.accesses = ARCH_CONFIG[args.arch]["accesses"][args.access]


def apply_train_access_defaults(args):
    if args.train_accesses is None:
        args.train_accesses = ARCH_CONFIG[args.arch]["accesses"][args.access]


def apply_threshold_defaults(args):
    if hasattr(args, "hit_threshold_ns"):
        args.hit_threshold_ns_auto = args.hit_threshold_ns is None
        if args.hit_threshold_ns is None:
            args.hit_threshold_ns = ARCH_CONFIG[args.arch]["threshold_ns"]
    elif args.threshold_ns is None:
        args.threshold_ns = ARCH_CONFIG[args.arch]["threshold_ns"]


def apply_cross_core_defaults(args):
    cross_core = ARCH_CONFIG[args.arch]["cross_core"]
    if args.train_core is None:
        args.train_core = cross_core["train_core"]
    if args.trigger_core is None:
        args.trigger_core = cross_core["trigger_core"]
