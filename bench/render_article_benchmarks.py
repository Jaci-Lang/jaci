#!/usr/bin/env python3
"""Render the measured benchmark figures used by the VM engineering article."""

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "site/src/assets/images"

INK = "#e8edf4"
MUTED = "#9ba8b8"
GRID = "#334155"
BEFORE = "#f97316"
AFTER = "#22c55e"
ACCENT = "#60a5fa"
BACKGROUND = "#0b1020"


def style_axes(ax, title, subtitle, ylabel):
    ax.set_facecolor(BACKGROUND)
    ax.figure.set_facecolor(BACKGROUND)
    ax.set_title(title, color=INK, fontsize=22, weight="bold", loc="left", pad=24)
    ax.text(0, 1.015, subtitle, transform=ax.transAxes, color=MUTED, fontsize=11, va="bottom")
    ax.set_ylabel(ylabel, color=MUTED, fontsize=11)
    ax.tick_params(colors=MUTED, labelsize=10)
    ax.grid(axis="y", color=GRID, linewidth=0.8, alpha=0.65)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_visible(False)


def save(fig, name):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUTPUT / name, dpi=200, bbox_inches="tight", facecolor=BACKGROUND)
    plt.close(fig)


def gc_step_latency():
    # Matched Release builds: upstream Luau 0.735 and current Jaci.
    luau = np.array([19.093582, 17.794010, 31.217523, 24.190882, 16.477634, 19.811549, 19.722465])
    jaci = np.array([0.703046, 0.665502, 0.083758, 0.090655, 0.056986, 0.580563, 0.088966])

    fig, ax = plt.subplots(figsize=(10, 5.6))
    style_axes(
        ax,
        "Bounded traversal removes the strong-table GC cliff",
        "Luau 0.735 release vs Jaci; maximum 1 KiB GC-step latency; 524,288 live hash entries; seven processes per VM",
        "Maximum step latency (ms, log scale)",
    )
    ax.set_yscale("log")
    positions = [0, 1]
    bp = ax.boxplot([luau, jaci], positions=positions, widths=0.45, patch_artist=True, showfliers=False)
    for patch, color in zip(bp["boxes"], [BEFORE, AFTER]):
        patch.set_facecolor(color)
        patch.set_alpha(0.28)
        patch.set_edgecolor(color)
        patch.set_linewidth(1.6)
    for element in ["whiskers", "caps", "medians"]:
        for line in bp[element]:
            line.set_color(INK)
            line.set_linewidth(1.3)

    jitter = np.linspace(-0.12, 0.12, 7)
    ax.scatter(np.full(7, positions[0]) + jitter, luau, color=BEFORE, s=48, zorder=4, label="Luau 0.735")
    ax.scatter(np.full(7, positions[1]) + jitter, jaci, color=AFTER, s=48, zorder=4, label="Jaci")
    ax.set_xticks(positions, ["Luau 0.735", "Jaci"])
    ax.set_ylim(0.015, 30)

    for x, values, color in [(0, luau, BEFORE), (1, jaci, AFTER)]:
        median = float(np.median(values))
        ax.annotate(
            f"median max\n{median:.3f} ms",
            (x, median),
            xytext=(52, 0),
            textcoords="offset points",
            color=color,
            fontsize=11,
            weight="bold",
            va="center",
        )

    fig.text(0.10, 0.01, "Lower is better. Points are observed process maxima; the logarithmic axis keeps both distributions visible.", color=MUTED, fontsize=9)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    save(fig, "jaci-gc-step-latency-benchmark.png")


def operation_time():
    # Focused medians from the upstream-release/Jaci comparison.
    names = ["Full collection\n262K string entries", "table.clear\n524K hash slots"]
    luau = np.array([83.456, 26.625])
    jaci = np.array([78.061, 22.786])
    changes = (luau - jaci) / luau * 100

    fig, ax = plt.subplots(figsize=(10, 5.6))
    style_axes(
        ax,
        "Throughput gains are real, but smaller than the latency gain",
        "Luau release vs Jaci focused medians; whole-operation times, not individual incremental GC steps",
        "Elapsed time (ms)",
    )
    x = np.arange(len(names))
    width = 0.30
    bars_luau = ax.bar(x - width / 2, luau, width, color=BEFORE, alpha=0.86, label="Luau 0.735")
    bars_jaci = ax.bar(x + width / 2, jaci, width, color=AFTER, alpha=0.86, label="Jaci")
    ax.set_xticks(x, names)
    legend = ax.legend(frameon=False, ncol=2, loc="upper right")
    for text in legend.get_texts():
        text.set_color(INK)

    for bars in [bars_luau, bars_jaci]:
        for bar in bars:
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 1.5, f"{bar.get_height():.3f}", ha="center", color=INK, fontsize=10)
    for index, change in enumerate(changes):
        ax.text(index, max(luau[index], jaci[index]) + 8, f"{change:.1f}% faster", ha="center", color=ACCENT, fontsize=12, weight="bold")

    ax.set_ylim(0, 105)
    fig.text(0.10, 0.01, "Lower is better. Do not compare bar heights across this chart and the incremental-step chart as if they measured the same operation.", color=MUTED, fontsize=9)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    save(fig, "jaci-table-gc-throughput-benchmark.png")


def llvm_backend():
    assembly = np.array([1.148, 1.153, 1.193, 1.185, 2.065, 1.357, 1.912])
    llvm = np.array([3.532, 2.234, 1.981, 2.460, 2.042, 3.129, 3.235])
    runs = np.arange(1, 8)

    fig, ax = plt.subplots(figsize=(10, 5.6))
    style_axes(
        ax,
        "LLVM is not the faster backend yet",
        "Same numeric-loop program; seven independent timings per backend",
        "Elapsed time (ms)",
    )
    ax.plot(runs, assembly, color=ACCENT, marker="o", linewidth=2.2, markersize=7, label="Assembly backend")
    ax.plot(runs, llvm, color=BEFORE, marker="o", linewidth=2.2, markersize=7, label="LLVM backend")
    ax.axhline(np.median(assembly), color=ACCENT, linestyle="--", alpha=0.65)
    ax.axhline(np.median(llvm), color=BEFORE, linestyle="--", alpha=0.65)
    ax.text(7.08, np.median(assembly), "1.193 ms median", color=ACCENT, fontsize=10, va="center")
    ax.text(7.08, np.median(llvm), "2.460 ms median", color=BEFORE, fontsize=10, va="center")
    ax.set_xlabel("Independent run", color=MUTED, fontsize=11)
    ax.set_xticks(runs)
    ax.set_xlim(0.7, 8.25)
    ax.set_ylim(0.8, 3.9)
    legend = ax.legend(frameon=False, ncol=2, loc="upper left")
    for text in legend.get_texts():
        text.set_color(INK)
    fig.text(0.10, 0.01, "Lower is better. Dashed lines show independent medians; the current LLVM path still resumes in the VM for parts of this workload.", color=MUTED, fontsize=9)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    save(fig, "jaci-llvm-backend-benchmark.png")


if __name__ == "__main__":
    gc_step_latency()
    operation_time()
    llvm_backend()
