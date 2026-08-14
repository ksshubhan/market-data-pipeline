Cache is very small fast memory sitting on chips. When you read something it gets copied into the cache and the next read of it comes from the cache instead. 
Our M2 has several levels of this. L1 -> L2 -> RAM/

What is a cache lines?
Cache does not move data byte by byte instead hardware fetches the data in blocks. These blocks are called cache lines. 
Our M2 has a cache line of 128. 

What is systcl?
a command line tool for reading kernel maintained system values like hardware properties, configurations and counters
sysctl hw.cachelinesize asks the kernel how big a cache line on our machine and the kernal answers 128 because it knows what chip the cache is running on. 

What is false sharing?
Consider two threads of different cores. Our queue has 2 counters. The producer writes tail constantly. The consumer writes head constantly. They never touch each other's counter — logically completely independent but what if they sit next to each other in memory?: we get cache coherence — the hardware machinery keeping cores' caches consistent -  tracking ownership per line, not per variable. It cannot tell that head and tail are unrelated. All it sees is: two cores keep writing to the same line.

So every time the producer writes tail, the hardware invalidates the consumer's copy of that line. The consumer must re-fetch it before touching head. Then the consumer writes head, invalidating the producer's copy. The line bounces back and forth across the interconnect, and both threads slow down badly.
so they appear to share a resource, but only as an accident of where the compiler put them.

What is padding?
Padding is the solution to false sharing. Padding is where we force the head and the tail apart so they cannot land within the same cache line:
[   head   ][ 120 bytes of padding ][   tail   ]
└──── cache line 1 ────────────────┘└─ line 2 ─┘

Obviously this means extra memory but we buy speed, thats the trade and that trade is measured by A2. 

In our code the constants: hardware_destructive_interference_size and hardware_constructive_interference_size advise us on padding:
hardware_destructive_interference_size — put things at least this far apart to avoid false sharing. Your library says 256.
hardware_constructive_interference_size — things within this span will probably share a line, so group things you use together. Your library says 64.

In summary the 3 numbers:
128 - size of our cache line i.e. blocks
256 - what the library recommends for seperation to prevent false sharing (hypothesis: some ARM implementations fetch catch lines in pairs so 2 objects 128 bytes could still be dragged into a single coherence event meaning 128 bytes of separation wouldn't actually prevent bouncing)
64 - what the library recommends for grouping for data that is used together. 

What is libc++, libstdc++?
C++ has a standard library - std::cout, std::vector etc. The standard specifies what it does. Several groups have written their own:
libc++ — LLVM's version. What clang uses. What you're using.
libstdc++ — GNU's version, shipping with GCC. What most Linux systems use.
Different codebases, same specification

We tested two libc++ builds — Homebrew's (220108) and Apple's (210106) — and both said 256/64.
If only one had said 256 we could have assumed it was a bug but two different versions giving identical answers implies a stable policy.

But libstdc++ is completely independent i.e. different authors, code

What is A2?
is the test we carry out to easure the trade off from padding to prevent false sharing. 
We can build three variants of the queue with head and tail separated by 64, 128, and 256 bytes, run the microbenchmark on each, compare. Expected 64 slowest. The real question is whether 128 and 256 differ. No results yet.
We measure how fast the queue works in terms of latency - how long one individual operation takes and throughput 

What is bouncing?
Each core has its own L1 cache. When A reads a cache line, a copy lands in A's L1. When core B reads the same line, a copy lands in B's L1. So now we have 2 copies of the same 128 bytes in two places. Now say A writes to it well now B's copy is instantly stale - it holds a value that is no longer true. So now if B is allowed to use its value then the two cores would disagree about the contents and your program would become nondeterministic garbage so the hardware runs a coherence protocol. 

Coherence protocl:
Before A can write it must gain exclusive ownership of that line which means telling every other core holding a copy of the line to discard it so B's copy is invalidated. 

Next time B wants the line, it isn't in B's cache any more. B must request it — and since A now owns the modified version, the line has to be transferred from A to B across the chip's interconnect. Then B writes, which invalidates A's copy. Then A wants it back. And so on.

That's the bouncing: ownership of one 128-byte line ricocheting between cores, with an interconnect round-trip every time

Bouncing is expensive. Reading from your own L1 is a few cycles. Getting a line transferred from another core's cache is on the order of tens of cycles — some multiple of ten times worse. Do that on every single queue operation and you've turned a handful of instructions into a memory-system negotiation.

False sharing does not cause bouncing it is the name given to the scenario here bouncing is unncessary where the two variables are logically unrelated but they only collide because of memeory layout 