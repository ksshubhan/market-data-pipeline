# Timer Calibration and Vernier Measurement

## Result

The measurement window between the two timer reads was estimated at approximately
**19.91 ns** on the run plotted here, with a range of roughly **19.5–22 ns** across eight
runs. This spread could be due to unpinned threads on heterogeneous P/E cores on a
fanless machine, and is itself a result: no experiment on this hardware can claim a
difference smaller than the run-to-run variance from a single run.

This window is smaller than the hardware timer's resolution of 41.667 ns, so it cannot
be measured directly from a single pair of readings so we use a vernier-style statistical
measurement instead.

---

## Timer Resolution

The resolution was obtained using `mach_timebase_info` which
returned `125 / 3`, meaning the timer increments every 41.667 ns — a frequency of
24 MHz.

This frequency describes the **hardware timer used for measurement**, not the frequency
at which the M2 CPU executes instructions.

---

## Timer Boundaries

Because the timer only increments once every 41.667 ns, its value remains constant
between two consecutive ticks. The exact instant at which the timer increments is
referred to here as a **timer boundary**.

                 timer boundary
                       |
                       v
-----------------------|-----------------------
   value = N              value = N + 1

Let two timer reads be performed, `t1` and `t2`. Let the actual time elapsed between
them be the **measurement window**.

If `t1` and `t2` occur within the same pair of timer boundaries, the timer has not
incremented between them:

boundary                         boundary
   |                                |
   |       t1 -------- t2           |
   |                                |

Both reads return the same timer value, so the measured difference is:
delta = 0

If instead `t1` and `t2` fall on either side of a timer boundary, the timer will have
incremented once between them:

                    boundary
                      |
             t1 ------|------ t2

Therefore the measured difference is one timer tick:
delta = 41.667 ns

So despite the measurement window being approximately 19.91 ns, an individual reading
will only ever report 0 ns or 41.667 ns.

---

## Vernier-Style Estimation

The timer cannot directly show 19.91 ns because it only updates every 41.667 ns.
But a smaller value can still be estimated by taking many measurements.

Each measurement is assumed to start at a roughly random point within the timer's
41.667 ns tick period. Because of this, some measurement windows cross a timer boundary
and some do not. We can use how often a boundary is crossed to find the size of the window:

measurement window  ≈  P(boundary crossing) × timer period

P(boundary crossing) ≈ K / N

`N` - the total number of measurements
`K` - the number that crossed a boundary.

For example, if approximately 48% of samples cross a boundary:

0.48 × 41.667 ≈ 20 ns

This estimates a time smaller than one timer tick even though the timer cannot show
that value directly. This type of measurement is called a **vernier-style measurement** because many less
precise measurements are used to estimate a more precise result.

From the measured data, the estimated window was approximately **19.91 ns**.

---

## Histogram as Evidence

The histogram supports the result. Nearly all measurements fall at either 0 ns or
41.667 ns, which is exactly what a timer updating every 41.667 ns should produce:

0 ns        → no timer boundary was crossed
41.667 ns   → one timer boundary was crossed

Values such as 10 ns or 20 ns should not appear, because the timer cannot represent
them.

![Timer calibration histogram](results/timer_calibration.png)

---

## Excluding Interrupted Measurements

The vernier method assumes the gap between the two reads is small enough to cross **no
more than one timer boundary**.

one tick  = 41.667 ns
two ticks = 83.333 ns

Since the estimated window is only about 19.91 ns, an uninterrupted measurement should
produce either 0 ns or 41.667 ns and nothing larger.

A cutoff of **70 ns** was therefore used, sitting between one and two ticks:

41.667 < 70 < 83.333

Any measurement above 70 ns is caused by something interrupting or delaying the
program, rather than by normal timer behaviour. These are excluded so they do not
affect the estimate.

---

## Outlier Populations

The excluded measurements fall into two distinct groups:

| Population         | Count |                 Cause                   |
|--------------------|-------|-----------------------------------------|
| Multi-tick         | 746   |             Memory stalls               |
| Microsecond-scale  | 9     | Context switches / scheduling delays    |

These are not part of normal timer behaviour. 

The 9 context-switch measurements matter later: they are evidence of the scheduling
delay that is expected to produce the long-latency tail in the mutex baseline (B1),
observed here on a thread doing nothing else.

The 746 memory-stall measurements come from a different source of delay and should not
be conflated with that mutex-tail mechanism.

---

## Key Takeaway

The hardware timer only updates every 41.667 ns, but the actual gap between the two
timer reads is about 19.91 ns. Because that is smaller than one timer tick, the timer
cannot show it directly.

Instead it is estimated from how often measurements cross a timer boundary:

measurement window ≈ P(boundary crossing) × timer period

This establishes the timing resolution and measurement limitations that constrain the
later SPSC ring-buffer benchmarks.