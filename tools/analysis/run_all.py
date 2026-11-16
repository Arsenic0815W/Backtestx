#!/usr/bin/env python3
"""Run every analysis script in order; exit non-zero if any fails. Scripts skip cleanly (exit 0) without the real data.

    python -I tools/analysis/run_all.py [--data DIR]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = ["data_quality.py", "niftybees_vs_index.py", "raw_vs_adjusted_signals.py", "benchmark_alpha_comparison.py"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data")
    a = ap.parse_args()
    failed = []
    for s in SCRIPTS:
        print(f"\n######## {s}")
        sys.stdout.flush()
        cmd = [sys.executable, "-I", os.path.join(HERE, s)] + (["--data", a.data] if a.data else [])
        if subprocess.run(cmd).returncode != 0:
            failed.append(s)
    if failed:
        print("\nFAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
