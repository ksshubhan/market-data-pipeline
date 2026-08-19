import matplotlib.pyplot as plt


with open("results/timer_calibration.csv", "r") as f:
    raw = f.read()


display_max_ns = 130
analysis_threshold_ns = 70


deltas = [int(line) for line in raw.split()]

# Caption statistics
beyond_display = len([x for x in deltas if x > display_max_ns])
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