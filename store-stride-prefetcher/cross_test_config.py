ARCH_CONFIG = {
    "X925": {
        "core": 6,
        "threshold_ns": 100,
        "cross_core": {
            "train_core": 6,
            "trigger_core": 7,
        },
        "accesses": {
            "store": 2,
            "load": 1,
        },
    },
    "A725": {
        "core": 4,
        "threshold_ns": 120,
        "cross_core": {
            "train_core": 4,
            "trigger_core": 10,
        },
        "accesses": {
            "store": 5,
            "load": 5,
        },
    },
    "A78": {
        "core": 4,
        "threshold_ns": 120,
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
        "core": 1,
        "threshold_ns": 120,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "accesses": {
            "store": 6,
            "load": 4,
        },
    },
    "A76": {
        "core": 1,
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
        "core": 0,
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
