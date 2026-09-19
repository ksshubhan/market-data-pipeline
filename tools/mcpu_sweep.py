#!/usr/bin/env python3
"""Re-run the -mcpu sweep of tools/interference_probe.cpp and commit its output.

The sweep was first run on 7 Sep 2026 under GCC 15.2.0 in an aarch64
Ubuntu guest, and its results were recorded only as prose. This script
re-runs it and writes the raw output to evidence/, so the claim has a
committed artifact.

For each setting it compiles the probe with g++, runs it, and records
the compiler's stderr and the probe's stdout verbatim. It then compares
each destructive value with the table recorded on 7 Sep. A mismatch is
a finding, not an error: it is reported in the file and the script
exits 2 rather than refusing to write.

It also records what "g++ -mcpu=apple-m1 -Q --help=target" reports for
-mcpu and -mtune, which is the evidence for the name being recognised.

Refuses unless: run from the repo root of a clean tree, on aarch64,
with a g++ that is GCC rather than clang. Exits 1 if any build or run
fails, or if the macro and <new> disagree in any build.
"""

import datetime
import hashlib
import platform
import subprocess
import sys

PROBE = "tools/interference_probe.cpp"

# Recorded 7 Sep 2026: GCC 15.2.0, Ubuntu 25.10 aarch64 guest.
EXPECTED = [
    (None, 256),
    ("generic", 256),
    ("apple-m1", 256),
    ("neoverse-n1", 64),
    ("neoverse-v1", 64),
    ("neoverse-v2", 64),
    ("cortex-a76", 64),
    ("cortex-x3", 64),
]

out_lines = []


def emit(line=""):
    print(line, flush=True)
    out_lines.append(line)


def refuse(msg):
    print("REFUSED: " + msg, flush=True)
    sys.exit(1)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def field(stdout, name):
    for line in stdout.splitlines():
        if line.startswith(name + ": "):
            return line[len(name) + 2:]
    return None


def os_name():
    try:
        with open("/etc/os-release") as f:
            for line in f:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return "unknown"


def main():
    code, top, _ = run(["git", "rev-parse", "--show-toplevel"])
    if code != 0:
        refuse("not inside a git repository")
    code, status, _ = run(["git", "status", "--porcelain"])
    if code != 0 or status.strip() != "":
        refuse("working tree is not clean")
    if platform.machine() != "aarch64":
        refuse("machine is " + platform.machine() + ", need aarch64")
    code, version, _ = run(["g++", "--version"])
    if code != 0:
        refuse("g++ not found")
    first = version.splitlines()[0] if version else ""
    if "clang" in version.lower():
        refuse("g++ is clang: " + first)

    _, head, _ = run(["git", "rev-parse", "--short", "HEAD"])
    now = datetime.datetime.now()
    stamp = now.strftime("%Y%m%d_%H%M%S")
    out_path = ("evidence/mcpu_sweep_gcc_aarch64_"
                + now.strftime("%Y%m%d") + ".txt")

    emit("interference_probe -mcpu sweep, GCC on aarch64 — raw output")
    emit()
    emit("Script:   tools/mcpu_sweep.py")
    emit("Repo:     " + head.strip() + ", tree clean at start (git status)")
    emit("Run:      " + stamp + " (guest clock)")
    emit("Guest:    " + os_name() + ", " + platform.machine())
    emit("Kernel:   " + platform.release())
    emit("Compiler: " + first)
    emit("Probe:    " + PROBE)
    emit()
    emit("Why this file exists: the sweep was first run on 7 Sep 2026 and")
    emit("its figures were recorded only as prose. This is a re-run whose")
    emit("output is committed unmodified. Each value is compared with the")
    emit("table recorded on 7 Sep under GCC 15.2.0.")
    emit()

    failures = 0
    mismatches = 0
    summary = []

    for mcpu, expected in EXPECTED:
        label = "(none)" if mcpu is None else mcpu
        binary = "/tmp/ip_sweep_" + ("default" if mcpu is None else mcpu)
        cmd = ["g++", "-std=c++20", "-Wall", "-Wextra"]
        if mcpu is not None:
            cmd.append("-mcpu=" + mcpu)
        cmd += ["-o", binary, PROBE]

        emit("=== -mcpu " + label)
        emit("Command:  " + " ".join(cmd))
        code, cout, cerr = run(cmd)
        emit("Build:    exit " + str(code))
        emit("Stderr:   (empty)" if cerr.strip() == "" else "Stderr:")
        for line in cerr.strip().splitlines():
            emit("  | " + line)
        if code != 0:
            emit("Result:   BUILD FAILED, nothing run")
            emit()
            failures += 1
            summary.append((label, "build failed", expected, "-"))
            continue

        code, pout, perr = run([binary])
        emit("Run:      exit " + str(code))
        for line in pout.strip().splitlines():
            emit("  " + line)
        for line in perr.strip().splitlines():
            emit("  stderr| " + line)

        destructive = field(pout, "hardware_destructive_interference_size")
        agrees = field(pout, "macro agrees with <new>")
        if code != 0 or destructive is None or agrees != "yes":
            emit("Result:   RUN FAILED or macro disagrees with <new>")
            emit()
            failures += 1
            summary.append((label, str(destructive), expected, "-"))
            continue

        match = "yes" if destructive == str(expected) else "NO"
        if match != "yes":
            mismatches += 1
        summary.append((label, destructive, expected, match))
        emit()

    emit("=== -Q --help=target with -mcpu=apple-m1")
    cmd = ["g++", "-mcpu=apple-m1", "-Q", "--help=target"]
    emit("Command:  " + " ".join(cmd))
    code, qout, qerr = run(cmd)
    emit("Exit:     " + str(code))
    emit("Lines:    only -mcpu= and -mtune=, runs of whitespace collapsed")
    shown = 0
    for line in qout.splitlines():
        stripped = line.strip()
        if stripped.startswith("-mcpu=") or stripped.startswith("-mtune="):
            emit("  " + " ".join(stripped.split()))
            shown += 1
    if shown == 0:
        emit("  (no -mcpu= or -mtune= line in the output)")
    for line in qerr.strip().splitlines():
        emit("  stderr| " + line)
    emit()

    emit("=== Summary")
    emit("  -mcpu           destructive   7 Sep   matches")
    for label, got, expected, match in summary:
        emit("  " + label.ljust(15) + " " + got.ljust(13) + " "
             + str(expected).ljust(7) + " " + match)
    emit()
    emit("Build or run failures: " + str(failures))
    emit("Differences from 7 Sep: " + str(mismatches))

    text = "\n".join(out_lines) + "\n"
    with open(out_path, "w") as f:
        f.write(text)
    print()
    print("wrote " + out_path)
    print("lines " + str(len(out_lines)))
    print("md5   " + hashlib.md5(text.encode("utf-8")).hexdigest())

    if failures:
        sys.exit(1)
    if mismatches:
        sys.exit(2)


if __name__ == "__main__":
    main()
