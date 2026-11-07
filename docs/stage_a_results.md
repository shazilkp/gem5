# Stage A results

DoM delayed-set coverage for exact value and pattern providers.

Status: the **v0** SPEC screen (lower bound) is complete for test/ref 50M and
ref 200M; the **v1** provenance + safe-cost probe is validated on
microbenchmarks, with the SPEC 200M v1 run still to be done on the server.

Every number here is a *screen* unless it comes from the v1 probe plus a
committed-only replay. The v0 records L1-D misses only and treats a completed
miss as committed; it is a lower bound on the delayed set and weights by the
miss latency.

## Environment

- gem5 v25.1.0.1, branch `feat/dom-stageA` from `stable c8222cc67a`.
- O3CPU, L1I/L1D 64 KiB 2-way, **no L2/L3**, `configs/deprecated/example/se.py`.
- Test screen: 512 MiB guest (the default). Ref screen: 2 GiB (mcf needs it;
  2 GiB is fine from a source/same-filesystem run dir).
- SPEC CPU2017 `run_base_test_riscv.0000` and `run_base_refrate_riscv.0000`;
  50M and 200M instruction windows (not full runs).
- v0 commits: `754ca72bd5` (probe+analyzer), `c35e76e94d` (profiler),
  `2c9a3bf3da` (region), `aab5cf7b36` (finite T2), `84ac9aba0f` (SPEC runner).
- v1 commit: `ff0d3c5833` (provenance, safe point, commit/squash).
- v1 trace: `L/V/C/X/S`; `L kind 0` = L1 miss, `kind 1` = hit on a
  still-speculative fill (a DoM miss). `V` = safe tick (ROB head); `C`/`X` =
  commit/squash. Coverage is committed-only; cost is
  `min(safe - issue, latency)`.

## Main table: v0 ref 200M

`T2` is the exact per-word committed-value table; `T3` is the exact pattern
region (zero/constant/affine). Region descriptors are **unlimited** here;
`desc` is how many were actually created.

| bench | loads | T2 @4K | T2 @65K | T3 64 B | T3 4 KiB | desc 64B / 4K |
|---|---|---|---|---|---|---|
| 505.mcf_r | 229,324 | 0.02% | 2.34% | 0.37% | 0.00% | 7,615 / 3 |
| 519.lbm_r | 12,453,218 | 0.27% | 0.27% | **21.8%** | 0.00% | **1,340,055** / 3 |
| 520.omnetpp_r | 1,585,000 | 33.9% | **69.2%** | 8.55% | 0.04% | 3,224 / 13 |
| 531.deepsjeng_r | 1,133 | 1.50% | 1.50% | 7.33% | 0.00% | 11,434,444 / 178,656 |
| 541.leela_r | 3,913 | 31.7% | **51.0%** | 1.64% | 0.20% | 26,359 / 411 |
| 548.exchange2_r | 1,160 | 3.79% | 6.12% | 8.36% | 0.43% | 693 / 6 |

## Test 50M (v0)

| bench | loads | T2 @4K | T2 @65K | T3 64 B | T3 4 KiB |
|---|---|---|---|---|---|
| mcf | 56,923 | 0.05% | 2.79% | 0.72% | 0.01% |
| lbm | 302 | 0.66% | 0.66% | 6.29% | 0.33% |
| omnetpp | 281,264 | 50.3% | 62.4% | 8.48% | 0.10% |
| deepsjeng | 1,065 | 1.60% | 1.60% | 6.85% | 0.00% |
| leela | 2,326 | 16.3% | 43.2% | 2.75% | 0.34% |
| exchange2 | 1,149 | 3.83% | 6.09% | 8.36% | 0.35% |

## Ref 50M (v0)

Only mcf changes materially; the other ref inputs share the first 50M with
the test (deepsjeng/exchange2 are bit-identical, leela/lbm differ by one or
zero stores).

| bench | loads | T2 @4K | T2 @65K | T3 64 B |
|---|---|---|---|---|
| mcf | 82,062 | 0.02% | 1.07% | 0.56% |
| lbm | 302 | 0.66% | 0.66% | 6.29% |
| omnetpp | 280,090 | 50.4% | 62.5% | 8.49% |
| deepsjeng | 1,065 | 1.60% | 1.60% | 6.85% |
| leela | 2,326 | 16.3% | 43.1% | 2.75% |
| exchange2 | 1,149 | 3.83% | 6.09% | 8.36% |

## Local xalancbmk, test 50M (v0)

- T2 finite sweep: 256 -> 3.4%, 1K -> 18.8%, 4K -> 43.1%, 16K -> 54.6%,
  65K -> 66.1%, 256K -> 74.1%, infinite -> **74.6%**.
- T3: 64 B 4.2% (1,982 descriptors), 256 B 1.4%, 4 KiB 0.06%.
- The exact table is the strong mechanism; pattern compression is not.

## Local mcf 5M/50M (v0)

