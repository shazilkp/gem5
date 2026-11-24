# Dispatch-order vs architectural load distance

**Why this note exists.** The prevalence study and A4 measured the exit-tail
distance `d` from QEMU's *architectural* instruction trace. EgDiff indexes the
speculative-queue *allocation* order, which can also contain wrong-path loads
from mispredicted branches. This note closes that gap in two ways: it reads the
base identity out of the gem5 trace for A4, and it compares the two distances
directly on a one-call `memcmp` probe.

Nothing here changes the model; the only code additions are debug output
(`alloc sn=.. pc=..` on every slot allocation, and `base_pc`/`base_val` on each
prediction).

## 1. A4 (`crypto_verify32`): the slot really holds the key byte

Run the A4 attack with `--debug-flags=EgDiff`:

```bash
EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/a4 \
  --debug-flags=EgDiff --debug-file=/tmp/a4/simout \
  configs/example/egdiff.py --cpu-type=O3CPU --caches \
  --cmd=$PWD/tests/test-progs/egdiff_attack/poc_verify32 \
  --options="attack 24:13,25:14,26:12,27:11,28:10,29:9,30:8,31:7"
```

The prediction log now names the slot's value and, when the slot was still in
the speculative queue, its PC. For each of the eight key bytes there are 16
predictions (one per attempt) whose `base_val` is that byte, e.g.:

```
[sn:111891] deferred wakeup -> publish 0x6a to dependent PC 0x10a96
            (d=13, base_pc=0x10f58, base_val=0x6a)
[sn:4354035] deferred wakeup -> publish 0xa1 to dependent PC 0x10a96
            (d=7, base_pc=0x10f7e, base_val=0xa1)
```

`0x10f58` is `lbu a5,0(a0)` and `0x10f7e` is `lbu a0,7(a0)` in
`poc_verify32` — the key byte loads. So at dispatch, the slot at distance 7..13
held the key byte; no wrong-path load displaced it. When the base had already
committed, `base_pc=0` (the NVQ stores values, not PCs) but `base_val` is still
the key byte.

Counts per key byte (one run): `6a` 16, `db` 16, `4c` 16, `bd` 16, `2e` 16,
`8f` 16, `10` 18, `a1` 16.

## 2. `memcmp` probe: architectural d == dispatch d

`tests/test-progs/egdiff_attack/memcmp_probe.c` calls the real libc `memcmp`
and then executes the marker as the first load after the call. Two cases:
`equal` (buffers equal) and `differ` (first byte differs, so `memcmp` exits
early). The sensitive load is `0x1fdb4` inside `__memcmpeq`; the marker is
`0x106b2` in `atk_load`.

| case | architectural (QEMU) | dispatch (gem5) |
|---|---|---|
| `equal` | `d = 1` | `d = 1` |
| `differ` (early exit) | `d = 1` | `d = 1` |

The early-exit case removes loads (`2009` vs `2001` allocations), but the
distance from the *last* sensitive load to the marker stays 1 because the
final compared byte is immediately before the return.

Reproduce:

```bash
D=tests/test-progs/egdiff_attack
riscv64-linux-gnu-gcc -static -O2 -fno-pic -no-pie -fno-builtin-memcmp \
  -o /tmp/memcmp_probe $D/memcmp_probe.c

for v in equal differ; do
  qemu-riscv64 -singlestep -d exec /tmp/memcmp_probe $v 2>/tmp/q_$v.log >/dev/null
  EGDIFF_TYPE=egdiff EGDIFF_ON=1 build/RISCV/gem5.opt -d /tmp/g5_$v \
    --debug-flags=EgDiff --debug-file=/tmp/g5_$v/simout \
    configs/example/egdiff.py --cpu-type=O3CPU --caches \
    --cmd=/tmp/memcmp_probe --options=$v >/dev/null 2>&1
  echo "== $v"
  python3 $D/measure_dispatch_d.py /tmp/memcmp_probe --sensitive 0x1fdb4 \
    --qemu-log /tmp/q_$v.log --gem5-log /tmp/g5_$v/simout
done
```

Output:

```
== equal
marker atk_load = 0x106b2; sensitive = 0x1fdb4
architectural(qemu)  base=0x1fdb4 d=1
dispatch(gem5)       base=0x1fdb4 d=1
== differ
marker atk_load = 0x106b2; sensitive = 0x1fdb4
architectural(qemu)  base=0x1fdb4 d=1
dispatch(gem5)       base=0x1fdb4 d=1
```

## Interpretation and limits

- For both probes the architectural and dispatch distances coincide. In A4 there
  is no branch between the last key load and the return; in `memcmp` the
  early-exit branch changes the *number* of loads before the last sensitive
  load, not the distance from it.
- The tool detects divergence: pass any sensitive PC and compare the two lines.
  If dispatch exceeds architectural, wrong-path loads were inside the window.
- This is a one-call probe. For a branchy loop with a mispredict *inside* the
  window, dispatch `d` can exceed architectural `d`; the prevalence margin
  argument (in-window targets have `d <= 7`, i.e. >= 25 loads of margin) bounds
  the impact, and this procedure can be applied to any target that sits close
  to the boundary.
