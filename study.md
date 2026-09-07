Cache is very small fast memory sitting on chips. When you read something it gets copied into the cache and the next read of it comes from the cache instead. 
Our M2 has several levels of this. L1 -> L2 -> RAM/

What is a cache lines?
Cache does not move data byte by byte instead hardware fetches the data in blocks. These blocks are called cache lines. 
Our M2 has a cache line of 128. 

Correction, 7 Sep: "cache line" turned out to be two different things and this line
conflates them. 128 is the fetch granularity - how much the hardware pulls in on a
miss, which is what sysctl reports. The coherence granule - the unit the ownership
protocol actually tracks - measures 64 on this machine. A2b measured it. Two
counters 64 bytes apart are inside the same 128-byte fetch line and pay nothing
for it, which is only possible if coherence is tracked at 64.

What is systcl?
a command line tool for reading kernel maintained system values like hardware properties, configurations and counters
sysctl hw.cachelinesize asks the kernel how big a cache line on our machine and the kernal answers 128 because it knows what chip the cache is running on. 

Correction, 7 Sep: the kernel answers 128 and that answer is true, but it is
answering about fetch, not about coherence. It is not the number that governs
padding. This is worth remembering as a general shape: the system told us a real
number to a question we had not asked precisely enough.

What is false sharing?
Consider two threads of different cores. Our queue has 2 counters. The producer writes tail constantly. The consumer writes head constantly. They never touch each other's counter — logically completely independent but what if they sit next to each other in memory?: we get cache coherence — the hardware machinery keeping cores' caches consistent -  tracking ownership per line, not per variable. It cannot tell that head and tail are unrelated. All it sees is: two cores keep writing to the same line.

So every time the producer writes tail, the hardware invalidates the consumer's copy of that line. The consumer must re-fetch it before touching head. Then the consumer writes head, invalidating the producer's copy. The line bounces back and forth across the interconnect, and both threads slow down badly.
so they appear to share a resource, but only as an accident of where the compiler put them.

What is padding?
Padding is the solution to false sharing. Padding is where we force the head and the tail apart so they cannot land within the same cache line:
[   head   ][ 120 bytes of padding ][   tail   ]
└──── cache line 1 ────────────────┘└─ line 2 ─┘

Obviously this means extra memory but we buy speed, thats the trade and that trade is measured by A2. 

Correction, 7 Sep: the diagram pads to 128 because that was the assumed line size.
64 is enough on this machine - measured, not assumed. The diagram is still a fair
picture of the idea, just not of the number we ended up using.

In our code the constants: hardware_destructive_interference_size and hardware_constructive_interference_size advise us on padding:
hardware_destructive_interference_size — put things at least this far apart to avoid false sharing. Your library says 256.
hardware_constructive_interference_size — things within this span will probably share a line, so group things you use together. Your library says 64.

