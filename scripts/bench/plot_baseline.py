#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

root = Path(__file__).resolve().parents[2]
res = root / "bench" / "results"
plot_dir = res / "plots"
plot_dir.mkdir(parents=True, exist_ok=True)

df = pd.read_csv(res / "phase1_merged.csv")

# 1) p99 latency vs orders
plt.figure(figsize=(9, 5))
for s, g in df.groupby("scenario"):
    gg = g.sort_values("orders")
    plt.plot(gg["orders"], gg["p99_ns"], marker="o", label=s)
plt.xscale("log")
plt.yscale("log")
plt.xlabel("orders")
plt.ylabel("p99 latency (ns)")
plt.title("Phase1 p99 latency vs book size")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "p99_vs_orders.png", dpi=160)
plt.close()

# 2) throughput vs orders
plt.figure(figsize=(9, 5))
for s, g in df.groupby("scenario"):
    gg = g.sort_values("orders")
    plt.plot(gg["orders"], gg["ops_s"], marker="o", label=s)
plt.xscale("log")
plt.ylabel("ops/s")
plt.xlabel("orders")
plt.title("Phase1 throughput vs book size")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "ops_vs_orders.png", dpi=160)
plt.close()

# 3) roofline proxy: ops/s vs ops/byte
roof = df.dropna(subset=["ops_per_byte_proxy", "ops_s"]).copy()
plt.figure(figsize=(9, 5))
for s, g in roof.groupby("scenario"):
    plt.scatter(g["ops_per_byte_proxy"], g["ops_s"], label=s, alpha=0.8)
plt.xscale("log")
plt.yscale("log")
plt.xlabel("Operational intensity proxy (ops/byte_proxy)")
plt.ylabel("Performance (ops/s)")
plt.title("Phase1 Memory Roofline Proxy")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "roofline_proxy.png", dpi=160)
plt.close()

print(f"plots -> {plot_dir}")