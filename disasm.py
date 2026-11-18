"""Recursive-descent disassembler for GARFIELD.PRG.

Usage: uv run python disasm.py [out.s]   (prints to stdout without an argument)
Seeds: PRG entry point plus relocated longs that are immediates of code already reached.
"""
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000, Cs

ROOT = Path(__file__).resolve().parent
prg = (ROOT / "extracted/GARFIELD.PRG").read_bytes()
_, TSIZE, DSIZE, BSIZE, SSIZE = struct.unpack_from(">HIIII", prg, 0)
text = prg[28 : 28 + TSIZE + DSIZE]


def read_relocs() -> list[int]:
    off = 28 + TSIZE + DSIZE + SSIZE
    pos = struct.unpack_from(">I", prg, off)[0]
    off += 4
    out = []
    while True:
        out.append(pos)
        while True:
            b = prg[off]
            off += 1
            if b == 0:
                return out
            if b == 1:
                pos += 254
            else:
                pos += b
                break


relocs = read_relocs()
reloc_set = set(relocs)
reloc_target = {r: struct.unpack_from(">I", text, r)[0] for r in relocs}  # file holds offsets from text start

md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
md.detail = False

# Mnemonics after which execution does not fall through.
NO_FALL = {"rts", "rte", "rtr", "bra", "jmp", "bra.b", "bra.w"}


def w(a): return struct.unpack_from(">H", text, a)[0]


def flow(addr: int, mnem: str):
    """Return (branch_target|None, falls_through, is_call) using the raw opcode."""
    op = w(addr)
    t = None
    if op & 0xF000 == 0x6000:  # Bcc/BRA/BSR
        d = op & 0xFF
        t = addr + 2 + (struct.unpack_from(">h", text, addr + 2)[0] if d == 0 else (d - 256 if d > 127 else d))
        return t, op & 0xFF00 not in (0x6000,), op & 0xFF00 == 0x6100
    if op & 0xF0F8 == 0x50C8:  # DBcc
        return addr + 2 + struct.unpack_from(">h", text, addr + 2)[0], True, False
    if op in (0x4EB9, 0x4EF9):
        return struct.unpack_from(">I", text, addr + 2)[0], op == 0x4EB9, op == 0x4EB9
    if op in (0x4EBA, 0x4EFA):
        return addr + 2 + struct.unpack_from(">h", text, addr + 2)[0], op == 0x4EBA, op == 0x4EBA
    if op in (0x4E75, 0x4E73, 0x4E77) or op & 0xFFC0 == 0x4EC0:  # rts rte rtr jmp <ea>
        return None, False, False
    return None, True, False


def trace(seeds):
    insns: dict[int, tuple[int, str, str]] = {}
    work = list(seeds)
    while work:
        a = work.pop()
        while a not in insns and 0 <= a < TSIZE:
            ins = next(md.disasm(text[a : a + 10], a, 1), None)
            if ins is None:
                break
            insns[a] = (ins.size, ins.mnemonic, ins.op_str)
            t, falls, _ = flow(a, ins.mnemonic)
            if t is not None and 0 <= t < TSIZE and t not in insns:
                work.append(t)
            if not falls:
                break
            a += ins.size
    return insns


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else None  # default: stdout
    insns = trace([0])
    # Second tier: relocated immediates in reached code that point into text.
    for _ in range(8):
        new = []
        for a, (sz, m, o) in list(insns.items()):
            for r in range(a + 2, a + sz, 2):
                if r in reloc_set and reloc_target[r] < TSIZE and reloc_target[r] not in insns:
                    new.append(reloc_target[r])
        if not new:
            break
        break_ = trace(new)
        insns.update(break_)
    covered = sum(s for s, _, _ in insns.values())
    print(f"{len(insns)} instructions, {covered}/{TSIZE} bytes covered ({100*covered//TSIZE}%)", file=sys.stderr)
    labels = {0: "start"}
    for a, (sz, m, o) in insns.items():
        t, _, call = flow(a, m)
        if t is not None and 0 <= t < TSIZE:
            labels.setdefault(t, f"{'sub' if call else 'L'}_{t:05x}")
    for r in relocs:
        t = reloc_target[r]
        if t < TSIZE:
            labels.setdefault(t, f"{'loc' if t in insns else 'dat'}_{t:05x}")
    with (out.open("w") if out else sys.stdout) as f:
        a = 0
        while a < TSIZE:
            if a in labels:
                f.write(f"{labels[a]}:\n")
            if a in insns:
                sz, m, o = insns[a]
                t, _, _ = flow(a, m)
                if t in labels and a + 2 not in reloc_set:
                    o = o.rsplit("$", 1)[0] + labels[t] if "$" in o else o
                f.write(f"  {a:05x}  {text[a:a+sz].hex():<20} {m} {o}\n")
                a += sz
            else:
                end = a + 1
                while end < TSIZE and end not in insns and end not in labels and end - a < 16:
                    end += 1
                f.write(f"  {a:05x}  {'':<20} dc.b {','.join(f'${b:02x}' for b in text[a:end])}\n")
                a = end


main()
