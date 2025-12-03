"""
Generate distribution graphics and per-node summary CSVs from latencies.csv.

Usage:
  python3 plot_latencies.py path/to/latencies.csv
Outputs:
  ./plots/latency_hist_kde.png
  ./plots/latency_ecdf.png
  ./plots/boxplot_by_sender.png
  ./plots/violin_by_dest.png
  ./plots/latency_stats_by_sender.csv
  ./plots/latency_stats_by_dest.csv
"""
import os
import argparse
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import numpy as np
import math

sns.set(style="whitegrid")

def ensure_outdir(d):
    os.makedirs(d, exist_ok=True)

def plot_per_sender_histograms(df, outdir):
    """Create a single figure with one histogram per sender.
    If exactly 4 senders, arrange them in a 2x2 grid (four corners).
    Otherwise use a ceil(sqrt(n)) grid."""
    senders = sorted(df["sender"].unique())
    n = len(senders)
    if n == 0:
        return

    if n == 4:
        rows, cols = 2, 2
    else:
        cols = math.ceil(math.sqrt(n))
        rows = math.ceil(n / cols)

    # sharex=True so all subplots use the same x-axis scale for easy comparison
    fig, axes = plt.subplots(rows, cols, figsize=(4 * cols, 3 * rows), sharey=True, sharex=True)

    # flatten axes array for easy indexing
    if isinstance(axes, plt.Axes):
        axes = [axes]
    else:
        axes = axes.flatten()

    # compute global x range and common bins for consistent plotting
    global_min = float(df["latency_ms"].min())
    global_max = float(df["latency_ms"].max())
    if global_min == global_max:
        # avoid zero-width bins
        global_min = max(0.0, global_min - 1.0)
        global_max = global_min + 2.0
    bins = np.linspace(global_min, global_max, 31)  # 30 bins

    for i in range(rows * cols):
        ax = axes[i]
        if i < n:
            s = senders[i]
            subdf = df[df["sender"] == s]
            sns.histplot(subdf["latency_ms"], bins=bins, kde=True, ax=ax, color=f"C{i%10}")
            ax.set_title(f"Sender {s} (n={len(subdf)})")
            ax.set_xlabel("Latency (ms)")
            ax.set_ylabel("Density")
            ax.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.6)
            # enforce identical x-limits
            ax.set_xlim(global_min, global_max)
        else:
            ax.axis("off")

    plt.tight_layout()
    outpath = os.path.join(outdir, "per_sender_histograms.png")
    fig.savefig(outpath, dpi=150)
    plt.close(fig)

def generate_plots(csvfile, outdir):
    df = pd.read_csv(csvfile)
    if "latency_ms" not in df.columns:
        raise SystemExit("latency_ms column not found in CSV")

    ensure_outdir(outdir)

    # Overall histogram + KDE
    plt.figure(figsize=(8,5))
    sns.histplot(df["latency_ms"], kde=True, stat="density", bins=60, color="C0")
    plt.xlabel("Latency (ms)")
    plt.title("Latency distribution (all packets)")
    plt.tight_layout()
    plt.savefig(os.path.join(outdir, "latency_hist_kde.png"), dpi=150)
    plt.close()

    # ECDF overall and by sender (limit number of senders plotted)
    plt.figure(figsize=(8,5))
    sns.ecdfplot(data=df, x="latency_ms", label="all", linewidth=2)
    # plot per-sender ECDF for up to 6 senders to avoid clutter
    senders = sorted(df["sender"].unique())
    for s in senders[:6]:
        sns.ecdfplot(data=df[df["sender"]==s], x="latency_ms", label=f"sender {s}", linestyle="--")

    # Use x-axis gridlines at 10 ms increments and show them
    import matplotlib.ticker as mticker
    ax = plt.gca()
    ax.xaxis.set_major_locator(mticker.MultipleLocator(10))
    ax.grid(axis='x', which='major', linestyle='--', linewidth=0.6)

    # Show ECDF y-axis as percentiles (0%..100%) in 10% increments
    y_ticks = np.linspace(0.0, 1.0, 11)
    ax.set_yticks(y_ticks)
    ax.set_yticklabels([f"{int(y*100)}%" for y in y_ticks])
    ax.grid(axis='y', which='major', linestyle='--', linewidth=0.6)
    
    # Compute percentiles and print them in a compact box at bottom-right of the plot
    percentiles = [50, 75, 90, 95, 99]
    pct_vals = np.percentile(df["latency_ms"], percentiles)
    # show latencies as whole milliseconds (rounded)
    pct_lines = [f"{p}th: {int(round(v))} ms" for p, v in zip(percentiles, pct_vals)]
    pct_text = "\n".join(pct_lines)
    bbox_props = dict(boxstyle='round', facecolor='white', alpha=0.85, edgecolor='gray')
    ax.text(0.98, 0.02, pct_text, transform=ax.transAxes, fontsize=8,
            va='bottom', ha='right', bbox=bbox_props)

    plt.xlabel("Latency (ms)")
    plt.ylabel("ECDF")
    plt.title("ECDF: overall and per-sender (up to 6 senders)")
    # place legend in the upper-left so it doesn't overlap the percentile box in the lower-right
    ax.legend(loc='upper left', fontsize=8)
    plt.tight_layout()
    plt.savefig(os.path.join(outdir, "latency_ecdf.png"), dpi=150)
    plt.close()

    # Boxplot by sender
    plt.figure(figsize=(10,6))
    sns.boxplot(x="sender", y="latency_ms", data=df, palette="Set3")
    plt.xlabel("Sender")
    plt.ylabel("Latency (ms)")
    plt.title("Latency by sender (boxplot)")
    plt.tight_layout()
    plt.savefig(os.path.join(outdir, "boxplot_by_sender.png"), dpi=150)
    plt.close()

    # Violin by destination (if many, limit to top 8 dests by count)
    dest_counts = df["dest"].value_counts()
    top_dests = dest_counts.index.tolist()[:8]
    plt.figure(figsize=(10,6))
    sns.violinplot(x="dest", y="latency_ms", data=df[df["dest"].isin(top_dests)], palette="Set2")
    plt.xlabel("Destination (top 8)")
    plt.ylabel("Latency (ms)")
    plt.title("Latency distribution by destination (violin)")
    plt.tight_layout()
    plt.savefig(os.path.join(outdir, "violin_by_dest.png"), dpi=150)
    plt.close()

    # Summary stats per sender and per destination
    agg_funcs = ["count", "mean", "median", "std", "min", "max"]
    stats_sender = df.groupby("sender")["latency_ms"].agg(agg_funcs).reset_index()
    stats_dest = df.groupby("dest")["latency_ms"].agg(agg_funcs).reset_index()

    stats_sender.to_csv(os.path.join(outdir, "latency_stats_by_sender.csv"), index=False)
    stats_dest.to_csv(os.path.join(outdir, "latency_stats_by_dest.csv"), index=False)

    # per-sender histograms (single image; 2x2 layout if 4 senders)
    plot_per_sender_histograms(df, outdir)

    print("Plots + stats written to", outdir)
    
if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("csvfile", nargs="?", default="latencies.csv", help="Path to latencies CSV")
    p.add_argument("--outdir", default="plots", help="Output directory for plots and stats")
    args = p.parse_args()

    generate_plots(args.csvfile, args.outdir)