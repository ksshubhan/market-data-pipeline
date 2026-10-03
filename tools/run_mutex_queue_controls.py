#!/usr/bin/env python3
"""Run the ten test_mutex_queue negative controls and record the results.

Each mutation is a single-occurrence text replacement in
src/mutex_queue.hpp. The script applies it, builds test_mutex_queue, runs
the suite once, or N_SCHED times for M5, M6 and M9, under a 10 s
timeout, and restores the header byte for byte before the next one. It
records each outcome and does not compare it with the predictions in
evidence/mutex_queue_wake_controls_20260919.txt.

It refuses, exit 1, unless:
  - git runs and the tree has no change and no untracked file,
  - the predictions file is tracked and still contains "PREDICTIONS
    ONLY", which its status block keeps after the results were appended
    to it, so this check shows nothing about which came first,
  - src/mutex_queue.hpp and src/test_mutex_queue.cpp match the md5s
    pinned below,
  - the unmutated suite builds and passes on every one of N_SCHED runs:
    the positive control, which shows no false FAIL from the suite's
    deadlines in those runs, not that none can occur.
Run it from the repository root. Nothing checks that: from a
subdirectory the predictions path matches nothing, and it refuses as
though the file were not committed.

Once past every refusal it writes what it printed to results/, except
the final "wrote" line. A refusal, including one after the baseline has
run, prints to stdout and writes nothing. Nothing is read from a run
whose build failed. It exits 1, after writing, if the rebuild or the run
after the header is restored fails or the tree is not clean.

Related: src/mutex_queue.hpp and src/test_mutex_queue.cpp, and the
committed results/mutex_queue_controls_*.txt.
"""

import datetime
import hashlib
import platform
import subprocess
import sys

# The mutation anchors and the predictions describe these two files as
# they stand, so both are pinned by md5 and checked before anything runs.
# Any edit to either file, comments included, changes its md5 and needs
# its pin updated here.
HEADER = "src/mutex_queue.hpp"
HEADER_MD5 = "8f403e291dfa13305e7153034f4e148a"
TEST = "src/test_mutex_queue.cpp"
TEST_MD5 = "2fbc30205d6a2e35a898a3e8d233c3bd"
PREDICTIONS = "evidence/mutex_queue_wake_controls_20260919.txt"
BINARY = "./build/default/test_mutex_queue"
BUILD = ["cmake", "--build", "--preset", "default",
         "--target", "test_mutex_queue"]
# Applied here rather than by ctest, since this runs the binary directly
# and a ctest TIMEOUT does not reach it. A failing run stops at the
# suite's 2 s deadline, well inside it.
TIMEOUT_S = 10
# Runs for the baseline and for M5, M6 and M9, whose outcome depends on
# scheduling.
N_SCHED = 100
# subprocess reports a death by SIGALRM as -14; 142 is 128 + 14, what a
# shell prints, and what the results record.
SIGALRM_EXIT = 142

MUTATIONS = [
    ("M1", "FIFO order: try_pop reads the slot after head", 1,
     "        value = buffer_[head & kMask];\n",
     "        value = buffer_[(head + 1) & kMask];\n"),

    ("M2", "Capacity: try_push treats the queue as full one early", 1,
     "        if (size == Capacity) {\n",
     "        if (size == Capacity - 1) {\n"),

    ("M3", "Rejection: full_rejections_ not incremented", 1,
     "            ++full_rejections_;\n            return false;\n",
     "            return false;\n"),

    ("M4", "Wrap: try_push writes the neighbouring slot on odd laps", 1,
     "        buffer_[tail & kMask] = value;\n",
     "        buffer_[(tail & kMask) ^ ((tail / Capacity) & 1)] = value;\n"),

    ("M5", "Wake: notify_one deleted from try_push", N_SCHED,
     "        if (was_empty) {\n            not_empty_.notify_one();\n",
     "        if (was_empty) {\n"),

    ("M6", "Close wake: notify_one deleted from close()", N_SCHED,
     "            closed_.store(true, std::memory_order_relaxed);\n"
     "        }\n\n        not_empty_.notify_one();\n",
     "            closed_.store(true, std::memory_order_relaxed);\n"
     "        }\n"),

    ("M7", "Close: wait_nonempty returns true when closed", 1,
     "        return size_hint() != 0;\n",
     "        return size_hint() != 0 ||\n"
     "               closed_.load(std::memory_order_relaxed);\n"),

    ("M8", "Parity: try_pop waits on the condvar when empty", 1,
     "        std::lock_guard<std::mutex> lock(mutex_);\n\n"
     "        const std::uint64_t head = head_.load",
     "        std::unique_lock<std::mutex> lock(mutex_);\n"
     "        not_empty_.wait(lock, [this] { return size_hint() != 0; });\n\n"
     "        const std::uint64_t head = head_.load"),

    ("M9", "Transition: notify_one on every successful push", N_SCHED,
     "        if (was_empty) {\n            not_empty_.notify_one();\n"
     "        }\n",
     "        not_empty_.notify_one();\n"),

    ("M10", "Accounting: parks_ not incremented", 1,
     "            ++parks_;\n",
     ""),
]

out_lines = []


def emit(line=""):
    print(line, flush=True)
    out_lines.append(line)


def refuse(msg):
    print("REFUSED: " + msg, flush=True)
    sys.exit(1)


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True)
    if r.returncode != 0:
        refuse("git " + " ".join(args) + " failed: " + r.stderr.strip())
    return r.stdout


def md5(data):
    return hashlib.md5(data).hexdigest()


