#!/bin/bash

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TIMESTAMP="$(date -u '+%Y%m%d_%H%M%S')"
OUTPUT="${ROOT_DIR}/env/measurement_environment_${TIMESTAMP}.txt"
PROBE="${ROOT_DIR}/build/default/environment_probe"

sysctl_value()
{
    local key="$1"

    if value="$(sysctl -n "$key" 2>/dev/null)"; then
        printf '%s: %s\n' "$key" "$value"
    else
        printf '%s: unavailable\n' "$key"
    fi
}

if [[ ! -x "$PROBE" ]]; then
    echo "environment_probe not found at:"
    echo "$PROBE"
    echo "Build it before running this script."
    exit 1
fi

GIT_COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD)"

# The same rule src/provenance.hpp applies for the timed programs, so the
# two dirty flags recorded for a session can be compared. Any change to a
# tracked file counts, and so does any untracked file outside results/ and
# env/. Those two directories hold what the dumps and the timed programs
# write and feed no build; without the exception a second dump, or one
# taken after a harness run, would report a dirty tree caused by the
# session's own output.
GIT_STATUS="$(git -C "$ROOT_DIR" status --porcelain --untracked-files=all \
    | grep -v -E '^\?\? (results|env)/' || true)"

if [[ -n "$GIT_STATUS" ]]; then
    GIT_DIRTY="yes"
else
    GIT_DIRTY="no"
fi

{
    echo "=== measurement environment ==="
    echo "utc_timestamp: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo

    echo "=== repository ==="
    echo "git_commit: $GIT_COMMIT"
    echo "git_dirty: $GIT_DIRTY"

    echo
    echo "=== hardware ==="
    sysctl_value hw.cachelinesize
    sysctl_value hw.perflevel0.logicalcpu
    sysctl_value hw.perflevel1.logicalcpu
    sysctl_value hw.l1dcachesize
    sysctl_value hw.l2cachesize

    # Per-core-type cache geometry. hw.l1dcachesize reports a single
    # figure on a heterogeneous SoC, which is not enough to reason about
    # A2 and A3: those experiments are about cache-line separation and
    # slot working-set size, and P-core and E-core figures differ.
    sysctl_value hw.perflevel0.l1dcachesize
    sysctl_value hw.perflevel0.l2cachesize
    sysctl_value hw.perflevel1.l1dcachesize
    sysctl_value hw.perflevel1.l2cachesize
    
    sysctl_value hw.memsize
    sysctl_value machdep.cpu.brand_string

    echo
    echo "=== operating system ==="
    sw_vers

    echo
    echo "=== compiler ==="
    "$(brew --prefix llvm)/bin/clang++" --version

    echo
    echo "=== sdk ==="
    echo "sdk_path: $(xcrun --show-sdk-path)"
    echo "sdk_version: $(xcrun --show-sdk-version)"

    echo
    echo "=== power ==="
    pmset -g batt

    LOW_POWER_MODE="$(pmset -g 2>/dev/null | awk '/lowpowermode/ {print $2; exit}')"

    if [[ -n "$LOW_POWER_MODE" ]]; then
        echo "low_power_mode: $LOW_POWER_MODE"
    else
        echo "low_power_mode: unavailable"
    fi

    echo
    echo "=== c++ environment ==="
    "$PROBE"

} > "$OUTPUT"

echo "$OUTPUT"
