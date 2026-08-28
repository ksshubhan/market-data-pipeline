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


## Record Layout

`Record` is an 80-byte runtime object:

- `sequence`: 8 bytes
- `replay_intended_send_ns`: 8 bytes
- embedded `CaptureRecord`: 56 bytes
- `symbol_id`: 2 bytes
- `reserved[6]`: 6 bytes

Therefore:

8 + 8 + 56 + 2 + 6 = 80 bytes

The six tail bytes are explicit rather than compiler-inserted padding, so they can be deliberately zeroed by the replay producer.

`sizeof(Record) == 80`
`alignof(Record) == 8`
`offsetof(Record, capture) == 16`

Any later A3 padding experiment applies to the enclosing ring-buffer slot, not to `Record` itself.

Capture receive timestamps use time.time_ns(), so they are Unix epoch wall-clock timestamps in nanoseconds. Binance E and T are Unix epoch wall-clock timestamps in milliseconds. After converting the Binance timestamp to nanoseconds, the values can be compared, but the difference is not a pure network-latency measurement: it also includes wall-clock synchronization error between Binance and the capture machine.

Long capture: 2026-08-20 13:08:13 UTC to 19:08:51 UTC (6:00:38). Captured 13,749,492 BTCUSDT futures bookTicker messages, 55,775 ETHWUSDT futures bookTicker messages, 85,853 BTCUSDT futures depth messages, and 5,376,025 BTCUSDT spot bookTicker messages. No stream reconnects occurred. Full BTCUSDT depth continuity validation found 0 sequence gaps across 85,853 events (current pu == previous u for every consecutive event).

### CaptureRecord layout

`CaptureRecord` contains the seven capture-derived 8-byte fields:

* `capture_wall_time_ns`
* `event_time_ms`
* `transaction_time_ms`
* `bid_price`
* `ask_price`
* `bid_qty`
* `ask_qty`

Therefore:

`sizeof(CaptureRecord) == 56`

`Record` embeds `CaptureRecord` directly as its `capture` member. This makes the relationship structural rather than maintaining a duplicated seven-field layout. A compile-time assertion verifies that `capture` begins at offset 16.

`Record` remains 80 bytes. Any later A3 padding experiment applies to the enclosing ring-buffer slot, not to `Record` itself.



### Fixed-point scale

Prices and quantities are stored as signed 64-bit integers scaled by `10^8`.

The futures captures required at most 6 fractional digits:

* BTCUSDT: maximum 3 fractional digits
* ETHWUSDT: maximum 6 fractional digits

The saved Binance futures `exchangeInfo` snapshot also reports a maximum rendered filter precision of 6 fractional digits for the replayed symbols:

* BTCUSDT: `tickSize = 0.10`, `stepSize = 0.001`
* ETHWUSDT: `tickSize = 0.000100`, `stepSize = 1`

A scale of `10^8` therefore provides two additional decimal places of headroom for the current replay symbols. The parser must still reject any value containing more than 8 fractional digits rather than silently truncating it.

### Futures capture validation

Both completed futures bookTicker captures were checked before parser implementation.

* BTCUSDT: 13,749,492 raw lines and 13,749,492 bookTicker messages
* ETHWUSDT: 55,775 raw lines and 55,775 bookTicker messages
* No non-payload messages were present
* No unusual leading-zero numeric representations were observed
* Binance event timestamp `E` never decreased in either capture

The ETHWUSDT trace also contains meaningful burst structure and is suitable for the B2 captured-burst experiment. Its inter-arrival statistics were:

* median gap: 28.36 ms
* mean gap: 387.91 ms
* coefficient of variation: 2.00
* 26.75% of gaps ≤ 1 ms
* 40.91% of gaps ≤ 10 ms
* 13.81% of gaps ≥ 1 s

B2 should preserve these relative inter-arrival gaps while applying uniform time compression to raise the offered rate. Experimental arms should be compared within the same symbol because BTCUSDT and ETHWUSDT have very different replay working-set sizes.

The parser converts one futures bookTicker capture message into validated, fixed-point capture data. It populates E, T, capture timestamp, bid/ask prices and quantities. It does not create replay sequence numbers or intended-send timestamps and does not perform replay/ring-buffer work.


### Timestamp fields and ownership

* `event_time_ms` and `transaction_time_ms` are Binance Unix epoch wall-clock timestamps in milliseconds.
* `capture_wall_time_ns` is the local capture machine's Unix epoch wall-clock timestamp from Python `time.time_ns()`, in nanoseconds.
* `replay_intended_send_ns` belongs to the replay run's monotonic clock and is used for replay scheduling and latency measurement.

`capture_wall_time_ns` may be compared with Binance `E` or `T` after converting units, but the difference includes wall-clock synchronization error between Binance and the capture machine and must not be interpreted as pure network latency.

`replay_intended_send_ns` belongs to a different clock domain and must not be directly compared with Binance wall-clock timestamps or `capture_wall_time_ns`.

`sequence` and `replay_intended_send_ns` are assigned by the replay producer. The parser populates only capture-derived fields.


Binary format version covers the entire file format. Any incompatible change to either the 64-byte header or CaptureRecord layout requires a version bump. record_size is still stored as an independent sanity check.