ROLE_COLORS = {
    "trained": "#D55E00",
    "trigger": "#CC79A7",
    "predicted": "#0072B2",
    "prefetched": "#56B4E9",
    "cache_miss": "#BDBDBD",
}


def plot_cross_bar_chart(
    rows,
    *,
    args,
    trigger_line,
    predicted_line,
    train_only_accesses,
    default_title,
    trained_label,
    trigger_label,
    summary_text,
    output_path,
    no_trigger_avg_ns=None,
    title=None,
):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ModuleNotFoundError as exc:
        print(f"Skipping bar plot: missing Python package '{exc.name}'.")
        print("Install plotting dependencies with:")
        print("  sudo apt install python3-matplotlib")
        print("or, for the current Python environment:")
        print("  python3 -m pip install matplotlib")
        return

    sorted_rows = sorted(rows, key=lambda row: row["position"])
    positions = [row["position"] for row in sorted_rows]
    values = [row["avg_ns"] for row in sorted_rows]
    colors = [
        ROLE_COLORS.get(row["role"], ROLE_COLORS["cache_miss"])
        for row in sorted_rows
    ]

    width = min(max(args.probe_positions / 5, 10), 18)
    fig, ax = plt.subplots(1, 1, figsize=(width, 4))

    ax.bar(positions, values, color=colors, width=0.85,
           edgecolor="black", linewidth=0.25)
    ax.axhline(args.hit_threshold_ns, color="black",
               linestyle="--", linewidth=0.9)
    ax.axvline(trigger_line, color=ROLE_COLORS["trigger"],
               linestyle=":", linewidth=1.0)
    ax.axvline(predicted_line, color=ROLE_COLORS["predicted"],
               linestyle=":", linewidth=1.0)

    ax.set_title(title or default_title, loc="left", pad=4)
    ax.set_ylabel("Average reload ns")
    ax.set_xlabel("Probe cache-line index")
    ax.set_ylim(0, max(300, max(values) * 1.05 if values else 300))
    ax.set_xlim(-1, max(positions) + 1 if positions else args.probe_positions)
    ax.grid(axis="y", alpha=0.25)

    tick_step = max(1, args.probe_positions // 16)
    ax.set_xticks(range(0, args.probe_positions, tick_step))

    legend_items = [
        Patch(facecolor=ROLE_COLORS["trained"], edgecolor="black",
              label=trained_label),
        Patch(facecolor=ROLE_COLORS["trigger"], edgecolor="black",
              label=trigger_label),
        Patch(facecolor=ROLE_COLORS["predicted"], edgecolor="black",
              label="predicted"),
        Patch(facecolor=ROLE_COLORS["prefetched"], edgecolor="black",
              label="other prefetched"),
        Patch(facecolor=ROLE_COLORS["cache_miss"], edgecolor="black",
              label="cache_miss"),
    ]
    ax.legend(handles=legend_items, loc="upper right", frameon=False, ncol=3)

    no_trigger_text = ""
    if no_trigger_avg_ns is not None:
        no_trigger_text = (
            f", no_trigger_line{predicted_line}={no_trigger_avg_ns} ns"
        )

    fig.suptitle(
        f"{summary_text}, threshold={args.hit_threshold_ns} ns"
        f"{no_trigger_text}",
        x=0.01,
        ha="left",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(output_path, dpi=300)
    plt.close(fig)
    print(f"Saved bar chart to {output_path}")
