#!/usr/bin/env python3
"""load_pcs.py -- classify RISC-V load PCs from `objdump -d` disassembly.

Two jobs:

1. Parse a non-PIE riscv64 binary and classify *every* load instruction,
   including compressed RVC forms (c.ld, c.lw, c.lwsp, c.ldsp, c.fld, ...)
   and all plain forms (lb/lh/lw/ld/lbu/lhu/lwu/flw/fld).

2. Optionally derive a *static* set of "sensitive" load PCs by intraprocedural
   argument provenance: at function entry a0..a7 are tainted with their
   argument index; `mv`/`addi`/`add` style instructions propagate the taint;
   any load whose base register is derived from a nominated argument is
   reported.  This is a conservative over-approximation and is used only as
   corroboration -- the authoritative set for the measurement is produced
   dynamically by the PROT_NONE/SIGSEGV fault probe in targets/harness.h.

Marker loads are located from their symbol: the marker function entry must be
the marker load itself, which proves that nothing (in particular no load) sits
between the call's return and the marker load PC.

Usage:
  load_pcs.py BIN --marker-begin eg_marker_call_begin \
                  --marker-after eg_marker_after_call \
                  [--sensitive FUNC=ARG]... [-o build/meta_x.json] [--show FUNC]
"""
import argparse
import json
import os
import re
import subprocess
import sys

OBJDUMP = os.environ.get("OBJDUMP", "riscv64-linux-gnu-objdump")

LOAD_MNEMONICS = {
    # plain integer / float loads
    "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "flw", "fld",
    # compressed RVC loads
    "c.ld", "c.lw", "c.ldsp", "c.lwsp", "c.fld", "c.fldsp", "c.flw", "c.flwsp",
    # Zcb compressed loads (not emitted by a plain rv64gc build, kept for safety)
    "c.lbu", "c.lh", "c.lhu", "c.lh", "c.sb",
}

RELOADS = {"lr.w", "lr.d"}          # atomics excluded on purpose (documented)

INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9._]*)\s*(.*)$")
SYM_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")

REG_RE = re.compile(r"^(x[0-9]+|a[0-7]|s[0-9]+|t[0-9]+|sp|gp|tp|ra|zero|fp)$")

NO_RD = {
    "sd", "sw", "sh", "sb", "fsd", "fsw", "fsh", "fsb", "sq",
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez",
    "blez", "bgez", "bltz", "bgtz", "bgt", "ble", "bgtu", "bleu",
    "j", "jr", "ret", "c.j", "c.jr", "c.jalr", "c.beqz", "c.bnez",
    "ecall", "ebreak", "nop", "c.nop", "fence", "fence.i", "fence.tso",
    "unimp", "mret", "sret", "wfi", "sfence.vma", "c.ebreak",
}

RD_RS1 = {
    "mv", "addi", "addiw", "andi", "ori", "xori", "slti", "sltiu",
    "slli", "srli", "srai", "slliw", "srliw", "sraiw", "sext.w", "zext.w",
    "zext.b", "zext.h", "neg", "negw", "not", "seqz", "snez",
    "c.addi", "c.addiw", "c.andi", "c.slli", "c.srli", "c.srai", "c.mv",
}

RD_RS1_RS2 = {
    "add", "sub", "and", "or", "xor", "sll", "srl", "sra", "slt", "sltu",
    "addw", "subw", "sllw", "srlw", "sraw", "mul", "mulh", "mulhu", "mulhsu",
    "div", "divu", "rem", "remu", "divw", "divuw", "remw", "remuw",
    "c.add", "c.sub", "c.and", "c.or", "c.xor", "c.addw", "c.subw",
}

EMPTY = frozenset()


def norm_reg(r):
    return "s0" if r == "fp" else r


def parse_operands(ops):
    return [o.strip() for o in ops.split(",")] if ops else []


def mem_base(ops):
    m = re.search(r"\(([a-z0-9]+)\)", ops or "")
    return norm_reg(m.group(1)) if m else None


def is_ret(mnem, ops):
    if mnem in ("ret", "jr", "c.jr"):
        return True
    if mnem in ("jalr",):
        o = parse_operands(ops)
        return bool(o) and norm_reg(o[0]) in ("zero", "x0")
    return False


