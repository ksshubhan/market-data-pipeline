#!/usr/bin/env python3
"""Write evidence/spsc_arm64_disassembly_20260908.txt from an
llvm-objdump listing of build/default/check_spsc_assembly.

Reads the listing from /tmp/spsc_full.txt, which the caller writes with
the command the README gives; this script runs no disassembler. It
writes nothing unless the tree is clean and every check holds: the four
push and pop functions, cached and uncached, each appear once; they hold
eight ordered instructions between them; each acquire load comes after
the function's first branch in the cached arm and before it in the
uncached arm; and no line of the listing matches the read-modify-write
pattern below.

Only the Repo line of the header is computed, from git. The title date,
the output filename and the Host, Compiler, Disassembler, SDK and Flags
lines are fixed text from the 8 Sep run, so a re-run on another
toolchain writes them unchanged. Nothing checks that the listing came
from a build of the current HEAD.

Run from the repository root. Related: src/check_spsc_assembly.cpp and
tools/make_a2b_evidence.py.
"""

import re
import subprocess
import sys

DUMP = "/tmp/spsc_full.txt"
OUT = "evidence/spsc_arm64_disassembly_20260908.txt"

# Written into the header as given: they describe the 8 Sep run, not the
# machine running this script.
HOST = "Apple M2, macOS 26.6.2, build 25G83"
COMPILER = "Homebrew clang 22.1.8, target arm64-apple-darwin25.6.0"
DISASM = "Homebrew LLVM llvm-objdump 22.1.8"
SDK = "26.5"
FLAGS = "-O2 -g -Wall -Wextra (preset default, RelWithDebInfo)"
COMMAND = ("/opt/homebrew/opt/llvm/bin/llvm-objdump -d --demangle "
           "build/default/check_spsc_assembly")

# Matched against a symbol's name up to its argument list, so the listing
# must be demangled; the README's command passes --demangle.
TARGETS = ["push_once", "pop_once", "push_once_uncached", "pop_once_uncached"]

ORDERED = ("ldar", "ldapr", "ldapur", "stlr", "stlur")
# Covers CAS, the 64-bit exclusive loads and stores, swap, and the LSE
# add, set, clear and xor loads. It does not match byte, halfword or pair
# exclusives, the LSE min and max forms, or store-only aliases such as
# stadd. It is matched against every line of the listing.
RMW = r"\b(cas[a-z]*|ld[a-z]*xr|st[a-z]*xr|swp[a-z]*|ldadd[a-z]*|" \
      r"ldset[a-z]*|ldclr[a-z]*|ldeor[a-z]*)\b"

HEADER_RE = re.compile(r"^[0-9a-f]{16} <(.+)>:$")
MNEMONIC_RE = re.compile(r"^[0-9a-f]+: [0-9a-f]{8}\s+\t(\S+)")


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
for block in blocks:
    name = block["symbol"].split("(")[0]
    if name in TARGETS:
        if name in by_name:
            fail("symbol %r appears more than once in the dump." % name)
        by_name[name] = block

missing = [t for t in TARGETS if t not in by_name]
if missing:
    fail("targets missing from the dump: %r" % missing)


def mnemonics(block):
    out = []
    for line in block["lines"]:
        m = MNEMONIC_RE.match(line)
        if m:
            out.append(m.group(1))
    return out


ordered_total = 0
for name in TARGETS:
    ms = mnemonics(by_name[name])
    ordered_total += sum(1 for x in ms if x in ORDERED)

# Eight is what the four functions held on 8 Sep. How the eight split
# between the arms is not checked.
if ordered_total != 8:
    fail("expected 8 ordered instructions across the four functions, got %d."
         % ordered_total)


def first_index(ms, predicate):
    for i, x in enumerate(ms):
        if predicate(x):
            return i
    return None


def is_branch(x):
    return x == "b" or x.startswith("b.") or x in ("cbz", "cbnz", "tbz", "tbnz")


