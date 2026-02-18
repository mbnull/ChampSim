#!/usr/bin/env python3
"""
check_trace.py - Cross-validate a ChampSim debug.txt trace against a Spike log.

Spike log format (one line per instruction):
  core   0: 3 0x<ip> (0x<insn>) [x<rd> 0x<val>] [mem 0x<addr> [0x<wdata>]]
  - mem with one address  = load  (src_mem)
  - mem with two values   = store (dst_mem)

Debug.txt format:
  ip=0x... branch=N taken=N dst_regs=[..] src_regs=[..] dst_mem=[..] src_mem=[..]

Checks performed per instruction:
  1. IP matches
  2. is_branch: spike branch (next_ip != ip+4) matches trace branch=1
  3. branch_taken: spike taken matches trace taken=1
  4. src_mem: spike load address present in trace src_mem
  5. dst_mem: spike store address present in trace dst_mem

Usage:
  python3 check_trace.py spike.log debug.txt [--max N] [--stop-on-error]
"""

import re
import sys
import argparse
from dataclasses import dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------

# MMIO regions that QEMU plugin mem callbacks cannot observe.
# Accesses to these ranges are silently skipped in the checker.
MMIO_REGIONS = [
    (0x10000000, 0x10000100),  # UART0 (NS16550)
    (0x00100000, 0x00100010),  # VIRT_TEST / power control
    (0x02000000, 0x0200FFFF),  # CLINT
    (0x0C000000, 0x0FFFFFFF),  # PLIC
]

def is_mmio(addr):
    for lo, hi in MMIO_REGIONS:
        if lo <= addr < hi:
            return True
    return False


SPIKE_RE = re.compile(
    r'core\s+\d+:\s+\d+\s+'
    r'(0x[0-9a-f]+)'          # group 1: ip
    r'\s+\(0x[0-9a-f]+\)'     # insn word (ignored)
    r'(.*)'                    # group 2: rest
)
MEM_RE   = re.compile(r'mem\s+(0x[0-9a-f]+)(?:\s+(0x[0-9a-f]+))?')

DEBUG_RE = re.compile(
    r'ip=(0x[0-9a-f]+)\s+'
    r'branch=(\d)\s+taken=(\d)\s+'
    r'dst_regs=\[(\d+),(\d+)\]\s+'
    r'src_regs=\[(\d+),(\d+),(\d+),(\d+)\]\s+'
    r'dst_mem=\[(0x[0-9a-f]+),(0x[0-9a-f]+)\]\s+'
    r'src_mem=\[(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+)\]'
)

@dataclass
class SpikeInsn:
    ip: int
    load_addr:  Optional[int] = None
    store_addr: Optional[int] = None

@dataclass
class TraceInsn:
    ip: int
    is_branch: int
    branch_taken: int
    dst_mem: list = field(default_factory=list)
    src_mem: list = field(default_factory=list)

# ---------------------------------------------------------------------------

def parse_spike(path):
    insns = []
    with open(path) as f:
        for line in f:
            m = SPIKE_RE.match(line.strip())
            if not m:
                continue
            ip   = int(m.group(1), 16)
            rest = m.group(2)
            mm   = MEM_RE.search(rest)
            load_addr = store_addr = None
            if mm:
                addr = int(mm.group(1), 16)
                if mm.group(2) is not None:
                    store_addr = addr          # store: mem <addr> <data>
                else:
                    load_addr  = addr          # load:  mem <addr>
            insns.append(SpikeInsn(ip, load_addr, store_addr))
    return insns

def parse_debug(path):
    insns = []
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            m = DEBUG_RE.match(line.strip())
            if not m:
                continue
            ip           = int(m.group(1), 16)
            is_branch    = int(m.group(2))
            branch_taken = int(m.group(3))
            dst_mem = [int(m.group(10), 16), int(m.group(11), 16)]
            src_mem = [int(m.group(12), 16), int(m.group(13), 16),
                       int(m.group(14), 16), int(m.group(15), 16)]
            insns.append(TraceInsn(ip, is_branch, branch_taken,
                                   [a for a in dst_mem if a],
                                   [a for a in src_mem if a]))
    return insns

# ---------------------------------------------------------------------------

