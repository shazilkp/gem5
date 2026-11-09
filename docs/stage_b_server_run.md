# Stage B server run

Branch: `feat/dom`.

## 1. Get the code and build

```bash
cd /home/spectre/gem5
git fetch && git checkout feat/dom && git pull
scons build/RISCV/gem5.opt -j"$(nproc)"
```

## 2. Micro tests first (fast)

```bash
GEM5_TIMEOUT=300 bash tests/dom_stage_b/run_all.sh
```

Expected: shadow-buffer/provider unit tests, B0/B2/B3, provider hit/update
with `GEM5_PROV_CHECK`, page-fault check, and the stress tests all pass.

## 3. 50M smoke test on one ref benchmark

```bash
cd /home/spectre/gem5
SPEC=/home/spectre/spec/benchspec/CPU \
DOM_OUT=/home/spectre/results_dom_stage_b \
PRIMARY_ONLY=1 GEM5_TIMEOUT=7200 \
tests/dom_stage_b/run_spec_arms.sh \
  505.mcf_r run_base_refrate_riscv.0000 \
  mcf_r_base.riscv "inp.in" 50000000 2
```

Check the TSV at
`/home/spectre/results_dom_stage_b/spec/505.mcf_r/ipc.tsv`.
The primary arms are `base`, `dom`, `dom_prov4k`, `dom_l1_128k`,
`base_l1_128k`, `dom_l2_64k`, `base_l2_64k`.  `PRIMARY_ONLY=1` skips the
out-of-scope 65K/1 MiB arms.

## 4. Full 50M primary matrix

```bash
SPEC=/home/spectre/spec/benchspec/CPU \
RUN=run_base_refrate_riscv.0000 \
DOM_OUT=/home/spectre/results_dom_stage_b \
PRIMARY_ONLY=1 GEM5_TIMEOUT=7200 \
tests/dom_stage_b/run_spec_all.sh 50000000
```

The per-benchmark options in `run_spec_all.sh` are the test-input names.
Verify them against the ref run dir (`speccmds.cmd`); if the ref options
differ, use the server's `gen_jobs_ref.sh` commands and call
`run_spec_arms.sh` directly per benchmark.

## 5. 200M finalists

Repeat step 4 with `50000000` → `200000000` for omnetpp, mcf, lbm,
xalancbmk, leela.  Parallelize across benchmarks with `xargs -d '\n' -P 3`
if desired; keep the arms within a benchmark sequential.

## 6. Summarize

```bash
python3 tests/dom_stage_b/summarize_spec.py \
  --glob '/home/spectre/results_dom_stage_b/spec/*/ipc.tsv'
```

It prints only the realistic equal-area table:
4K provider (~64 KiB) vs 128 KiB L1 and 64 KiB L2, plus their no-DoM
baselines.  The 65K provider and 1 MiB L1 are excluded.

## 7. Validation

- Run one provider arm with `GEM5_PROV_CHECK=1` (functional read on every
  provider hit) and confirm no panic and the same program output.
- Compare the SPEC output files against the reference (SPEC `compare.cmd`
  or the server's existing comparison script), not just stdout.

## Notes

- Use a source/same-fs directory and `MEM=2GiB`; copying the run directory
  to `/tmp` previously produced mcf `read error`.
- Keep `GEM5_TIMEOUT` high enough for the 200M runs, or set it to `0` to
  disable the per-run timeout for the long arms.
- The 1 MiB L1 / 65K provider arms remain in the repo only as out-of-scope
  sensitivity; do not report them as a research signal.