class Disasm:
    def __init__(self, binary):
        out = subprocess.run(
            [OBJDUMP, "-d", "--no-show-raw-insn", binary],
            check=True, capture_output=True, text=True,
        ).stdout
        self.insns = []          # (addr, mnemonic, operands, func)
        self.funcs = {}          # name -> start address
        self._by_func = {}       # name -> list of (addr, mnemonic, operands)
        cur = None
        for line in out.splitlines():
            m = SYM_RE.match(line)
            if m:
                cur = m.group(2)
                self.funcs.setdefault(cur, int(m.group(1), 16))
                continue
            m = INSN_RE.match(line)
            if m:
                addr = int(m.group(1), 16)
                rec = (addr, m.group(2), m.group(3).strip())
                self.insns.append(rec + (cur,))
                self._by_func.setdefault(cur, []).append(rec)
        self.loads = {}
        for addr, mn, ops, fn in self.insns:
            if mn in LOAD_MNEMONICS:
                self.loads[addr] = (mn, ops, fn)
        self.rets = {
            addr for addr, mn, ops, fn in self.insns if is_ret(mn, ops)
        }

    def func_range(self, name):
        if name not in self.funcs:
            return None
        start = self.funcs[name]
        later = [v for v in self.funcs.values() if v > start]
        end = min(later) if later else 1 << 64
        return start, end

    def func_insns(self, name):
        rng = self.func_range(name)
        if not rng:
            return []
        start, end = rng
        return [i for i in self.insns if start <= i[0] < end]

    # ---- intraprocedural argument provenance -----------------------------
    def sensitive_pcs(self, name, key_args):
        """Loads in `name` whose base register derives from one of key_args."""
        taint = {"a%d" % i: frozenset([i]) for i in range(8)}
        hits = {}
        for addr, mn, ops, _fn in self.func_insns(name):
            o = parse_operands(ops)
            if mn in LOAD_MNEMONICS:
                base = mem_base(ops)
                prov = taint.get(base, EMPTY) if base else EMPTY
                if prov & key_args:
                    hits[addr] = (mn, ops, sorted(prov))
                if o and REG_RE.match(norm_reg(o[0])):
                    taint[norm_reg(o[0])] = EMPTY
                continue
            if mn in NO_RD:
                continue
            if mn in RD_RS1 and len(o) >= 2:
                taint[norm_reg(o[0])] = taint.get(norm_reg(o[1]), EMPTY)
            elif mn in RD_RS1_RS2 and len(o) >= 3:
                taint[norm_reg(o[0])] = (taint.get(norm_reg(o[1]), EMPTY) |
                                         taint.get(norm_reg(o[2]), EMPTY))
            elif o and REG_RE.match(norm_reg(o[0])):
                # jal/jalr/lui/auipc/unknown: destination is not a pointer
                taint[norm_reg(o[0])] = EMPTY
        return hits

    def marker_pc(self, name):
        """First load inside the marker symbol.

        Returns (load_pc, entry_addr, n_instructions_before, n_loads_before).
        `n_loads_before` must be 0: it is the mandatory static check that no
        load sits between the call's return and the marker load PC.
        """
        rng = self.func_range(name)
        if not rng:
            raise SystemExit("error: marker symbol %r not found" % name)
        start, end = rng
        insns = [i for i in self.insns if start <= i[0] < end]
        if not insns:
            raise SystemExit("error: marker %r has no instructions" % name)
        n_before = 0
        n_loads = 0
        for addr, mn, ops, fn in insns:
            if addr in self.loads:
                return addr, insns[0][0], n_before, n_loads
            n_before += 1
        raise SystemExit("error: marker %r contains no load" % name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--marker-begin", required=True)
    ap.add_argument("--marker-after", required=True)
    ap.add_argument("--sensitive", action="append", default=[],
                    metavar="FUNC=ARG", help="static provenance seed (repeatable)")
    ap.add_argument("--show", action="append", default=[], metavar="FUNC")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    d = Disasm(args.binary)

    begin_pc, begin_entry, begin_before, begin_loads_before = \
        d.marker_pc(args.marker_begin)
    after_pc, after_entry, after_before, after_loads_before = \
        d.marker_pc(args.marker_after)

    static = {}
    seeds = []
    for spec in args.sensitive:
        fn, _, arg = spec.rpartition("=")
        if not fn or not arg.isdigit():
            raise SystemExit("bad --sensitive %r (want FUNC=ARG)" % spec)
        idx = int(arg)
        seeds.append({"func": fn, "arg": idx})
        for pc, (mn, ops, prov) in d.sensitive_pcs(fn, frozenset([idx])).items():
            static[pc] = (mn, ops, fn)

    if args.show:
        for fn in args.show:
            rng = d.func_range(fn)
            print("== %s  [%s]" % (fn, "missing" if not rng else
                                   "0x%x..0x%x" % rng))
            for addr, mn, ops, _ in d.func_insns(fn):
                base = mem_base(ops)
                tag = ""
                if addr in d.loads:
                    tag = "   <-- LOAD base=%s" % base
                if addr in static:
                    tag += " [static-sensitive]"
                print("  %x: %-24s %-28s%s" % (addr, mn, ops, tag))

    meta = {
        "binary": os.path.abspath(args.binary),
        "marker_begin_pc": begin_pc,
        "marker_begin_entry": begin_entry,
        "marker_begin_insns_before": begin_before,
        "marker_begin_loads_before": begin_loads_before,
        "marker_after_pc": after_pc,
        "marker_after_entry": after_entry,
        "marker_after_insns_before": after_before,
        "marker_after_loads_before": after_loads_before,
        "marker_no_load_before": (begin_loads_before == 0 and
                                  after_loads_before == 0),
        "load_pcs": sorted("%x" % a for a in d.loads),
        "load_disasm": {"%x" % a: " ".join((v[0], v[1]))
                        for a, v in d.loads.items()},
        "static_sensitive_pcs": sorted("%x" % a for a in static),
        "static_seeds": seeds,
        "ret_pcs": sorted("%x" % a for a in d.rets),
    }
    txt = json.dumps(meta, indent=1)
    if args.out:
        with open(args.out, "w") as f:
            f.write(txt + "\n")
        print("wrote %s: %d load PCs, %d static sensitive PCs, "
              "marker_begin=0x%x (%d insns, %d loads before) "
              "marker_after=0x%x (%d insns, %d loads before) "
              "marker_no_load_before=%s"
              % (args.out, len(d.loads), len(static), begin_pc, begin_before,
                 begin_loads_before, after_pc, after_before,
                 after_loads_before, meta["marker_no_load_before"]))
    else:
        print(txt)


if __name__ == "__main__":
    main()