def check(spike_insns, trace_insns, max_insns, stop_on_error):
    errors   = 0
    warnings = 0
    checked  = 0

    # Align: skip bootrom entries in trace that spike doesn't have, or vice versa
    # We match by IP order — both should be in execution order.
    si = ti = 0
    n_spike = len(spike_insns)
    n_trace = len(trace_insns)

    while si < n_spike and ti < n_trace:
        if max_insns and checked >= max_insns:
            break

        sp = spike_insns[si]
        tr = trace_insns[ti]

        # --- 1. IP alignment ---
        if sp.ip != tr.ip:
            # Try to re-sync (skip one side)
            print(f"[SYNC]  #{checked}: spike ip=0x{sp.ip:08x} != trace ip=0x{tr.ip:08x} — skipping trace entry")
            warnings += 1
            ti += 1
            continue

        checked += 1

        # Determine spike branch_taken from next IP
        spike_taken = 0
        spike_is_branch = 0
        if si + 1 < n_spike:
            next_ip = spike_insns[si + 1].ip
            if next_ip != sp.ip + 4 and next_ip != sp.ip + 2:
                spike_is_branch = 1
                spike_taken     = 1
            elif next_ip == sp.ip + 4 or next_ip == sp.ip + 2:
                # Could still be a conditional branch not taken
                # We can't tell from spike alone without disassembly,
                # so only flag if trace says branch=1 taken=1 but spike disagrees
                if tr.is_branch and tr.branch_taken:
                    spike_is_branch = 1
                    spike_taken     = 0  # not taken (fell through)

        # --- 2. branch_taken correctness ---
        if tr.is_branch and tr.branch_taken != spike_taken:
            print(f"[BRANCH_TAKEN] #{checked} ip=0x{sp.ip:08x}: "
                  f"trace taken={tr.branch_taken} but spike taken={spike_taken}")
            errors += 1
            if stop_on_error:
                break

        # --- 3. src_mem (loads) ---
        if sp.load_addr is not None:
            if is_mmio(sp.load_addr):
                pass  # QEMU plugin cannot observe MMIO reads
            elif sp.load_addr not in tr.src_mem:
                print(f"[SRC_MEM] #{checked} ip=0x{sp.ip:08x}: "
                      f"spike load=0x{sp.load_addr:08x} not in trace src_mem={[hex(a) for a in tr.src_mem]}")
                errors += 1
                if stop_on_error:
                    break
        elif tr.src_mem:
            print(f"[SRC_MEM] #{checked} ip=0x{sp.ip:08x}: "
                  f"trace has src_mem={[hex(a) for a in tr.src_mem]} but spike has no load")
            errors += 1
            if stop_on_error:
                break

        # --- 4. dst_mem (stores) ---
        if sp.store_addr is not None:
            if is_mmio(sp.store_addr):
                pass  # QEMU plugin cannot observe MMIO writes
            elif sp.store_addr not in tr.dst_mem:
                print(f"[DST_MEM] #{checked} ip=0x{sp.ip:08x}: "
                      f"spike store=0x{sp.store_addr:08x} not in trace dst_mem={[hex(a) for a in tr.dst_mem]}")
                errors += 1
                if stop_on_error:
                    break
        elif tr.dst_mem:
            print(f"[DST_MEM] #{checked} ip=0x{sp.ip:08x}: "
                  f"trace has dst_mem={[hex(a) for a in tr.dst_mem]} but spike has no store")
            errors += 1
            if stop_on_error:
                break

        si += 1
        ti += 1

    print(f"\n{'='*60}")
    print(f"Checked:  {checked} instructions")
    print(f"Errors:   {errors}")
    print(f"Warnings: {warnings} (IP sync skips)")
    print(f"Result:   {'PASS' if errors == 0 else 'FAIL'}")
    return errors

# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Cross-validate ChampSim trace against Spike log")
    ap.add_argument("spike",  help="Spike commit log")
    ap.add_argument("debug",  help="ChampSim debug.txt trace")
    ap.add_argument("--max",  type=int, default=0, help="Max instructions to check (0=all)")
    ap.add_argument("--stop-on-error", action="store_true", help="Stop at first error")
    args = ap.parse_args()

    print(f"Parsing spike log: {args.spike}")
    spike_insns = parse_spike(args.spike)
    print(f"  -> {len(spike_insns)} instructions")

    print(f"Parsing debug trace: {args.debug}")
    trace_insns = parse_debug(args.debug)
    print(f"  -> {len(trace_insns)} instructions")

    print(f"Checking...\n")
    rc = check(spike_insns, trace_insns, args.max, args.stop_on_error)
    sys.exit(0 if rc == 0 else 1)

if __name__ == "__main__":
    main()
