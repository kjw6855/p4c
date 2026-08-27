import os
import re
import sys
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import statsmodels.api as sm

# =========================
# Configuration
# =========================
# CSV path + output dir may be overridden on the CLI: plot_stat.py [CSV] [OUTPUT_DIR]
CSV_FILE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/Workspace-remote/usenix27/metrics/metrics.csv")
OUTPUT_DIR = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
    "~/Workspace-remote/usenix27/metrics/plots")
ARCH_COLORS = {
    "v1model": "red",
    "tna": "blue",
}
CONFIDENCE_ALPHA = 0.05  # 95% confidence interval

os.makedirs(OUTPUT_DIR, exist_ok=True)

# =========================
# Load and clean data
# =========================
df = pd.read_csv(CSV_FILE, comment='#')

# Normalize column names slightly to avoid hidden whitespace / NBSP issues
df.columns = [str(c).replace("\xa0", " ").strip() for c in df.columns]

# Normalize arch values
df["arch"] = df["arch"].astype(str).str.strip().str.lower()

# Keep only the two requested architectures
df = df[df["arch"].isin(["v1model", "tna"])].copy()

# Derived columns requested in the plots
df["A2S_node_plus_edge"] = pd.to_numeric(df["A2S_node"], errors="coerce") + pd.to_numeric(df["A2S_edge"], errors="coerce")
df["A2S_node_cubed_times_edge"] = (pd.to_numeric(df["A2S_node"], errors="coerce") ** 3) * pd.to_numeric(df["A2S_edge"], errors="coerce")
df["S2V_node_plus_edge"] = pd.to_numeric(df["S2V_node"], errors="coerce") + pd.to_numeric(df["S2V_edge"], errors="coerce")
df["S2V_node_cubed_times_edge"] = (pd.to_numeric(df["S2V_node"], errors="coerce") ** 3) * pd.to_numeric(df["S2V_edge"], errors="coerce")

# =========================
# Plot specifications
# =========================
plot_specs = [
    ("lines_of_code", "Total (ms)", "1_lines_of_code_vs_Total_ms"),
    ("lines_of_code", "P4SD (ms)", "2_lines_of_code_vs_P4SD_ms"),
    ("lines_of_code", "P4SD.Act->SO (ms)", "3_lines_of_code_vs_P4SD_Act_to_SO_ms"),
    ("A2S_node", "P4SD.Act->SO (ms)", "4_A2S_node_vs_P4SD_Act_to_SO_ms"),
    ("A2S_node_plus_edge", "P4SD.Act->SO (ms)", "5_A2S_node_plus_edge_vs_P4SD_Act_to_SO_ms"),
    ("A2S_node_cubed_times_edge", "P4SD.Act->SO (ms)", "6_A2S_node3_times_edge_vs_P4SD_Act_to_SO_ms"),
    ("S2V_node", "P4SD.SO->KEY/HDR (ms)", "7_S2V_node_vs_P4SD_SO_to_KEY_HDR_ms"),
    ("S2V_node_plus_edge", "P4SD.SO->KEY/HDR (ms)", "8_S2V_node_plus_edge_vs_P4SD_SO_to_KEY_HDR_ms"),
    ("S2V_node_cubed_times_edge", "P4SD.SO->KEY/HDR (ms)", "9_S2V_node3_times_edge_vs_P4SD_SO_to_KEY_HDR_ms"),
]

# =========================
# Helpers
# =========================
def sanitize_filename(name: str) -> str:
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", name)
    return name.strip("_")

def fit_and_draw(ax, subdf, x_col, y_col, color, label):
    tmp = subdf[[x_col, y_col]].copy()
    tmp[x_col] = pd.to_numeric(tmp[x_col], errors="coerce")
    tmp[y_col] = pd.to_numeric(tmp[y_col], errors="coerce")
    tmp = tmp.dropna()

    if len(tmp) == 0:
        return None

    x = tmp[x_col].to_numpy(dtype=float)
    y = tmp[y_col].to_numpy(dtype=float)

    # Scatter points
    ax.scatter(x, y, color=color, alpha=0.75, s=40, edgecolors="black", linewidths=0.4, label=f"{label} data")

    # Need at least 2 points for a fitted line
    if len(tmp) < 2 or np.allclose(x, x[0]):
        return None

    X = sm.add_constant(x)
    model = sm.OLS(y, X).fit()

    x_grid = np.linspace(np.min(x), np.max(x), 200)
    X_grid = sm.add_constant(x_grid)

    pred = model.get_prediction(X_grid).summary_frame(alpha=CONFIDENCE_ALPHA)

    y_mean = pred["mean"].to_numpy()
    y_low = pred["mean_ci_lower"].to_numpy()
    y_high = pred["mean_ci_upper"].to_numpy()

    # Trend line
    ax.plot(x_grid, y_mean, color=color, linewidth=2.0, label=f"{label} trend")

    # Confidence band
    ax.fill_between(x_grid, y_low, y_high, color=color, alpha=0.18, label=f"{label} 95% CI")

    return model

# =========================
# Generate plots
# =========================
for x_col, y_col, out_name in plot_specs:
    fig, ax = plt.subplots(figsize=(8, 6))

    for arch in ["v1model", "tna"]:
        subdf = df[df["arch"] == arch]
        fit_and_draw(ax, subdf, x_col, y_col, ARCH_COLORS[arch], arch)

    ax.set_xlabel(x_col)
    ax.set_ylabel(y_col)
    ax.set_title(f"{y_col} vs {x_col}")
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.legend()
    plt.tight_layout()

    out_path = os.path.join(OUTPUT_DIR, sanitize_filename(out_name) + ".png")
    plt.savefig(out_path, dpi=300, bbox_inches="tight")
    plt.close(fig)

print(f"Saved plots to: {OUTPUT_DIR}")