- 5M: 37 MB trace; 10,468 loads; T2 @65K 2.79%; T3 64 B 1.06%.
- 50M: 341 MB; 56,923 loads; T2 infinite 3.0%, @65K 2.79%; T3 64 B 0.70%.

## v1 microbenchmarks (`run_micro.sh`)

| test | result |
|---|---|
| array_pattern per-word | 75.0% (affine 100%, zeros 50.0%) |
| array_pattern region 4K | 98.0% (affine 99.0%, zeros 97.1%) |
| spec_hit | 5 x `L kind=1`; committed 123 |
| squash | 58 x `X`; none in committed coverage |
| probe off vs on | identical: 1,302,065 cycles, IPC 1.013116 |
| determinism | v1 trace bit-identical across runs (`sha256 42f3b197...`) |
| mutation | disabling speculative-hit detection -> spec test fails (`spec=0`) |

## v1 local mcf 50M (real-workload plumbing check)

- Timing identical to the v0 screen: 25,961,569 cycles, IPC 1.925924.
- Trace 345 MB; committed 42,535, squashed 11,080, unresolved 3,466.
- kind 0 (L1 miss) 42,451; kind 1 (speculative hit) 84; spec-hit coverage
  72/84; ordinary miss coverage 2,046/42,451 at 65K.
- T2 @4K 0.19%, @65K **4.98%** (cost-weighted **8.52%**);
  T3 64 B 0.43%, 4 KiB 0.00%.
- The committed-only percentage is higher than the v0 screen (2.79%) because
  squashed/unresolved misses are excluded, not because mcf became regular.

## Caveats

- **v0 is a lower bound.** It misses speculative hits, treats completed
  misses as committed, and has no safe-point weighting. The v1 SPEC 200M is
  the valid number.
- **50M test/ref share the opening** for most benchmarks; only 200M shows
  the ref-specific behaviour (lbm: 302 -> 12.45M misses).
- **T3 descriptors are unlimited** in these tables. The 21.8% lbm 64 B result
  needs 1.34M descriptors and the 4 KiB representation is 0.00%; at a
  realistic 64K descriptor budget it collapses. The 4 KiB pattern is
  consistently near zero.
- **Tiny samples:** deepsjeng (1,133), exchange2 (1,160), leela (3,913);
  their percentages are indicative only. deepsjeng/exchange2 are store-heavy
  (159M/22M stores) and have almost no load misses.
- **Area is not yet normalized.** T2 entries are words (~16 B/entry with tag
  and value); T3 descriptors have a different cost. Compare coverage at
  equal bytes before drawing area conclusions.
- **Value compression** of the T2 table (frequent-value/BDI) is not measured;
  pattern-region compression is the settled part.

## Reading

- **Exact per-word table (T2) is the mechanism:** omnetpp 69.2% @65K and
  xalancbmk 74.6% infinite (43.1% at 4K, 66.1% at 65K); leela 51.0% (small
  sample). Capacity is the binding constraint.
- **Pattern-region compression (T3) is not competitive on real workloads:**
  best realistic result is omnetpp 8.55% (3,224 descriptors) against 69.2%
  for T2; lbm's 21.8% is a 1.34M-descriptor artifact and its page-level
  coverage is zero.
- **mcf is the negative pole** at 5M, 50M and 200M (1.07-3.0% T2, 0.4-0.7%
  T3); pointer chasing has neither exact re-reads nor patterns in this
  window.
- **lbm is negative for a practical cache** (0.27% T2, 0.00% T3 at 4 KiB).
- **ISER context:** SEED'21 reports 43% average VRC coverage of shadowed L1
  misses (26% VP), 5% above DoM and 3% above oracular VP. The T2 exact table
  is the counterpart here; the T3 pattern result does not reach that bar on
  real workloads.

## Commands

Local microbenchmarks:

```bash
bash tests/dom_stage_a/run_micro.sh
python3 -m unittest discover -s tests/dom_stage_a
```

Server screen (v0 or v1 binary, same scripts; the analyzer accepts `M` and
`L` traces):

```bash
MAXI=200000000 RES=$HOME/results_ref200_v1 bash gen_jobs_ref.sh
sed -n '1p' jobs_ref.txt | bash
tail -n +2 jobs_ref.txt | \
  taskset -c 0,2,4,6,8,10,12,14,16-31 \
  xargs -d '\n' -P 3 -I{} bash -c '{}'
```

Analysis:

```bash
for d in $RES/*/ref; do
  [ -f "$d/DONE" ] || continue
  echo "### $(basename "$(dirname "$d")")"
  python3 tools/dom_stage_a/profile_trace.py   "$d/trace.txt" --capacity 4096
  python3 tools/dom_stage_a/profile_trace.py   "$d/trace.txt" --capacity 65536
  python3 tools/dom_stage_a/profile_regions.py "$d/trace.txt" --block-size 64
  python3 tools/dom_stage_a/profile_regions.py "$d/trace.txt" --block-size 4096
done
```
