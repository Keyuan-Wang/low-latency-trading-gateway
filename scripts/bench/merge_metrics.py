#!/usr/bin/env python3
import pandas as pd
from pathlib import Path

root = Path(__file__).resolve().parents[2]
res = root / "bench" / "results"

base = pd.read_csv(res / "phase1_baseline.csv")
perf = pd.read_csv(res / "phase1_perf_raw.csv")

# pivot perf metrics
pv = perf.pivot_table(
    index=["scenario", "orders", "levels", "iters"],
    columns="metric",
    values="value",
    aggfunc="mean"
).reset_index()

df = base.merge(pv, on=["scenario", "orders", "levels", "iters"], how="left")

# derived metrics
df["cpi"] = df["cycles"] / df["instructions"]
df["branch_miss_rate"] = df["branch-misses"] / df["branches"]
df["llc_miss_per_op"] = (df["LLC-load-misses"] + df["LLC-store-misses"]) / df["iters"]
df["bytes_per_op_proxy"] = 64.0 * df["llc_miss_per_op"]
df["ops_per_byte_proxy"] = 1.0 / df["bytes_per_op_proxy"].replace(0, pd.NA)

out = res / "phase1_merged.csv"
df.to_csv(out, index=False)
print(f"merged -> {out}")