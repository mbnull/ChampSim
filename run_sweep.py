#!/usr/bin/env python3
"""Sweep branch predictors across traces and generate a comparison report."""

import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

# === Configuration — edit these ===
BRANCH_PREDICTORS = ["gshare", "bimodal",]
TRACES = ["trace/rv32im_o0_10it.trace.bin"]
WARMUP = 10
SIM_INSTR = 0
BASE_CONFIG = "inorder_config.json"

ROOT = Path(__file__).resolve().parent
RESULTS_DIR = ROOT / "results" / datetime.now().strftime("%Y%m%d_%H%M%S")


def run(cmd, **kw):
    print(f"  $ {cmd}")
    return subprocess.run(cmd, shell=True, cwd=ROOT, **kw)


def build_binaries():
    """Generate config and build a binary for each branch predictor."""
    with open(ROOT / BASE_CONFIG) as f:
        base = json.load(f)

    for bp in BRANCH_PREDICTORS:
        cfg_path = RESULTS_DIR / f"config_{bp}.json"
        base["executable_name"] = f"champsim_{bp}"
        base["ooo_cpu"][0]["branch_predictor"] = bp
        cfg_path.write_text(json.dumps(base, indent=2))

        print(f"\n[BUILD] bp={bp}")
        run(f"./config.sh {cfg_path}")
        run("make clean", capture_output=True)
        r = run("make -j$(nproc)", capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  BUILD FAILED:\n{r.stderr[-500:]}")
            sys.exit(1)
        print(f"  -> bin/champsim_{bp}")


def _run_one(args):
    """Run a single simulation (called in worker process)."""
    bp, trace = args
    tname = Path(trace).stem.split(".")[0]
    tag = f"{bp}__{tname}"
    run_dir = RESULTS_DIR / tag
    run_dir.mkdir(parents=True, exist_ok=True)
    txt = run_dir / "output.txt"
    jsn = run_dir / "stats.json"
    runcmd = f"./bin/champsim_{bp} -w {WARMUP}"
    if SIM_INSTR > 0:
        runcmd += f" -i {SIM_INSTR}"
    runcmd += f" --json {jsn} {trace}"
    r = subprocess.run(
        runcmd,
        shell=True, cwd=ROOT, capture_output=True, text=True,
    )
    txt.write_text(r.stdout + r.stderr)
    shutil.copy(RESULTS_DIR / f"config_{bp}.json", run_dir / "config.json")
    return (bp, tname, txt, jsn)


def run_simulations():
    """Run all simulations in parallel."""
    jobs = [(bp, t) for bp in BRANCH_PREDICTORS for t in TRACES]
    workers = min(len(jobs), os.cpu_count() or 4)
    print(f"\nLaunching {len(jobs)} simulations with {workers} workers")

    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(_run_one, jobs))

    for bp, tname, *_ in results:
        print(f"  done: bp={bp}  trace={tname}")
    return results


def parse_stats(txt_path):
    """Extract IPC, branch accuracy, MPKI from text output."""
    text = txt_path.read_text()
    stats = {"ipc": "N/A", "acc": "N/A", "mpki": "N/A"}
    for line in text.splitlines():
        if line.startswith("CPU 0 cumulative IPC"):
            stats["ipc"] = line.split()[4]
        if "Branch Prediction Accuracy" in line:
            parts = line.split()
            for i, p in enumerate(parts):
                if p == "Accuracy:":
                    stats["acc"] = parts[i + 1]
                if p == "MPKI:":
                    stats["mpki"] = parts[i + 1]
    return stats


def generate_report(results):
    """Print and save a summary table."""
    report = RESULTS_DIR / "report.txt"
    header = f"{'Branch Predictor':<22} {'Trace':<20} {'IPC':>8} {'Accuracy':>10} {'MPKI':>8}"
    sep = "-" * len(header)

    lines = [header, sep]
    for bp, tname, txt, jsn in results:
        s = parse_stats(txt)
        lines.append(f"{bp:<22} {tname:<20} {s['ipc']:>8} {s['acc']:>10} {s['mpki']:>8}")

    text = "\n".join(lines)
    report.write_text(text + "\n")
    print(f"\n{'='*60}")
    print(text)
    print(f"{'='*60}")
    print(f"\nResults dir: {RESULTS_DIR}")


if __name__ == "__main__":
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    build_binaries()
    results = run_simulations()
    generate_report(results)
