ARCH_CONFIG = {
    "X925": {
        "core": 6,
        "cross_core": {
            "train_core": 6,
            "trigger_core": 7,
        },
        "train_accesses": {
            "store": 3,
            "load": 3,
        },
    },
    "A725": {
        "core": 4,
        "cross_core": {
            "train_core": 4,
            "trigger_core": 5,
        },
        "train_accesses": {
            "store": 7,
            "load": 7,
        },
    },
    "A78": {
        "core": 4,
        "cross_core": {
            "train_core": 4,
            "trigger_core": 5,
        },
        "train_accesses": {
            "store": 2,
            "load": 2,
        },
    },
    "A55": {
        "core": 1,
        "cross_core": {
            "train_core": 1,
            "trigger_core": 0,
        },
        "train_accesses": {
            "store": 5,
            "load": 5,
        },
    },
}


def arch_choices():
    return ARCH_CONFIG.keys()


def apply_single_core_defaults(args):
    if args.core is None:
        args.core = ARCH_CONFIG[args.arch]["core"]


def apply_train_access_defaults(args):
    if args.train_accesses is None:
        args.train_accesses = ARCH_CONFIG[args.arch]["train_accesses"][args.access]


def apply_cross_core_defaults(args):
    cross_core = ARCH_CONFIG[args.arch]["cross_core"]
    if args.train_core is None:
        args.train_core = cross_core["train_core"]
    if args.trigger_core is None:
        args.trigger_core = cross_core["trigger_core"]
