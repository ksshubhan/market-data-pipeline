# Low-Latency Market Data Pipeline

A wait-free SPSC ring buffer feeding a market data handler, measured
against a tuned `std::mutex` + condition variable queue under controlled
offered load, on macOS/ARM64.

**p99 handoff latency 125 ns against 375 ns** for the tuned baseline at
500k msg/s, both arms passing the same validity gates.

C++20, ~8,500 lines of source excluding blanks and comments and ~12,800
including them, across 34 files in `src/`. Every measurement in this
README is reproducible from this repository with the commands in
[How to reproduce](#how-to-reproduce), and every figure quoted here is
recomputed from a committed artifact rather than transcribed.

---

## The problem

In a trading system the thread reading the socket must never block. If it
stalls, the kernel receive buffer fills and you either drop packets or
fall behind the feed — and a stale book is worse than no book. So you
decouple: one thread does I/O and minimal parsing, hands off, and returns
immediately to the socket.

The handoff is the problem. A mutex means the producer's progress can
depend on the consumer's. Uncontended, `std::mutex` on macOS is a single
userspace atomic and is cheap. Contended, it drops into `__ulock_wait` —
a syscall that parks the thread and hands the CPU to the scheduler.

That asymmetry is the subject. The mutex is fine on the mean, because most
acquisitions are uncontended. It is expensive on the tail, because the
contended ones cost a syscall and a scheduler decision. And the tail is
what matters: the p99.9 message is disproportionately the one where the
market moved, because bursts and volatility arrive together.

---

## Results

![p99.9 latency vs offered load](results/b1_latency_vs_load.png)

*p99.9 end-to-end latency against offered rate, log/log, valid datapoints
only. Median of three passes per baseline configuration; the spsc arm is
configured identically in both files, so its points are the median of six. The three lines converge because all three
sit on the same ~12 µs scheduler floor — see [the scheduler
floor](#the-scheduler-floor-why-p999-is-not-the-headline-here). The
baselines stop where they fail the producer-lag gate: the parking
configuration above 250k/s, the tuned one above 1M/s.*

![latency distribution](results/b1_percentile_distribution.png)

*The same three configurations across p50 to max at a single offered
rate. The gap is widest at p50 and p99 and closes as the scheduler floor
takes over — which is the whole story of this measurement in one
picture.*

All figures from an Apple M2 (4 P-cores + 4 E-cores, fanless), mains
power, Low Power Mode off, `QOS_CLASS_USER_INTERACTIVE` requested and
**read back** on every measurement thread. Full environment dumps in
`env/`, one per measurement session.

### B1 — end-to-end latency under controlled offered load

Latency is measured from *intended* send time to consumer-processed, with
the send schedule computed before the clock starts. Median of three
passes for each baseline configuration. The spsc arm is identically
configured in both runs, so `analyse_harness_b.py` pools them and its
points are the median of six — the script prints the count per cell, and
it reads 6/6 against 3/3. Only datapoints passing both validity gates are
shown.

Three configurations:

| Configuration | What it is |
|---|---|
| `spsc` | The wait-free ring buffer |
| `mutex-tuned` | Condvar queue, 8192-iteration consumer spin before blocking (§4's fair baseline) |
| `mutex-parking` | The same queue with a 1000-iteration spin, so it parks frequently |

**Median and p99 latency, nanoseconds:**

| Offered rate | spsc p50 | tuned p50 | spsc p99 | tuned p99 | p99 ratio |
|---|---|---|---|---|---|
| 100k/s | 83 | 4,880 | 167 | 11,380 | 68× |
| 250k/s | 83 | 334 | 125 | 9,540 | 76× |
| 500k/s | 83 | 166 | 125 | 375 | **3.0×** |
| 1M/s | 83 | 167 | 125 | 3,380 | 27× |

**p99.9 latency, microseconds:**

| Offered rate | spsc | mutex-tuned | ratio |
|---|---|---|---|
| 100k/s | 11.88 | 19.04 | 1.60× |
| 250k/s | 13.00 | 20.54 | 1.58× |
| 500k/s | 12.65 | 18.71 | 1.48× |
| 1M/s | 12.69 | 20.42 | 1.61× |

**The p99.9 ratio is small, and the reason is the most interesting result
in the project.** Both arms sit on a ~12 µs floor imposed by the machine,
not by either queue. See [the scheduler
floor](#the-scheduler-floor-why-p999-is-not-the-headline-here).

### Messages disturbed — the metric p99.9 cannot express

Counting messages delayed past 1 µs, at equal offered rate. Insensitive
to the scheduler floor that dominates both arms' upper percentiles.

| Offered rate | spsc | mutex-tuned | ratio |
|---|---|---|---|
| 100k/s | 12,081 (0.60%) | 1,062,550 (53.1%) | **88×** |

At 100k/s the tuned baseline's consumer parks on roughly half of all
messages — its 8192-iteration spin is ~10.6 µs against a 10 µs
inter-arrival gap, right at the boundary — and each park costs the
producer a wake syscall on the critical path of its own send schedule.
The lock-free arm never parks. Same machine, same rate, same scheduler
floor: 88× fewer messages delayed past a microsecond.

Worth naming what that 53.1% is, because the heading could mislead. It is
not tail behaviour on the baseline arm — a cost paid on every other
message is not a tail. It is the wait policy showing up in the bulk of the
distribution, and it is visible here rather than in the percentiles
because the percentiles are pinned to the scheduler floor described
below.

### The scheduler floor: why p99.9 is not the headline here

Dumping every sample above 1 µs and grouping them into stalls:

| | stalls/s | messages per stall | worst latency in stall |
|---|---|---|---|
| spsc @ 1M/s | 398 | median 4, max 47 | median 4.5 µs, max 46.8 µs |
| mutex-tuned @ 1M/s | 148 | median 4, max 75 | median 4.2 µs, max 44.8 µs |
| spsc @ 100k/s | 346 | median 1, max 171 | median 6.7 µs, max 1.18 ms |

At 1M/s slow messages arrive in contiguous runs: one event stalls the
consumer for several microseconds and every message arriving during it is
delivered late together. At 100k/s the same stalls are mostly singletons,
because ten times fewer messages arrive while the CPU is away.

**Two hypotheses, and the data separates them.** A per-message cost — the
sample buffer crossing a 16 KiB page every 1,024 entries, say — leaves
index spacing unchanged when the offered rate changes. A per-time cost
scales it with the rate. Running the SPSC arm at both rates:

| Offered rate | stalls | messages per stall interval | stalls/s |
|---|---|---|---|
| 100k/s | 6,911 | 289 | 346 |
| 1M/s | 795 | 2,516 | 398 |

A 10× change in offered rate moves index spacing **8.7×** and stall rate
1.15×. Per-message predicts 1.0 and 10; per-time predicts 10 and 1.0. It
is per-time, and the page-crossing hypothesis is dead — that would have
put stalls one per 1,024 records at both rates.

That is the scheduler taking the CPU away, and it is the same population
found in timer calibration *before the queue existed*, in a loop that did
nothing but read the clock twice. Note that the stall rate differs by arm,
398/s against 148/s at the same offered rate, so this is not one external
metronome striking both equally. What is common is the mechanism and the
~12 µs floor it puts under p99.9.

So on this hardware the upper percentiles of both arms are dominated by
the operating system, and the queue difference is only visible below
p99.9. This is the quantified version of the "no thread pinning"
limitation: macOS offers no `taskset` and no `isolcpus`, and the cost of
that shows up as a 12 µs floor on any latency distribution measured here.

**It is a limit of the measurement platform, not a null result.** The
mechanism was measured in calibration, predicted, and then observed in the
pipeline.

Two caveats on the numbers above. The analysis script reports two
per-cluster durations and they are not interchangeable: `drain span` is
the span of *dequeue* timestamps across a cluster, i.e. how long the
backlog took to drain once the consumer was running again, while
`stall length` is the worst latency inside the cluster and is a lower
bound on how long the CPU was away. The figures quoted here are stall
lengths. An earlier version of the script called the first column
`stall duration`, and that name was read into the plan as a stall length
before its own median values — 0 ns and 83 ns — made clear it could not
be one. And the 100k runs last 20 s against the 1M runs' 2 s, so the
100k row has ten times the exposure to rare events; that is most of why
its maximum is 1.18 ms. p99.9 is insensitive to run length and the 12 µs
floor is unaffected.

### B3 — parse cost against handoff cost

Batched timing over 200,000 real captured messages, 20 rounds,
interleaved, median reported.

| | ns per message |
|---|---|
| `parse_book_ticker` over captured JSON | **161.5** |
| Full push + pop through the SPSC ring, single-threaded | 5.83 |
| 56-byte `CaptureRecord` assignment (the pre-parsed path) | 0.93 |

**Parse cost dominates handoff cost by 27.7×** — which is why real venues
ship binary protocols rather than JSON.

Three qualifications, because the number flatters the parser otherwise.

The handoff arm is **single-threaded**, so it pays no cross-core coherence
traffic and is a *lower bound* on a real handoff. A1b's two-thread figure
is ~30 ns/handoff, against which the ratio is ~5.4×. The lower bound is
the honest comparison to lead with: if parse dominates the cheapest
possible handoff, it dominates the real one.

**This is a schema-specific key scanner, not a general JSON parser.** It
assumes a known Binance bookTicker layout and does a single left-to-right
pass with no DOM, no allocation and no floating point. 161 ns is far below
the 1,000–3,000 ns a generic JSON library would cost, so this figure must
not be quoted as "the cost of JSON parsing".

The 0.93 ns copy figure is a **floor, not a like-for-like third arm** — a
56-byte assignment in a tight loop over a resident array, where the
compiler is free to keep everything in registers. It bounds what the
pre-parsed path can cost; it is not what the producer pays per record in
context.

B3 is measured this way rather than as an end-to-end `--parse-in-ingest`
arm because a ~161 ns parse would be invisible inside a distribution whose
upper percentiles sit on a 12 µs scheduler floor. Batching answers a
question about a mean at far higher resolution — which is the same
reasoning that split the harnesses in the first place.

### A-series microbenchmarks

Two-thread queue microbenchmark, batched timing, 10M handoffs per arm.

| # | Result | Strength |
|---|---|---|
| **A1b** | Acquire-release is **1.23×** faster than `seq_cst` in the real queue (33.07 vs 26.88 M handoffs/s) | No distribution overlap. Two runs five days apart agree: 1.25 and 1.23 |
| **A1a** | Ordering cost is **not measurable** in a bare atomic ping-pong: relaxed 36.9, acq-rel 38.5, seq_cst 38.3 ns/handoff | A null result, and the explanation is the point — see below |
| **A2b** | The coherence granule is **64 bytes** — contradicting `hw.cachelinesize` (128) and libc++'s `hardware_destructive_interference_size` (256) | Two runs three days apart agree: 3.566× and 3.596× against a shared line, with the 64/128/256 arms within 0.207% and 0.160% of each other. 64 bytes buys the whole benefit; 128 and 256 add nothing measurable |
| **A3b** | Natural 80-byte ring slots cost **1.81×** against 128-byte padded slots | No overlap; disassembly confirms both loops cover all 80 bytes |
| **A4b** | Caching the opposite index saves **~10 ns/push**, 1.36× | Cached faster in 17 of 20 paired rounds at 5 MB, **20 of 20** at 80 MB, and 19 of 20 on the 7 Sep re-run at 80 MB. Medians 1.400×, 1.359× and 1.396× — the headline quotes the 80 MB figure. Quote the median, not the mean: the 5 MB run's cached arm spans 17.4 to 83.4 M ops/s around a 35.3 median, which is core placement rather than the queue, and its mean ratio reads 1.52× |

**A1a's null result is more informative than a number would have been.**
A bare ping-pong serialises every handoff behind a cross-core coherence
round trip, so the measurement is coherence-bound rather than
instruction-bound and the ordering cost disappears below the noise. A1b
shows a real 1.23× because the queue pipelines across 1024 slots and has
own-index loads that change from `ldr` to `ldar` under `seq_cst`.

**Three of the M2's cache numbers disagree**, and this project can say
which one governs: `hw.cachelinesize` reports 128 (fetch granularity),
the standard library reports 256, and A2b measures **64** (coherence).

**The 256 turned out to be a default rather than a judgement.** Both
libc++ builds report it — LLVM 220108 and Apple 210106 — but they are one
lineage, so that agreement was never independent confirmation. libstdc++
15 on ARM64 Linux also reports 256 in a default build, and then moves:
`neoverse-n1`, `neoverse-v1`, `neoverse-v2`, `cortex-a76` and `cortex-x3`
all give **64**, while `generic` and `apple-m1` give 256. GCC emits the
constant from the tuning model's prefetch table, and the generic table
leaves the cache-line field unset.

So the sharper statement is not that the libraries are conservative by
4×. It is that **a direct measurement and a compiler independently arrive
at 64** — A2b on this machine, and GCC for every aarch64 part it models —
while 256 is what you get for not telling the toolchain what it is
building for. Details and the source trace are in the limitations
section; the constant is always quoted with its library version because
it is a property of one, not of the hardware.

### There is no compare-and-swap in this queue

The producer performs an atomic *load* of the head and an atomic *store*
of the tail. No read-modify-write, no `ldxr`/`stxr` exclusive pair, no
`cas`. Verified from the committed disassembly
(`evidence/spsc_arm64_disassembly.txt`), not asserted.

`std::atomic` is not buying a lock or a CAS here — a naturally-aligned
64-bit access on ARM64 is already indivisible in hardware. What it buys is
C++ semantics: data-race freedom, compiler discipline, and ordering.

The disassembly also corrected an assumption. Acquire loads compile to
**`ldapr`/`ldapur`** — ARMv8.3 RCpc — not `ldar`, and release stores to
`stlur`, an ARMv8.4 unscaled form. `ldar` is RCsc, stronger than acquire
requires; clang uses the cheaper RCpc form where the standard permits.
That is the mechanism behind A1b: sequential consistency cannot use RCpc,
so `seq_cst` forces `ldar`.

---

## Design decisions

**Single producer, single consumer, no MPMC.** Adding a second producer
means two threads want the same slot index, which needs a CAS retry loop,
which drops the guarantee from wait-free to lock-free. Wait-free is the
guarantee that matches the pitch: the entire premise is bounding the worst
case.

**Wait-free, and the full path is what makes it true.** When the queue is
full, `try_push` inspects the cached head, acquire-loads the real head
*exactly once*, and returns `false`. Two loads maximum, no branch back. A
`while (full) { reload; }` would look almost identical in review and would
silently make the producer blocking.

**Monotonic `uint64_t` counters, full advertised capacity.** The
sacrificed slot exists only when *wrapped* indices are stored, where
`head == tail` is ambiguous. With monotonic counters the unsigned
difference disambiguates: empty when `tail − head == 0`, full when
`tail − head == capacity`. Never an ordering comparison — `head < tail` is
wrong across a wrap.

**Records stored by value, fixed size.** Forced by the guarantee, not
chosen for simplicity: no allocation in a wait-free producer means memory
must be preallocated, which means slots must be fixed size. `malloc` can
take a lock, and the moment you allocate in the producer the allocator's
worst case becomes your worst case.

**Reject-newest on full; the caller's policy is drop-newest.** This is a
known semantic deviation and it is stated rather than hidden: bookTicker
is a snapshot stream, so drop-*oldest* is the semantically correct policy.
Drop-oldest is not expressible here — the producer would have to advance
the consumer-owned index, requiring a read-modify-write and a retry loop,
and it would overwrite a slot the consumer may be mid-copy on. Obtaining
drop-oldest safely needs an overwriting-ring design with consumer lap
detection and per-slot sequence validation. That design is described in
the plan and deliberately not built.

It does not contaminate any reported measurement, because **every reported
datapoint has zero drops**.

**Two counters, not one.** `full_rejections` counts `try_push` calls that
returned `false` — a queue event. `dropped_records` counts records the
*caller* abandoned — a policy decision. Harness A retries and loses
nothing; the replay producer abandons and loses one. Conflating them
double-counts, and under a zero-drop gate it would have marked every
harness A datapoint invalid.

**The mutex baseline is tuned, and the tuning is derived.** Signal only on
the empty→non-empty transition; bounded consumer spin before blocking;
identical `try_push`/`try_pop` interface and identical reject-newest
semantics, so the two arms are not doing different things above
saturation.

---

## Methodology

### Coordinated omission

The offered rate is fixed, the send schedule is computed as a pure
function of record index *before the measurement window opens*, and
latency is measured from the **intended** send time.

Pushing as fast as the consumer can drain measures throughput saturation,
not latency: when the system stalls the producer stalls with it, so the
stall never appears in any latency figure. If the producer instead
computed an intended send time at the moment it was about to push,
coordinated omission would return through the back door — the producer
stalls, the schedule slides with it, and the stall vanishes again.

### Validity gates

Every datapoint must pass two gates, both fixed numerically **before any
harness B code existed**:

1. **Zero dropped records.** Dropping is cheaper than delivering, so an
   arm that drops looks faster.
2. **p99 producer lag within one offered-rate period.** One period is the
   only threshold with a physical meaning: a producer a full inter-send
   interval behind has missed its slot, so the rate on the x-axis is not
   the rate delivered.

Maximum producer lag is **reported but is deliberately not a gate**. An
earlier draft gated on max lag at ten periods; producer characterisation
showed that gate failing by a factor of 460 at 20M/s, because OS
scheduling events have an absolute duration whatever the rate, so a gate
expressed in periods is unsatisfiable at high rates by construction. The
concept was wrong, not the constant — a single 50 µs stall in a 200,000
message run produces one large latency sample, which is data about the
tail rather than evidence the rate was not offered.

Invalid datapoints are written to the CSV with the failing gate named,
not deleted. **The mutex arm fails the lag gate at every rate at or above
2.5M/s**, and the parking configuration fails at 500k/s and above. That
is reported as a limit on the measurable range.

### Why three harnesses, not one

The timer steps in ~41.67 ns (24 MHz; `mach_timebase_info` returns 125/3,
confirmed by measurement). ARM's cycle counter is PMU-gated and unreadable
from userspace here, so 24 MHz is the ceiling rather than a default.

The handoff is ~80 ns and a JSON parse is ~1,000–3,000 ns. Hunting a 10 ns
ordering delta inside a 2,000 ns measurement gives three overlapping
distributions and nothing to report. So:

| Harness | Timing method | Produces |
|---|---|---|
| **A** | Batched: one clock read, 10M ops, one clock read | Means only |
| **B** | Per-message timestamps | Full percentile distributions |
| **C** | No timing; sequence oracle | Failure evidence |

Batching is sound for A because quantisation error is bounded at ±1 tick
*for the whole run*, not per operation. What it loses is percentiles,
which is acceptable because "what does release ordering cost" is a
question about a mean.

**Clock resolution determined which harness could answer which question.**

### Timer calibration

Read the clock twice back-to-back, ~1M times. Every result is either 0
(both reads inside one tick) or ~41.67 (a tick boundary fell between
them). The nonzero fraction times the tick period gives the loop duration
— measuring below the clock's own resolution using the statistics of when
it steps.

The outlier threshold is **70 ns, not a round number**: the
single-boundary model forbids two ticks within one iteration, so anything
at or above ~2 ticks is a model violation by definition. A threshold of
200 would have silently absorbed real violations into the normal
population.

![timer calibration histogram](results/timer_calibration.png)

The histogram is bimodal with nothing between, which is the evidence that
the uniform-phase assumption holds. It also found ~746 multi-tick samples
(memory stalls) and **nine** microsecond-scale samples — 1.0 to 30.8 µs,
median 11.1 µs — context switches, observed before the queue existed, and
the same mechanism that produces the scheduler floor in B1. Nine is the
count; the magnitude is the 11.1 µs median, and it is the ~12 µs floor B1
measures.

The full derivation — the vernier estimator and the 19.5–22 ns spread
across eight runs — is in [`NOTES.md`](NOTES.md), the measurement
notebook. It covers calibration, record layout, the
A-series and C1, and stops at the dataset regeneration on 4 September; the
queue tuning and B1 are in this file rather than there.

### Clock domains

Three, never to be collapsed:

| Domain | Source | Properties |
|---|---|---|
| Exchange | Binance `E`/`T` | Unix epoch ms, someone else's wall clock |
| Capture | Python `time.time_ns()` | Unix epoch ns, **NTP-disciplined, can step backwards** |
| Replay | `CLOCK_UPTIME_RAW` | Monotonic ns, boot-relative |

Capture and replay must **never** be subtracted — different origins
entirely. The field names are deliberately dissimilar so the mistake looks
wrong on the page.

Because capture timestamps are unsigned, a backwards NTP step wraps rather
than going negative, so the replay schedule is built from **cumulative
clamped gaps written as an explicit comparison**, not `max(0, b - a)` —
which is a no-op on unsigned arithmetic and would convert a millisecond
backwards step into a ~585-year forward jump.

The capture timestamp is taken *after* the Python websocket library hands
the message up, so it carries interpreter and asyncio jitter. It is fine
for replay pacing and burst shape. It is **not** an arrival timestamp.

**That jitter is measured, not estimated, and it is ~1 ms.** Binance's
event time `E` is quantised to a millisecond and comes off the exchange's
clock rather than this machine's, so a pair genuinely emitted *d* apart
shares an exchange millisecond with probability 1 − *d*/1 ms under uniform
phase — a prediction with a number on it. Cross-checking the ETHW capture
bucket by bucket:

| capture gap | pairs | share an `E` | predicted if genuine | genuine fraction | Δ`E` p90 |
|---|---|---|---|---|---|
| 1–10 µs | 8,500 | 40.9% | 99.3% | **0.41** | 18 ms |
| 10–100 µs | 3,366 | 42.4% | 98.0% | **0.43** | 21 ms |
| 100 µs – 1 ms | 3,051 | 12.6% | 54.5% | **0.23** | 29 ms |
| > 10 ms | 32,958 | 0.02% | ~0% | — | 1,656 ms |

The Δ`E` column needs no threshold to read: pairs the capture clock places
1–10 µs apart are up to 18 ms apart at the exchange. Below a millisecond
the capture clock is wrong by three to four orders of magnitude on a large
fraction of pairs; at and above a millisecond it tracks, reproducing the
capture-gap distribution to within 0.4% from p90 upward — 1,234 ms against
1,238 ms, 3,723 against 3,723, 11,046 against 11,048. The 0.64 ms residual
at p50 is smaller than `E`'s own millisecond quantum: Δ`E` is a difference
of two integer-ms stamps, so a true 28.361 ms median can only render as 28
or 29 depending on phase. That is a resolution floor, not a discrepancy.
So coarse structure is trustworthy and sub-millisecond structure is not. **No
latency measurement in this project depends on the capture clock.**

### Page warming and dataset handling

The dataset is ~734 MiB. On first traversal it is not in the page cache,
so the producer would take demand-paging faults *inside the measured
window*. The mapping is warmed before the clock starts, with an observable
side effect — a touch loop whose result is discarded is dead code at `-O2`
and clang deletes it.

Warming is then **verified** by comparing two full traversals. Note that
§6.4a of the plan records a lap-1/lap-2 floor of ~1.2 for the full
13.7M-record file, where the residual is cache warming rather than paging.
For the 2M-record slice used in B1 (~112 MB, far beyond the 16 MB L2) both
laps stream from DRAM and the ratio sits near 1.0. A ratio at 1.0 there is
correct, not a failed check.

**Do not compare across symbols.** The BTC dataset streams from DRAM; the
ETHW dataset (55,775 records, ~3.1 MB) fits in L2. Comparing them at equal
offered rate would partly compare an L2-resident read path against a
DRAM-streaming one, which has nothing to do with the queue. Experimental
arms are compared *within* a symbol.

### Ring capacity

16,384 slots (1.31 MB), chosen against a criterion fixed before the first
run: **the smallest power of two** such that dropped records are zero at
every rate below the knee and reconstructed depth stays under half of
capacity.

Smallest, not largest, and the reason is not memory. A larger ring means
the producer stores into slots that have fallen out of cache and the
consumer reads cold ones. That cost is additive and near-identical in
absolute nanoseconds for both arms — so it is a large fraction of a
dozen-instruction push and a small fraction of one that takes a lock.
Oversizing quietly narrows the gap that is the result. A4b saw exactly
this: the cached-index advantage fell from 1.400 at 5 MB to 1.359 at
80 MB.

The pilot recorded **zero dropped records at every rate on every arm**, so
the criterion is met.

### The spin count, and the derivation that was wrong

The baseline's consumer spins before blocking. Choosing that constant went
wrong in an instructive way, and both attempts are kept.

**First attempt — ski-rental.** Spinning is worth doing only while it
costs less than blocking. Spin for exactly the cost of a park and wake and
the worst case is twice optimal. `measure_condvar_wakeup` measured
park/wake at **~1296 ns** and a contended spin iteration at **~1.29 ns**,
giving 1004, rounded to 1000.

**Why it was wrong.** That model costs the *waiter* correctly and ignores
what blocking costs the *signaller*. When a consumer is parked, the
producer's `notify_one` becomes a `__ulock_wake` syscall on the critical
path of its own send schedule. In a queue whose producer must never be
delayed, that term dominates the one the model optimised.

**What measurement showed.** Sweeping the constant directly, at 1M
records/s:

| spin | parks (of 2M) | producer p99 lag |
|---|---|---|
| 1000 | 646,253 | 3,208 ns |
| 8192 | 437 | 41 ns |
| 65536 | 8 | 41 ns |

A 78× reduction in producer lag tracking a 1479× reduction in parks, and
65536 changes nothing further — so 8192 is on the flat part of the curve.
8192 iterations is ~10.6 µs, which exceeds the inter-arrival gap at every
rate at or above ~95k/s. That bound is the justification, rather than a
fitted crossover.

**What it does not fix.** Above ~2.5M/s the spin budget stops mattering
entirely: parks fall from 69,674 to 3 and producer lag does not move
(5,891 vs 6,358 ns). There the cost is the lock itself, and the baseline's
ceiling is real.

`measure_condvar_wakeup` is kept in the repository with its model's
limitation documented in place. Its measurements are correct; the
inference from them to a spin count was not.

### Thermal and scheduling mitigations

A fanless M2 throttles under sustained load, so arms are **interleaved and
their order alternated between passes** rather than run in sequence —
thermal drift then spreads across conditions instead of loading onto
whichever ran last.

There is no thread pinning on macOS. `QOS_CLASS_USER_INTERACTIVE` biases
toward P-cores; it is a hint, not a guarantee. The class is **read back
after being set** and the run aborts if it did not apply — a request that
silently did nothing is worse than not making it. Applying it reduced
within-arm spread on A1 from 21.1% to 5.0%.

---

## Correctness evidence

**The clean result means something because the control fires.** Running
the deliberately-broken C1 arm in the same TSan build produces a data race
naming `spsc_ring_buffer.hpp:130` (the payload read in `try_pop`) against
`spsc_ring_buffer.hpp:100` (the payload store in `try_push`) — the two
non-atomic accesses that acquire/release exists to order. Not the index:
the payload. The report is committed at `evidence/c1_tsan_report.txt`.
A "TSan clean" claim with no verified negative control says only that the
tool was quiet.

**ThreadSanitizer clean on the valid arms**, on three toolchains across
two independent implementations. Homebrew clang and AppleClang are one
lineage, LLVM/libc++. GCC 15.2.0 with libstdc++ 15, on ARM64 Linux, is a
separately written detector on a different operating system and a
different standard library. All seven suites pass under both, and both
runs are committed rather than asserted: `evidence/tsan_llvm_ctest_20260907.txt`
and `evidence/tsan_gcc15_aarch64.txt`. Each carries its own negative
control, run in the same build as the suites it certifies.

**The independent run is worth more than a third green tick, because the
control agrees too.** GCC's ThreadSanitizer flags C1 at
`spsc_ring_buffer.hpp:130` in `try_pop` against `:100` in `try_push` —
the same two non-atomic payload accesses LLVM named, not the index. Two
detectors with no shared code identify the same race at the same two
lines, and independently find nothing in the valid arms. The workload was
verified identical rather than assumed: `kCount` is a hardcoded
`1'000'000`, so the faster Linux wall times are speed, not a smaller
test. Committed at `evidence/tsan_gcc15_aarch64.txt`.

**The C1 broken-ordering arm is expected to be flagged**, and that report
is the evidence rather than a failure of the criterion. C1 removes the
synchronises-with edge, producing an intentionally invalid C++ program: a
data race on the non-atomic payload. Its throughput is not reported,
because a number produced by a program with undefined behaviour is not a
measurement of anything — and the error runs in the unfavourable
direction, since UB licenses transformations a correct program would not
permit.

**The tearing signature was predicted before the run.** From the ring
geometry, a consumer reading a slot before the payload writes are visible
should see a sequence of exactly `N − capacity` beside fresh price fields.
The prediction was committed first; the observed corruption matched.

**Sequence oracle.** A dropping queue makes gaps legal, so "I saw a gap"
proves nothing. Three checks: strict monotonicity (no legal drop policy
can produce a repeat or a decrease), gap reconciliation against
`dropped_records`, and tearing detection.

The reconciliation has **three terms**, and the boundary terms are not
optional:

```
leading  = first_delivered_sequence - first_sequence
trailing = (first_sequence + slice_length - 1) - last_delivered_sequence
interior = sum over consecutive delivered pairs of (seq[i] - seq[i-1] - 1)

leading + interior + trailing == dropped_records
```

Summing only the interior gaps misses records abandoned before the first
delivery or after the last — and it does not report an error, it reports
agreement while missing drops. Removing the trailing term makes the test
fail by exactly one record. Under drop-newest the last records of a
saturated run are disproportionately likely to be the abandoned ones.

Gap width is also not always 1: each abandoned record contributes exactly
one to the total, but *k* consecutive abandonments merge into a single
observed gap of width *k*. The invariant is the sum, never the individual
widths.

**2×10⁹-message stress run**, dense sequence, zero drops.

**Parser validated exhaustively and independently.** All 13,749,492
records round-tripped against their originating log lines by a separate
Python implementation using `decimal.Decimal` — not the C++ parser, so a
shared digit-counting bug cannot cancel itself out. A deliberate one-byte
corruption of the binary was detected and named. A deliberately malformed
input caused the converter to fail loudly and leave neither `.bin` nor
`.bin.<pid>.tmp` behind.

**The published `.bin` is durable, and publishing it is exclusive.** The
temporary file is flushed with `F_FULLFSYNC` before it is published,
not `fsync`: on Darwin `fsync` hands the data to the drive and returns,
leaving it free to sit in the drive's own volatile write cache, and
`F_FULLFSYNC` is the call that asks for that cache to be flushed.
Publishing is `link` then `unlink` rather than `rename`, because
`rename` replaces whatever is at the destination and the
refuse-to-overwrite check runs at startup, hundreds of milliseconds
earlier. Forty converters launched at once against one output path: one
publishes, the rest are refused, five of them by the publish itself. The
same forty against a build that renamed instead published twelve times
and kept the last.

**Properties of the feed, recorded rather than assumed.** Over all
13,749,492 records: `T <= E` holds universally — 78% have `T == E`, 22%
have `T < E` with a maximum lag of 30 ms, and **no record has `T > E`**.
Both fields are `uint64` on the wire, so `E - T` underflows rather than
going negative; the check compares operands rather than testing the sign
of a difference, which is the same unsigned trap the replay clamp
documents. Minimum quantity is 0.001 on both sides with no zeros, which
independently corroborates BTCUSDT's `stepSize` from the `exchangeInfo`
snapshot. **No crossed or locked books in the entire capture** — bid is
strictly below ask on every message, which also rules out the
case-sensitivity trap where a `tolower` in the key path silently swaps
price and quantity.

**Byte-identical output across two conversions** six days apart on
different commits: 13,749,492 records identical, with only the provenance
fields differing.

**Checks that abort rather than warn.** Harness B enforces, at run time
and at `-O2`: `full_rejections == dropped_records` (the two counters must
agree under drop-newest with no retry), `book_updates == delivered` (the
consumer did the work the methodology claims), and QoS applied on both
threads. `assert` is not used for these — both build presets define
`NDEBUG`, which once left the entire parser test suite compiling to
nothing and exiting 0 for three days.

---

## How to reproduce

Requires Homebrew LLVM (not Apple Clang), CMake, and a Binance capture.

```sh
cmake --preset default
cmake --build --preset default
ctest --test-dir build/default --output-on-failure

# Inspect the raw capture before trusting it: precision and quantity
# maxima, message counts, leading zeros, E monotonicity, and
# capture-clock monotonicity, in one pass
python3 tools/inspect_capture.py <capture.log>

# Convert a capture to the binary dataset, then validate it exhaustively
./build/default/convert_capture <capture.log> <out.bin> BTCUSDT \
    $(git rev-parse HEAD) 0
python3 tools/validate_capture.py <capture.log> <out.bin>

# Record the environment before every measurement session
bash env/dump_environment.sh

# A-series microbenchmarks. <experiment> is one of:
#   a1   a2   a2b   a3b   a4   a4b
# a1 runs both A1a (atomic-only, three orderings) and A1b (queue arms).
# harness_a writes to stdout and creates no file, so redirect it. The
# committed results/a*.txt are named from the utc_timestamp the harness
# stamps into its own output.
./build/default/harness_a $(git rev-parse HEAD) 0 a1 > /tmp/a1.txt

# B1 load sweep, both baseline configurations
./build/default/harness_b $(git rev-parse HEAD) 0 <out.bin> BTCUSDT book 3 8192
./build/default/harness_b $(git rev-parse HEAD) 0 <out.bin> BTCUSDT book 3 1000

# The spin sweep behind the choice of 8192. Three candidates an order of
# magnitude apart, not a scan. It takes no seventh argument: the sweep
# sets the spin itself.
./build/default/harness_b $(git rev-parse HEAD) 0 <out.bin> BTCUSDT book spin-sweep

# Post-processing and graphs. The tables print with the standard library
# alone; the graphs need matplotlib, which on a Homebrew Python needs a
# virtual environment (PEP 668). Python dependencies for tools/ are in
# requirements.txt.
python3 -m venv .venv && .venv/bin/pip install matplotlib
.venv/bin/python tools/analyse_harness_b.py results/harness_b_spin8192_*.csv \
                                            results/harness_b_spin1000_*.csv

# Tail structure. The dump is ~48 MB per run and results/tail_samples_*.csv
# is gitignored, so the second command needs the first to have been run in
# this clone. Its output, results/tail_stalls_*.txt, is committed.
./build/default/harness_b $(git rev-parse HEAD) 1 <out.bin> BTCUSDT book dump
python3 tools/analyse_tail_samples.py results/tail_samples_*.csv
```

Measurement runs require mains power and Low Power Mode off. `harness_b`
records UTC timestamp, capacity, spin count and QoS class into its CSV
header, so those fields are self-describing. `harness_a` records three of
the four — it has no spin count to record. The smaller measurement
binaries are less complete than either: `measure_parse_cost` and
`measure_condvar_wakeup` carry the run timestamp in the filename and not
in the file, and `results/a1_memory_order_20260830_232947.txt` predates
the header entirely, carrying a shuffle seed and nothing else. The 4
September A1 re-run supersedes that one.

**The commit and dirty flag are not.** Both harnesses take them from
`argv` and neither consults git; only `convert_capture` verifies tree
state itself, via `--require-clean`. So a results file's `git_dirty: no`
is the operator's assertion, and the corroboration is
`env/dump_environment.sh`, which computes the same field from `git status`
and is re-run before each session. Three artifacts from 4 September record
`git_dirty: no` beside environment dumps taken in the same second at the
same commit recording `yes`; they are kept, and the A2b and A4b figures
above come from re-runs on a tree verified clean before the run. Compare
the pair, not the flag.

The two dirty flags differ deliberately: measurement runs pass `0`, while
the tail dump passes `1` because it is a diagnostic rather than a reported
result and is expected to run against modified source.

---

## Limitations

**The upper percentiles measure the operating system, not the queue.**
Both arms sit on a ~12 µs floor from scheduler stalls arriving one to
four hundred times per second, at a rate that differs by arm. Below p99.9 the queue difference is clean;
at p99.9 it compresses to ~1.6×. Bare-metal Linux with thread pinning
would resolve this; macOS offers no equivalent.

**No thread pinning, and heterogeneous cores.** `QOS_CLASS_USER_INTERACTIVE`
biases toward P-cores but the scheduler can still move a thread mid-run.
Expect higher run-to-run variance than a pinned Linux box would show.

**No PMU access.** `perf c2c` for false-sharing counter evidence is Linux
+ PMU only, so A2b infers the coherence granule from timing rather than
from counters. The disassembly of both A2b arms is committed at
`evidence/a2b_arm64_disassembly.txt`, confirming the loops differ only in
slot stride.

**Measurement provenance is corroborated, not enforced.** The harnesses
trust the caller's commit and dirty flag; the environment dump computes
both from git. Two sources that agree is the check, and it is weaker than
`convert_capture`'s, which verifies tree state before it will write. The
harnesses were left alone deliberately — the gap is documented and the
affected artifacts re-run, which costs minutes, where making three
binaries self-verifying costs a day and re-verification on two toolchains.

**Coarse timer.** ~41.67 ns per tick. On x86, `rdtsc` is over 100× finer.
This methodology exists *because* the ARM timer is coarse.

**SPSC only.** MPMC would need a CAS on slot claim and would drop the
guarantee to lock-free. Not built, deliberately.

**Drop-newest, where the stream wants drop-oldest.** Described above; no
reported measurement drops, so nothing in the results depends on it.

**Variable-length records not supported.** A record cannot straddle the
wrap point, which would need padding records and would cost the clean
power-of-two bitmask. Aeron does this properly; out of scope here.

**Binary format is deliberately non-portable.** Native little-endian,
enforced by a host-endianness `static_assert`. An `endianness` byte would
be decoration: a big-endian host would read `record_count` — byte-swapped
— *before* reaching a field telling it the file is little-endian.

**Format v1 assumes SHA-1 git object ids.** The header stores a raw
20-byte commit id. A SHA-256 repository requires `format_version` 2.

**The "messages disturbed" comparison uses a fixed 1 µs threshold**, so it
is only meaningful where one arm crosses it and the other does not. At
1M/s the tuned baseline's per-message cost falls below the threshold and
the comparison inverts — that is an artifact of the fixed threshold, not
a reversal.

**ThreadSanitizer now has two independent implementations, not three
toolchains of one.** Homebrew clang and AppleClang are a single
LLVM/libc++ lineage; GCC 15 on ARM64 Linux is separately written. Both
report clean on the valid arms and both flag C1 at the same two lines.
The remaining limitation is narrower: all three run on ARM64, so nothing
here is a cross-architecture check.

**The interference constants are closed, and the 256 is a default rather
than a judgement.** libstdc++ 15.2.0 on aarch64 reports 256 destructive
and 64 constructive in a default build — matching both libc++ builds. But
the number moves with `-mcpu`: `generic` and `apple-m1` give 256, while
`neoverse-n1`, `neoverse-v1`, `neoverse-v2`, `cortex-a76` and `cortex-x3`
all give **64**, confirmed through `<new>` and not only the macro.

GCC 15's source explains it. `aarch64-cores.def` recognises `apple-m1` —
real MIDR values, `V8_5A` — but wires all Apple parts to the
`generic_armv8_a` tuning model, and the constant is emitted from the
tuning model's prefetch table gated on `l1_cache_line_size >= 0`.
`generic_prefetch_tune` leaves that field at `-1`;
`generic_armv9a_prefetch_tune`, used by the Neoverse N1 tuning, sets 64.
**So 256 is what GCC emits when the tuning model declines to state a line
size**, and `-mcpu=apple-m1` looks like asking GCC about this chip while
actually being routed to the table with the field unset.

The result is therefore not that the libraries are wrong by 4x. It is that
**one direct measurement and one compiler independently say 64** — A2b on
this M2, and GCC for every aarch64 part it models — while 256 is the price
of not telling the toolchain what it is building for. libc++ has no tuning
notion at all and reports 256 flat, so it cannot participate in the
distinction. Measured in an Ubuntu 25.10 aarch64 guest under UTM;
`evidence/interference_libstdcxx_gcc15_aarch64.txt`, probe at
`tools/interference_probe.cpp`.

**B2 is closed as an analysis result rather than a measurement.** The
compression factor is 38,791, giving 100k msg/s mean offered load from the
ETHW capture, chosen on peak windowed arrival rate rather than on mean
rate — a mean does not fill a ring, a burst does. The burst-replay arm was
specified and deliberately not run: simulating the compressed schedule
against a constant drain gives a peak backlog of **137 slots out of
16,384**, and the trace cannot fill the ring at any compression factor
that also keeps the tuned baseline inside its validity range, because
filling it would need peak sustained arrival to exceed the consumer's
drain rate. The analysis reproduces the schedule builder's arithmetic
bit-for-bit, so this is a computed result with committed provenance rather
than an untested assumption. Reproduce it with
`tools/inspect_interarrival.py`.

**Compressed schedules contain ties.** The schedule builder truncates each
compressed gap to integer nanoseconds before accumulating, so any captured
gap shorter than the compression factor becomes exactly zero and the
affected records share an intended-send offset — 20.9% of gaps at the
chosen factor, essentially all of them below the capture clock's own
resolution. The producer then issues those records back to back at its own
~20 ns ceiling and the shortfall appears as producer lag rather than as
schedule spacing. Gaps in the 100 µs – 1 ms band are ~77% capture artifact
and do survive compression as 2.6–25.8 ns of spacing, most but not all of
which is below that ceiling. Stated as contamination rather than waved
off.

B3 is measured, by direct comparison rather than as an end-to-end arm —
see above for why.