# Draws results/timer_calibration.csv, the clock-read differences that
# src/calibrate.cpp writes one per line, as a histogram on a log scale and
# saves it as results/timer_calibration.png. Run it from the repository
# root with the pinned requirements: .venv/bin/python
# scripts/plot_calibration.py. Only differences up to display_max_ns are
# drawn, in 1 ns bins; the caption counts the rest. The title's tick
# length and sample count, and the caption's threshold, are fixed text,
# not read from the data or from calibrate.cpp.

import matplotlib.pyplot as plt


with open("results/timer_calibration.csv", "r") as f:
    raw = f.read()


# Just past three ticks: every drawn difference is 0, 41 or 42, 83 or 84,
# or 125 ns, and anything longer is left to the caption.
display_max_ns = 130
# calibrate.cpp's max_single_boundary_ns, copied by hand for the caption;
# nothing ties the two.
analysis_threshold_ns = 70


deltas = [int(line) for line in raw.split()]

# Caption statistics
beyond_display = len([x for x in deltas if x > display_max_ns])
# Above 1000 ns, where calibrate.cpp's microsecond bucket starts at 1000:
# a difference of exactly 1000 ns, 24 ticks, is counted there and not
# here.
context_switches = len([x for x in deltas if x > 1000])
max_delta = max(deltas)


plt.hist(deltas, bins=range(0, display_max_ns + 1))
plt.yscale("log")


plt.xlabel("Delta between consecutive clock reads (ns)")
plt.ylabel("Count (log scale)")
plt.title("Timer resolution: 41.667 ns tick, n=1,000,000")

# Leave enough space for the two-line caption
plt.subplots_adjust(bottom=0.20)

plt.figtext(
    0.5,
    0.055,
    f"{beyond_display} of {len(deltas)} samples exceed the {display_max_ns} ns display range; "
    f"{context_switches} exceed 1 µs (context switches), with the remainder sub-µs stalls.",
    ha="center",
    fontsize=8
)

plt.figtext(
    0.5,
    0.02,
    f"Maximum observed delta: {max_delta} ns. "
    f"calibrate.cpp analysis threshold: {analysis_threshold_ns} ns.",
    ha="center",
    fontsize=8
)

plt.savefig("results/timer_calibration.png", dpi=150)