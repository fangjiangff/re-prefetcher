ANSI_GREEN = "\033[32m"
ANSI_RED = "\033[31m"
ANSI_RESET = "\033[0m"


def _yes_no(ok):
    color = ANSI_GREEN if ok else ANSI_RED
    return f"{color}{'YES' if ok else 'NO'}{ANSI_RESET}"


def _avg_for_line(rows, line):
    if not rows:
        return None
    if isinstance(rows, dict):
        row = rows.get(line)
    else:
        row = next((item for item in rows if item.get("position") == line), None)
    if row is None:
        return None
    return row.get("avg_ns")


def _avg_text(avg):
    return "missing" if avg is None else f"{avg} ns"


def _mean(values):
    return sum(values) / len(values) if values else None


def compute_cross_threshold(
    *,
    configured_threshold_ns,
    auto_threshold,
    baseline1_rows,
    baseline2_rows,
):
    if not auto_threshold:
        return configured_threshold_ns, "specified"

    trained_values = [
        row["avg_ns"]
        for row in (baseline1_rows or [])
        if row.get("role") == "trained"
    ]
    trained_avg = _mean(trained_values)
    if trained_avg is None:
        return configured_threshold_ns, "arch-fallback"

    miss_values = [
        row["avg_ns"]
        for row in (baseline2_rows or [])
        if row.get("role") not in ("trained", "trigger")
        and row.get("avg_ns") is not None
        and row["avg_ns"] > trained_avg
    ]
    miss_avg = _mean(miss_values)
    if miss_avg is None:
        return configured_threshold_ns, "arch-fallback"

    return int(round((trained_avg + miss_avg) / 2)), (
        f"auto trained_avg={trained_avg:.1f} ns miss_avg={miss_avg:.1f} ns"
    )


def reclassify_cross_rows(rows, threshold_ns):
    for row in rows or []:
        if row.get("role") in ("trained", "trigger", "predicted"):
            continue
        if row.get("probes", 0) > 0 and row.get("avg_ns", 0) <= threshold_ns:
            row["role"] = "prefetched"
        else:
            row["role"] = "cache_miss"


def print_cross_evaluation(
    *,
    predicted_line,
    threshold_ns,
    threshold_source=None,
    baseline1_rows,
    baseline2_rows,
    experiment3_rows,
    experiment4_rows,
):
    baseline1_avg = _avg_for_line(baseline1_rows, predicted_line)
    baseline2_avg = _avg_for_line(baseline2_rows, predicted_line)
    experiment3_avg = _avg_for_line(experiment3_rows, predicted_line)
    experiment4_avg = _avg_for_line(experiment4_rows, predicted_line)

    baseline1_ok = baseline1_avg is not None and baseline1_avg < threshold_ns
    baseline2_ok = baseline2_avg is not None and baseline2_avg > threshold_ns
    baselines_ok = baseline1_ok and baseline2_ok
    experiment3_prefetch = (
        experiment3_avg is not None and experiment3_avg < threshold_ns
    )
    experiment4_prefetch = (
        experiment4_avg is not None and experiment4_avg < threshold_ns
    )
    experiment3_ok = baselines_ok and experiment3_prefetch
    experiment4_ok = baselines_ok and experiment4_prefetch

    print("Cross-test evaluation:")
    if threshold_source:
        print(f"  Hit threshold: {threshold_ns} ns ({threshold_source})")
    print(
        f"  Baseline1 target line {predicted_line} cache hit "
        f"(< {threshold_ns} ns): {_yes_no(baseline1_ok)} "
        f"avg={_avg_text(baseline1_avg)}"
    )
    print(
        f"  Baseline2 target line {predicted_line} cache miss "
        f"(> {threshold_ns} ns): {_yes_no(baseline2_ok)} "
        f"avg={_avg_text(baseline2_avg)}"
    )
    print(f"  Baselines valid: {_yes_no(baselines_ok)}")
    print(
        f"  Experiment3 target line {predicted_line} prefetch "
        f"(< {threshold_ns} ns): {_yes_no(experiment3_prefetch)} "
        f"avg={_avg_text(experiment3_avg)}"
    )
    print(
        "  Experiment3 proves prefetcher state survives context switch "
        f"and is not flushed: {_yes_no(experiment3_ok)}"
    )
    print(
        f"  Experiment4 target line {predicted_line} prefetch "
        f"(< {threshold_ns} ns): {_yes_no(experiment4_prefetch)} "
        f"avg={_avg_text(experiment4_avg)}"
    )
    print(
        "  Experiment4 proves prefetcher state is shared across contexts: "
        f"{_yes_no(experiment4_ok)}"
    )
