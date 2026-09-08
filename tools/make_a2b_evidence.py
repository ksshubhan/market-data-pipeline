#!/usr/bin/env python3
"""Generate the A2b/A3b disassembly evidence file from a live objdump dump.

Verifies what it contains and writes only if every assertion holds.
Repo state is computed from git, not asserted.

Run from the repo root of a clean clone built at the current HEAD.
"""

import re
import subprocess
import sys

DUMP = "/tmp/a2b_full.txt"
OUT = "evidence/a2b_a3b_arm64_disassembly_20260908.txt"

HOST = "Apple M2, macOS 26.6.2, build 25G83"
COMPILER = "Homebrew clang 22.1.8, target arm64-apple-darwin25.6.0"
DISASM = "Homebrew LLVM llvm-objdump 22.1.8"
SDK = "26.5"
FLAGS = "-O2 -g -Wall -Wextra (preset default, RelWithDebInfo)"
COMMAND = ("/opt/homebrew/opt/llvm/bin/llvm-objdump -d "
           "build/default/check_a2b_assembly")

STORE_LOOPS = ["store_loop_16", "store_loop_64",
               "store_loop_128", "store_loop_256"]
SLOTS = ["slot_write_80", "slot_write_128", "slot_read_80", "slot_read_128"]

HEADER_RE = re.compile(r"^[0-9a-f]{16} <(.+)>:$")
INSN_RE = re.compile(r"^([0-9a-f]+): ([0-9a-f]{8})\s+\t(\S+)\s*(.*)$")

WIDTH = {"x": 8, "w": 4, "q": 16, "d": 8, "s": 4, "b": 1, "h": 2}


def fail(msg):
    print("REFUSED: " + msg)
    sys.exit(1)


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True, check=True).stdout.strip()


try:
    head = git("rev-parse", "HEAD")
    dirty = git("status", "--porcelain")
except Exception as exc:
    fail("git failed: %r" % (exc,))

if dirty:
    fail("working tree is not clean:\n" + dirty)

with open(DUMP, "r", encoding="utf-8") as f:
    dump_lines = f.read().split("\n")

blocks = []
current = None
for line in dump_lines:
    m = HEADER_RE.match(line)
    if m:
        current = {"symbol": m.group(1), "lines": []}
        blocks.append(current)
        continue
    if current is not None and line.strip():
        current["lines"].append(line)

by_name = {}
for want in STORE_LOOPS + SLOTS:
    hits = [b for b in blocks if want in b["symbol"]]
    if len(hits) != 1:
        fail("expected exactly 1 symbol containing %r, found %d."
             % (want, len(hits)))
    by_name[want] = hits[0]


def insns(block):
    out = []
    for line in block["lines"]:
        m = INSN_RE.match(line)
        if m:
            out.append({"addr": m.group(1), "enc": m.group(2),
                        "mnem": m.group(3), "ops": m.group(4)})
    return out


for name in STORE_LOOPS:
    n = sum(1 for i in insns(by_name[name]) if i["mnem"] == "stlr")
    if n != 1:
        fail("%s has %d stlr, expected exactly 1." % (name, n))


def reg_width(operand):
    operand = operand.strip().rstrip(",")
    if operand == "xzr":
        return 8
    if operand == "wzr":
        return 4
    if operand and operand[0] in WIDTH and operand[1:].isdigit():
        return WIDTH[operand[0]]
    return None


OFF_RE = re.compile(r"\[x0(?:, #(0x[0-9a-f]+|\d+))?\]")


def coverage(block):
    spans = []
    for i in insns(block):
        if i["mnem"] not in ("str", "stur", "stp"):
            continue
        m = OFF_RE.search(i["ops"])
        if not m:
            continue
        off = int(m.group(1), 0) if m.group(1) else 0
        regs = i["ops"].split("[")[0].split(",")
        widths = [reg_width(r) for r in regs if reg_width(r) is not None]
        if not widths:
            continue
        spans.append((off, sum(widths), i))
    return sorted(spans)


