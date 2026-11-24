#!/usr/bin/env python3
"""
F2 — behavioural conformance of the EgDiff model on SPEC CPU2017.

Runs a subset of the RISC-V SPEC CPU2017 refrate binaries under gem5 with the
EgDiff predictor and reports, per benchmark:

    coverage          = (normal + deferred predictions) / dispatched loads
    correct-coverage  = correct predictions / dispatched loads
    accuracy          = correct / (correct + incorrect)

The paper reports > 99% accuracy and roughly 22-26% coverage; this script is a
ballpark check that the model behaves like the paper's predictor, not a
reproduction of its exact numbers (different ISA, different hash, shorter
window, no SimPoints).

Usage:
    python3 experiment/egdiff_eval/spec_coverage/run_spec.py [--maxinsts N]
                                                             [--benches a,b]
                                                             [--work DIR]

Environment:
    GEM5   gem5 binary (default build/RISCV/gem5.opt at the repo root)
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
GEM5 = os.environ.get("GEM5", os.path.join(REPO, "build", "RISCV", "gem5.opt"))
CONFIG = os.path.join(REPO, "configs", "example", "egdiff.py")
SPEC = os.environ.get("SPEC", os.path.expanduser("~/cpu2017/benchspec/CPU"))

# bench: (binary under exe/, argv options, run dir suffix)
BENCHES = {
    "505.mcf":         ("mcf_r_base.riscv",         ["inp.in"], "505.mcf_r"),
    "519.lbm":         ("lbm_r_base.riscv",         ["3000", "reference.dat", "0", "0",
                                                     "100_100_130_ldc.of"], "519.lbm_r"),
    "531.deepsjeng":   ("deepsjeng_r_base.riscv",   ["ref.txt"], "531.deepsjeng_r"),
    "544.nab":         ("nab_r_base.riscv",         ["1am0", "1122214447", "122"], "544.nab_r"),
    "548.exchange2":   ("exchange2_r_base.riscv",   ["6"], "548.exchange2_r"),
    "557.xz":          ("xz_r_base.riscv",          ["input.combined.xz", "250",
                                                     "a841f68f38572a49d86226b7ff5baeb31bd19dc637a922a972b2e6d1257a890f6a544ecab967c313e370478c74f760eb229d4eef8a8d2836d233d3e9dd1430bf",
                                                     "40401484", "41217675", "7"], "557.xz_r"),
    "503.bwaves":      ("bwaves_r_base.riscv",      ["bwaves_4"], "503.bwaves_r"),
    "510.parest":      ("parest_r_base.riscv",      ["ref.prm"], "510.parest_r"),
}

STAT = r"^system\.cpu\.loadValuePredictor\.{name}\s+([0-9]+)"


def stat(stats_text, name):
    m = re.search(STAT.format(name=name), stats_text, re.M)
    return int(m.group(1)) if m else 0


def run_one(bench, maxinsts, work):
    exe, opts, rundir_name = BENCHES[bench]
    rundir = os.path.join(SPEC, rundir_name, "run", "run_base_refrate_riscv.0000")
    binary = os.path.join(SPEC, rundir_name, "exe", exe)
    out = os.path.join(work, bench)
    os.makedirs(out, exist_ok=True)

    if not os.path.isdir(rundir) or not os.path.isfile(binary):
        return {"bench": bench, "error": "missing run dir or binary"}

    cmd = [GEM5, "-d", out, CONFIG,
           "--cpu-type=O3CPU", "--caches", "--l2cache",
           f"--maxinsts={maxinsts}",
           f"--cmd={binary}", f"--options={' '.join(opts)}"]

    t0 = time.time()
    with open(os.path.join(out, "stdout"), "w") as f:
        rc = subprocess.call(cmd, cwd=rundir, stdout=f, stderr=subprocess.STDOUT)
    dt = time.time() - t0

    sp = os.path.join(out, "stats.txt")
    if rc != 0 or not os.path.isfile(sp):
        return {"bench": bench, "error": f"gem5 rc={rc}"}

    with open(sp, errors="replace") as f:
        s = f.read()

    # Architectural metrics: a prediction counts only if its load commits.
    # Dispatch-time counts are inflated on branchy code by wrong-path loads
    # (measured: 557.xz showed 1.78M dispatch predictions for 0.20M commits).
    committed = stat(s, "egdiffCommits")
    pred_committed = stat(s, "egdiffCommittedPredicted")
    incorrect = stat(s, "numPredictionIncorrect")

    # Diagnostic only: dispatch-time counts, not comparable across workloads
    # with heavy speculation.
    dispatched = stat(s, "egdiffDispatchedLoads")
    pred_spec = stat(s, "egdiffPredictions") + stat(s, "egdiffDeferredPredictions")
    nvq = stat(s, "egdiffNVQBases")

    return {
        "bench": bench,
        "committed_loads": committed,
        "pred_committed": pred_committed,
        "incorrect": incorrect,
        "coverage": pred_committed / committed if committed else 0.0,
        "accuracy": (pred_committed / (pred_committed + incorrect)
                     if (pred_committed + incorrect) else 0.0),
        "dispatched": dispatched,
        "pred_spec": pred_spec,
        "coverage_spec": pred_spec / dispatched if dispatched else 0.0,
        "nvq_base": nvq,
        "secs": dt,
        "error": "",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maxinsts", type=int, default=30_000_000)
    # Default set: benchmarks that run a real workload under SE in reasonable
    # time.  519.lbm spins in a register-only loop under SE, 503.bwaves exits
    # after ~138k instructions, 557.xz is ~8x slower because of ~2M
    # branch-misprediction squashes, and 531.deepsjeng spends its first 30M
    # instructions zeroing a 720 MB hash table (stores, almost no loads).
    # They remain selectable but are not in the default set.
    ap.add_argument("--benches",
                    default="505.mcf,544.nab,548.exchange2,510.parest")
    ap.add_argument("--work", default="/tmp/opencode/spec_f2")
    args = ap.parse_args()

    benches = [b.strip() for b in args.benches.split(",") if b.strip()]
    os.makedirs(args.work, exist_ok=True)

    rows = []
    for b in benches:
        print(f"[{b}] running {args.maxinsts // 1_000_000}M insts ...", flush=True)
        r = run_one(b, args.maxinsts, args.work)
        rows.append(r)
        if r.get("error"):
            print(f"[{b}] ERROR: {r['error']}", flush=True)
        else:
            print(f"[{b}] committed={r['committed_loads']} "
                  f"coverage={100*r['coverage']:.2f}% "
                  f"accuracy={100*r['accuracy']:.2f}% "
                  f"(spec-inflated={100*r['coverage_spec']:.2f}%, "
                  f"{r['secs']:.0f}s)", flush=True)

    csv_path = os.path.join(args.work, "spec_f2.csv")
    fields = ["bench", "committed_loads", "pred_committed", "incorrect",
              "coverage", "accuracy", "dispatched", "pred_spec",
              "coverage_spec", "nvq_base", "secs", "error"]
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in fields})

    good = [r for r in rows if not r.get("error") and r.get("dispatched")]
    print()
    print(f"{'bench':<18} {'coverage':>9} {'accuracy':>9} "
          f"{'spec-cov':>9}  (coverage/accuracy at commit; spec-cov is the "
          f"dispatch-time diagnostic)")
    for r in good:
        print(f"{r['bench']:<18} {100*r['coverage']:>8.2f}% "
              f"{100*r['accuracy']:>8.2f}% {100*r['coverage_spec']:>8.2f}%")
    if good:
        n = len(good)
        print(f"{'mean':<18} "
              f"{100*sum(r['coverage'] for r in good)/n:>8.2f}% "
              f"{100*sum(r['accuracy'] for r in good)/n:>8.2f}% "
              f"{100*sum(r['coverage_spec'] for r in good)/n:>8.2f}%")
    print(f"\nCSV: {csv_path}")


if __name__ == "__main__":
    main()