def build():
    r = subprocess.run(BUILD, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip().splitlines()


def run_once():
    # macOS has no timeout(1). A pending alarm survives exec, so SIGALRM
    # ends the suite itself.
    r = subprocess.run(
        ["perl", "-e", "alarm shift; exec @ARGV", str(TIMEOUT_S), BINARY],
        capture_output=True, text=True)
    code = r.returncode
    if code == -14:
        code = SIGALRM_EXIT
    fail_line = ""
    for line in r.stderr.splitlines():
        if line.startswith("FAIL: "):
            fail_line = line
            break
    return code, fail_line


def main():
    # git() refuses first, with git's own error, whenever rev-parse fails,
    # so this message is never printed.
    if git("rev-parse", "--show-toplevel").strip() == "":
        refuse("not inside a git repository")
    if git("status", "--porcelain").strip() != "":
        refuse("working tree is not clean")
    # The path is relative to the working directory, so from a
    # subdirectory it matches nothing and this refuses.
    if git("ls-files", PREDICTIONS).strip() != PREDICTIONS:
        refuse(PREDICTIONS + " is not committed; predictions come first")
    with open(PREDICTIONS) as f:
        if "PREDICTIONS ONLY" not in f.read():
            refuse(PREDICTIONS + " no longer reads as predictions only")

    with open(HEADER, "rb") as f:
        original = f.read()
    if md5(original) != HEADER_MD5:
        refuse(HEADER + " md5 " + md5(original) + ", expected " + HEADER_MD5)
    with open(TEST, "rb") as f:
        test_md5 = md5(f.read())
    if test_md5 != TEST_MD5:
        refuse(TEST + " md5 " + test_md5 + ", expected " + TEST_MD5)

    # With the header pinned by md5, a count other than 1 can only come
    # from a pin updated to a header that lacks an anchor.
    text = original.decode("utf-8")
    for tag, _, _, old, _ in MUTATIONS:
        count = text.count(old)
        if count != 1:
            refuse(tag + " anchor occurs " + str(count) + " times, need 1")

    head = git("rev-parse", "--short", "HEAD").strip()
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = "results/mutex_queue_controls_" + stamp + ".txt"

    emit("test_mutex_queue negative controls — raw runner output")
    emit("Script:   tools/run_mutex_queue_controls.py")
    emit("HEAD:     " + head + ", tree clean at start (git status)")
    emit("Started:  " + stamp)
    emit("Host:     " + platform.platform())
    emit("Header:   " + HEADER + " md5 " + HEADER_MD5)
    emit("Suite:    " + TEST + " md5 " + TEST_MD5)
    emit("Predict:  " + PREDICTIONS)
    emit("Timeout:  " + str(TIMEOUT_S) + " s per run via perl alarm; "
         "SIGALRM reported as " + str(SIGALRM_EXIT))
    emit("Build:    " + " ".join(BUILD))
    emit()

    code, log = build()
    if code != 0:
        refuse("baseline build failed:\n" + "\n".join(log[-20:]))
    base_tally = {}
    base_fail = ""
    for _ in range(N_SCHED):
        rc, fl = run_once()
        base_tally[rc] = base_tally.get(rc, 0) + 1
        if fl and not base_fail:
            base_fail = fl
    emit("BASELINE  unmutated suite: build exit 0, runs " + str(N_SCHED))
    for rc in sorted(base_tally):
        emit("    exit " + str(rc) + ": " + str(base_tally[rc])
             + " of " + str(N_SCHED))
    if base_tally.get(0, 0) != N_SCHED:
        refuse("unmutated suite does not pass every run: "
               + (base_fail if base_fail else "(no FAIL line)"))
    emit()

    try:
        for tag, desc, runs, old, new in MUTATIONS:
            with open(HEADER, "w") as f:
                f.write(text.replace(old, new, 1))
            diff = git("diff", "-U0", "--", HEADER)

            emit(tag + "  " + desc)
            for line in diff.splitlines():
                if line.startswith(("+", "-")) and not line.startswith(
                        ("+++", "---")):
                    emit("    " + line)

            code, log = build()
            if code != 0:
                emit("    build exit " + str(code) + " — no run read")
                for line in log[-10:]:
                    emit("    | " + line)
            else:
                tally = {}
                first_fail = ""
                for _ in range(runs):
                    rc, fl = run_once()
                    tally[rc] = tally.get(rc, 0) + 1
                    if fl and not first_fail:
                        first_fail = fl
                emit("    build exit 0, runs " + str(runs))
                for rc in sorted(tally):
                    emit("    exit " + str(rc) + ": " + str(tally[rc])
                         + " of " + str(runs))
                emit("    first FAIL line: "
                     + (first_fail if first_fail else "(none)"))

            with open(HEADER, "wb") as f:
                f.write(original)
            emit()
    finally:
        with open(HEADER, "wb") as f:
            f.write(original)

    with open(HEADER, "rb") as f:
        if md5(f.read()) != HEADER_MD5:
            refuse(HEADER + " not restored")
    code, log = build()
    # -1 means the suite was not run, because the rebuild failed.
    end_code, end_fail = run_once() if code == 0 else (-1, "")
    emit("RESTORED  header md5 verified; rebuild exit " + str(code)
         + ", run exit " + str(end_code))
    status = git("status", "--porcelain").strip()
    emit("Tree:     " + ("clean" if status == "" else "DIRTY: " + status))

    # Written only here, past every refusal: REFUSED lines and the final
    # "wrote" line are printed and never written.
    with open(out_path, "w") as f:
        f.write("\n".join(out_lines) + "\n")
    print("wrote " + out_path)

    if code != 0 or end_code != 0 or status != "":
        sys.exit(1)


if __name__ == "__main__":
    main()