In summary the 3 numbers:
128 - size of our cache line i.e. blocks
256 - what the library recommends for seperation to prevent false sharing (hypothesis: some ARM implementations fetch catch lines in pairs so 2 objects 128 bytes could still be dragged into a single coherence event meaning 128 bytes of separation wouldn't actually prevent bouncing)
64 - what the library recommends for grouping for data that is used together. 

Correction, 7 Sep: the pairs hypothesis is dead. If lines were fetched in pairs the
penalty would appear at 128 and vanish at 256. A2b shows no penalty at 128 at all -
64, 128 and 256 sit inside 0.207% of each other. The prediction the hypothesis makes
is exactly the thing that did not happen, which is the cleanest way a hypothesis can
die.

There are 4 numbers now, not 3:
128 - fetch granularity, what sysctl reports
256 - what libc++ says, and what libstdc++ says by default
64  - what libc++ says for constructive, AND what libstdc++ says for every specific
      ARM core, AND what A2b measured as the coherence granule on this chip
The three 64s arriving from three unrelated directions is the interesting part.

What is libc++, libstdc++?
C++ has a standard library - std::cout, std::vector etc. The standard specifies what it does. Several groups have written their own:
libc++ — LLVM's version. What clang uses. What you're using.
libstdc++ — GNU's version, shipping with GCC. What most Linux systems use.
Different codebases, same specification

We tested two libc++ builds — Homebrew's (220108) and Apple's (210106) — and both said 256/64.
If only one had said 256 we could have assumed it was a bug but two different versions giving identical answers implies a stable policy.

But libstdc++ is completely independent i.e. different authors, code

Answered 7 Sep, and the answer was not the one expected. libstdc++ 15 on ARM64
Linux also says 256/64 in a default build. So no disagreement.

But then it moves, and this is the actual finding. Ask GCC to compile for a
specific ARM core and the number changes:

  generic       256
  apple-m1      256
  neoverse-n1    64
  neoverse-v1    64
  neoverse-v2    64
  cortex-a76     64
  cortex-x3      64

Every core GCC has a real model of says 64. Only the generic fallback says 256.

Why: GCC's aarch64-cores.def does recognise apple-m1, but wires all Apple parts to
the generic_armv8_a tuning model. The constant is emitted from that tuning model's
prefetch table, and the generic table leaves the cache-line field at -1. So 256 is
what GCC emits when it has nothing to say. It is a default, not a measurement, and
-mcpu=apple-m1 looks like asking GCC about the M2 while actually being routed to the
table with the field unset.

So the thing to say out loud is not "the library is wrong by 4x". It is that a direct
measurement and a compiler independently arrive at 64 - A2b on this chip, GCC for
every ARM core it models - and 256 is the price of not telling the toolchain what it
is building for. libc++ has no -mcpu notion at all so it cannot even participate in
that distinction.

What is A2?
is the test we carry out to easure the trade off from padding to prevent false sharing. 
We can build three variants of the queue with head and tail separated by 64, 128, and 256 bytes, run the microbenchmark on each, compare. Expected 64 slowest. The real question is whether 128 and 256 differ. No results yet.
We measure how fast the queue works in terms of latency - how long one individual operation takes and throughput 

Results, 4 Sep. The prediction above is wrong and worth keeping wrong on the page.
"Expected 64 slowest" - 64 was the fastest of the three, by a margin too small to
mean anything:

  16 bytes apart   1.849 G ops/s
  64 bytes apart   6.595 G ops/s
  128 bytes apart  6.581 G ops/s
  256 bytes apart  6.585 G ops/s

Spread across 64/128/256 is 0.207%. The 16-byte arm is 3.566x slower with no overlap
between populations, so the test does detect false sharing when it is there - that is
the positive control, and without it the three-way tie would only mean the
measurement was blind.

"The real question is whether 128 and 256 differ" - they do not, and neither differs
from 64, so the real question turned out to be a different one: where does the
penalty actually start? Somewhere between 16 and 64. That was not asked here.

What is bouncing?
Each core has its own L1 cache. When A reads a cache line, a copy lands in A's L1. When core B reads the same line, a copy lands in B's L1. So now we have 2 copies of the same 128 bytes in two places. Now say A writes to it well now B's copy is instantly stale - it holds a value that is no longer true. So now if B is allowed to use its value then the two cores would disagree about the contents and your program would become nondeterministic garbage so the hardware runs a coherence protocol. 

Coherence protocl:
Before A can write it must gain exclusive ownership of that line which means telling every other core holding a copy of the line to discard it so B's copy is invalidated. 

Next time B wants the line, it isn't in B's cache any more. B must request it — and since A now owns the modified version, the line has to be transferred from A to B across the chip's interconnect. Then B writes, which invalidates A's copy. Then A wants it back. And so on.

That's the bouncing: ownership of one 128-byte line ricocheting between cores, with an interconnect round-trip every time

Correction, 7 Sep: 64-byte, not 128-byte. The protocol tracks ownership at the
coherence granule and that is 64 here. The description of the mechanism is right,
only the size is wrong - and the size is the whole reason we were measuring.

Bouncing is expensive. Reading from your own L1 is a few cycles. Getting a line transferred from another core's cache is on the order of tens of cycles — some multiple of ten times worse. Do that on every single queue operation and you've turned a handful of instructions into a memory-system negotiation.

False sharing does not cause bouncing it is the name given to the scenario here bouncing is unncessary where the two variables are logically unrelated but they only collide because of memeory layout