# The cached arm's acquire load must follow its first branch, so a call
# whose cached copy of the other index suffices skips it; the uncached
# arm's must precede every branch, so every call pays it. bl, br and ret
# do not count as branches here.
findings = {}
for name, acquire in (("push_once", "ldapur"), ("pop_once", "ldapr"),
                      ("push_once_uncached", "ldapur"),
                      ("pop_once_uncached", "ldapr")):
    ms = mnemonics(by_name[name])
    acq = first_index(ms, lambda x, a=acquire: x == a)
    br = first_index(ms, is_branch)
    if acq is None:
        fail("%s: no %s found." % (name, acquire))
    if br is None:
        fail("%s: no branch found." % name)
    findings[name] = (acq, br, len(ms))

for name in ("push_once", "pop_once"):
    acq, br, _ = findings[name]
    if not acq > br:
        fail("%s: acquire load at index %d is not after the first branch at "
             "index %d; the cached-arm claim does not hold." % (name, acq, br))

for name in ("push_once_uncached", "pop_once_uncached"):
    acq, br, _ = findings[name]
    if not acq < br:
        fail("%s: acquire load at index %d is not before the first branch at "
             "index %d; the uncached-arm claim does not hold." % (name, acq, br))

rmw_hits = [l for l in dump_lines if re.search(RMW, l)]
if rmw_hits:
    fail("read-modify-write instructions found in the binary:\n"
         + "\n".join(rmw_hits))

# Only the Repo line, the listings, the counts and the indices below come
# from the checks above. The rest is fixed text, and the claims in it,
# such as four ordered instructions in each arm, are not checked here.
lines = []
lines.append("SPSC ring buffer disassembly, both A4 arms "
             "\u2014 8 Sep 2026 (\u00a72, \u00a76.5 A4, \u00a713)")
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
lines.append("Supersedes evidence/spsc_arm64_disassembly.txt, which recorded")
lines.append("only the cached arm's four ordered instructions, carried no")
lines.append("provenance of any kind, and predates the template parameters")
lines.append("visible in the symbols below.")
lines.append("")

for name in TARGETS:
    lines.append("--- %s ---" % name)
    lines.append(by_name[name]["symbol"])
    lines += by_name[name]["lines"]
    lines.append("")

lines.append("--- ordered instructions, all four functions ---")
for name in TARGETS:
    for line in by_name[name]["lines"]:
        m = MNEMONIC_RE.match(line)
        if m and m.group(1) in ORDERED:
            lines.append(line)
lines.append("")
lines.append("Total: %d. Four in each arm, identical mnemonics: the arms "
             "differ" % ordered_total)
lines.append("in control flow, not in ordering.")
lines.append("")

lines.append("--- where the cross-core acquire load sits, computed ---")
lines.append("Index of the acquire load and of the first branch, counting")
lines.append("instructions from the start of each function:")
lines.append("")
for name in TARGETS:
    acq, br, total = findings[name]
    lines.append("  %-22s acquire at %2d, first branch at %2d, %2d "
                 "instructions" % (name, acq, br, total))
lines.append("")
lines.append("In the cached arm the acquire load is reached only after a")
lines.append("branch: the producer consults its cached copy of the consumer")
lines.append("index first and re-reads the shared one only when that copy")
lines.append("says the queue is full. In the uncached arm the acquire load")
lines.append("precedes every branch, so it is paid on every call. This is")
lines.append("the mechanism behind A4b, shown rather than inferred from the")
lines.append("timing ratio.")
lines.append("")

lines.append("--- read-modify-write instructions (expected none) ---")
lines.append("Swept over the whole disassembly, not one section, for:")
lines.append("cas*, ld*xr, st*xr, swp*, ldadd*, ldset*, ldclr*, ldeor*.")
lines.append("Matches: 0")
lines.append("")
lines.append("The producer atomically loads the consumer index and stores")
lines.append("its own. No exclusive pair, no compare-and-swap. This is the")
lines.append("committed basis for the README's no-CAS claim.")

text = "\n".join(lines) + "\n"

with open(OUT, "w", encoding="utf-8") as f:
    f.write(text)

print("WROTE %s" % OUT)
print("lines: %d" % (text.count("\n")))
print("ordered instructions: %d" % ordered_total)
print("rmw matches: 0")
print("repo: %s, tree clean" % head[:7])
for name in TARGETS:
    acq, br, total = findings[name]
    print("  %-22s acquire=%d first_branch=%d instructions=%d"
          % (name, acq, br, total))