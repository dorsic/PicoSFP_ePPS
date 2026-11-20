"""Render the ideal encoder waveform from the timing in picoENC.pio.

Run with Python 3 and matplotlib; writes PNG and SVG beside this script.
This is a timing illustration, not an oscilloscope capture.
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    # Two ordinary cycles, four encoded cycles, then two ordinary cycles.
    # Every period is eight 12.5 ns PIO clocks.
    high_times = [50, 50, 25, 75, 25, 75, 50, 50]
    times, levels = [0], [1]
    for index, high in enumerate(high_times):
        start = index * 100
        times.extend([start + high, start + 100])
        levels.extend([0, 1])

    fig, axes = plt.subplots(
        2, 1, figsize=(13, 5.2), sharex=True,
        gridspec_kw={"height_ratios": [1, 2]}, layout="constrained",
    )
    fig.suptitle("10 MHz carrier with PPS duty-cycle encoding", fontsize=17, weight="bold")
    for ax in axes:
        ax.axvspan(200, 600, color="#dceefb", alpha=0.85)
        ax.set_ylim(-0.25, 1.55)
        ax.set_yticks([0, 1], ["LOW", "HIGH"])
        ax.spines[["top", "right", "left"]].set_visible(False)
        ax.grid(axis="x", alpha=0.22)
        ax.tick_params(axis="y", length=0)
    axes[0].step([0, 200, 600, 800], [0, 1, 0, 0], where="post", lw=2.5, color="#7346a0")
    axes[0].set_ylabel("PPS input", labelpad=16)
    axes[0].text(400, 1.22, "PPS HIGH · window shortened for illustration", ha="center", fontsize=10)
    axes[1].step(times, levels, where="post", lw=2.5, color="#176caa")
    axes[1].set_ylabel("Encoded carrier", labelpad=16)
    axes[1].set_xlim(0, 800)
    axes[1].set_xticks(range(0, 801, 100))
    axes[1].set_xlabel("Time (ns) · every rising-edge interval is 100 ns")
    for i, high in enumerate(high_times):
        axes[1].text(i * 100 + 50, 1.22, f"{high}% duty", ha="center", fontsize=10, weight="bold")
        axes[1].text(i * 100 + 50, -0.15, f"{high}/{100-high} ns", ha="center", va="top", fontsize=9)
    fig.supxlabel("Each encoded pair has 100 ns HIGH + 100 ns LOW over 200 ns", fontsize=10)
    for suffix in ("png", "svg"):
        fig.savefig(Path(__file__).with_name(f"pps_waveform.{suffix}"), dpi=180)
    plt.close(fig)


if __name__ == "__main__":
    main()