for name in ("slot_write_80", "slot_write_128"):
    spans = coverage(by_name[name])
    if not spans:
        fail("%s: no stores into the slot were found." % name)
    cursor = 0
    for off, width, _ in spans:
        if off != cursor:
            fail("%s: store at offset %d leaves a gap or overlap; expected %d."
                 % (name, off, cursor))
        cursor = off + width
    if cursor != 80:
        fail("%s: stores cover %d bytes, expected 80." % (name, cursor))

pairs = [("slot_write_80", "slot_write_128"), ("slot_read_80", "slot_read_128")]
for a, b in pairs:
    ea = [i["enc"] for i in insns(by_name[a])]
    eb = [i["enc"] for i in insns(by_name[b])]
    if ea != eb:
        fail("%s and %s do not have identical encodings:\n  %s\n  %s"
             % (a, b, ea, eb))

lines = []
lines.append("A2b false sharing and A3b slot stride \u2014 disassembly, "
             "8 Sep 2026 (\u00a75, \u00a76.5, \u00a713)")
lines.append("")
lines.append("Host:       " + HOST)
lines.append("Compiler:   " + COMPILER)
lines.append("Disassembler: " + DISASM)
lines.append("SDK:        " + SDK)
lines.append("Flags:      " + FLAGS)
lines.append("Repo:       %s, tree clean (verified by this script via git, "
             "not asserted)" % head[:7])
lines.append("Command:    " + COMMAND)
lines.append("")
lines.append("Supersedes evidence/a2b_arm64_disassembly.txt, which recorded")
lines.append("only the four store loops, reordered them by hand out of link")
lines.append("order, and carried no provenance. The four A3b slot functions")
lines.append("are in the same binary and were in no committed artifact at")
lines.append("all, while the README cited their disassembly as evidence.")
lines.append("")
lines.append("Symbols are left mangled and in link order, as objdump emits")
lines.append("them.")
lines.append("")

for name in STORE_LOOPS + SLOTS:
    lines.append("--- %s ---" % name)
    lines.append(by_name[name]["symbol"])
    lines += by_name[name]["lines"]
    lines.append("")

lines.append("--- A2b: one release store per loop iteration ---")
for name in STORE_LOOPS:
    n = sum(1 for i in insns(by_name[name]) if i["mnem"] == "stlr")
    lines.append("  %-16s stlr per iteration: %d" % (name, n))
lines.append("")
lines.append("Four loops, four release stores, one each. The separations")
lines.append("differ only in the layout of SeparatedCounters, so the")
lines.append("instruction streams are the same work over different strides.")
lines.append("")

lines.append("--- A3b: the write loops cover all 80 bytes, contiguously ---")
for name in ("slot_write_80", "slot_write_128"):
    lines.append("  %s:" % name)
    cursor = 0
    for off, width, insn in coverage(by_name[name]):
        lines.append("    offset %#06x  width %2d  %s %s"
                     % (off, width, insn["mnem"], insn["ops"]))
        cursor = off + width
    lines.append("    total: %d bytes, no gap" % cursor)
lines.append("")

lines.append("--- A3b: the two arms are byte-identical ---")
for a, b in pairs:
    ea = [i["enc"] for i in insns(by_name[a])]
    lines.append("  %s and %s: %d instructions, encodings identical"
                 % (a, b, len(ea)))
lines.append("")
lines.append("This is stronger than 'both loops cover all 80 bytes'. The")
lines.append("compiler emits one instruction stream for both arms, so the")
lines.append("1.81x cannot be an instruction-selection artifact. The only")
lines.append("variable is the stride the same code walks.")

text = "\n".join(lines) + "\n"

with open(OUT, "w", encoding="utf-8") as f:
    f.write(text)

print("WROTE %s" % OUT)
print("lines: %d" % (text.count("\n")))
print("repo: %s, tree clean" % head[:7])
for name in ("slot_write_80", "slot_write_128"):
    total = coverage(by_name[name])[-1]
    print("  %-16s covers %d bytes" % (name, total[0] + total[1]))
for a, b in pairs:
    print("  %s == %s: %d instructions identical"
          % (a, b, len(insns(by_name[a]))))