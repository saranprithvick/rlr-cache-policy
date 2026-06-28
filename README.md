# RLR Cache Replacement Policy

An implementation of the **Reinforcement Learned Replacement (RLR)** cache
replacement policy inside the [libCacheSim](https://github.com/1a1a11a/libCacheSim)
simulation framework, written in C.

This is a replication project based on the paper:

> *Cost Effective Cache Replacement Policy Using ML*

---

## What is RLR?

RLR is a static cache replacement policy derived from insights gained by
training a reinforcement learning agent on LLC (Last Level Cache) access
patterns. Rather than running a neural network at runtime, the authors
analyzed the trained agent's behavior and distilled it into a lightweight
hardware-implementable policy.

The key insight is that three low-cost per-line registers are sufficient
to make near-optimal eviction decisions:

| Register | Size | Purpose |
|---|---|---|
| Age Counter | 5-bit saturating | Counts set misses since last access |
| Hit Register | 1-bit | Set to 1 when the line receives a demand hit |
| Type Register | 1-bit | 1 = non-prefetch access, 0 = prefetch access |

A per-set **Accumulator** tracks preuse distances across demand hits and
recomputes the predicted Reuse Distance every 32 hits: RD = 2 × average_preuse_distance

On every eviction decision, each cache line is scored: P_line = 8 × P_age + P_type + P_hit

The line with the **lowest score** is evicted. Ties are broken by recency.

---

## Repository Structure
rlr-cache-policy/

├── README.md

├── src/

│   └── RLR.c               # RLR policy implementation

└── patch/

└── cacheObj.h.diff     # Patch to add RLR metadata to libCacheSim

---

## Setup and Build

### Prerequisites

- macOS or Linux
- CMake ≥ 3.12
- GCC or Clang
- libCacheSim dependencies (`zstd`, `lz4`, `glib-2.0`)

### 1. Clone this repository with the libCacheSim submodule

```bash
git clone https://github.com/<your-username>/rlr-cache-policy.git
cd rlr-cache-policy
git submodule update --init --recursive
```

### 2. Apply the cacheObj.h patch

```bash
cd libCacheSim
git apply ../patch/cacheObj.h.diff
```

### 3. Copy RLR.c into libCacheSim

```bash
cp ../src/RLR.c libCacheSim/cache/eviction/RLR.c
```

### 4. Build libCacheSim

```bash
mkdir build && cd build
cmake ..
cmake --build . -j4
```

> **Note:** Tasks 11–13 (registering RLR in the CMakeLists and policy
> registry) are in progress. The build step will be updated once
> integration is complete.

---

## Implementation Notes

### Simulator Limitation — Type Register

The paper's Type Register distinguishes between **prefetch** and **demand**
accesses, allowing RLR to prioritize eviction of unreused prefetched lines.
libCacheSim's `request_t` struct does not expose a prefetch flag, and its
`req_op_e` enum does not model hardware prefetch traffic.

As a result, `type_register` is set to `1` for all accesses, which is
functionally equivalent to disabling the Type Register entirely. The paper
reports that disabling the Type Register reduces IPC speedup over LRU by
approximately **30%** (Section IV-B). Evaluation results from this
implementation should be interpreted with this caveat in mind.

A complete evaluation would require ChampSim traces with explicit prefetch
distinction, which is the simulator used in the original paper.

### Age Counter Optimization

Following the paper's hardware optimization (Section IV-C), the Age Counter
counts **set misses** rather than every set access. This reduces overhead
from 5 bits to 3 bits per line in the original hardware design. In this
software simulation, the counter is stored as a `uint8_t` for simplicity
while preserving the same counting semantics.

### RD Computation

Reuse Distance is computed as: RD = accumulated_preuse >> 4

This is equivalent to right-shifting by 5 (divide by 32) then left-shifting
by 1 (multiply by 2), combined into a single right-shift by 4.

## Trace Compatibility — Known Limitation

### SPEC CPU 2006 ChampSim Traces

The original paper evaluates RLR using SPEC CPU 2006 traces recorded by
ChampSim. We attempted to use these traces from
[Zenodo](https://zenodo.org/records/10959705) but found them fundamentally
incompatible with libCacheSim due to a simulation level mismatch.

**The core issue:** ChampSim is a full CPU pipeline simulator that models
private L1 (32KB) and L2 (256KB) caches before the LLC. Its traces record
every memory access the program makes, including those that hit in L1/L2
and never reach the LLC. libCacheSim operates directly at the LLC access
level with no concept of upstream private caches filtering accesses.

When fed ChampSim traces directly, libCacheSim sees only the tiny hot
working set that fits in L1/L2 — resulting in near-zero miss ratios
regardless of cache size or replacement policy. For example:

- `454.calculix-104B`: only 567 unique objects in 50 million requests
- `403.gcc-48B`: only 373 unique objects in 50 million requests

To use ChampSim traces correctly in libCacheSim, one would need to
simulate L1 and L2 caches during conversion and output only the accesses
that miss both — effectively building a mini cache hierarchy inside the
converter. This is outside the scope of this replication.

### Traces Used

We instead evaluate on two traces included with libCacheSim that operate
at the correct abstraction level:

| Trace | Type | Requests | Description |
|---|---|---|---|
| cloudPhysicsIO.vscsi | Block storage I/O | 113,872 | VMware block I/O trace |
| twitter_cluster52.csv | KV cache | 1,000,000 | Twitter Memcached trace |

---

## Evaluation Results

### cloudPhysicsIO.vscsi — Block Storage Trace

This trace most closely resembles LLC behavior — fixed-size block accesses
with spatial locality. RLR consistently outperforms both LRU and FIFO
across all cache sizes.

| Cache Size | LRU | FIFO | RLR | RLR vs LRU |
|---|---|---|---|---|
| 1MB | 0.8646 | 0.8766 | **0.8514** | −1.32% |
| 2MB | 0.8501 | 0.8609 | **0.8352** | −1.49% |
| 4MB | 0.8428 | 0.8497 | **0.8291** | −1.37% |
| 8MB | 0.8382 | 0.8432 | **0.8251** | −1.31% |

### twitter_cluster52.csv — In-Memory KV Cache Trace

RLR underperforms LRU on this workload. This is expected — RLR was
specifically designed for CPU LLC workloads with set-associative access
patterns and preuse-distance-based reuse prediction. The Twitter trace is
a Memcached key-value workload with variable-size objects and
application-level key popularity patterns that do not exhibit the spatial
and temporal locality assumptions RLR relies on.

| Cache Size | LRU | FIFO | RLR | RLR vs LRU |
|---|---|---|---|---|
| 100KB | **0.4061** | 0.4349 | 0.4420 | +3.59% |
| 200KB | **0.3626** | 0.3893 | 0.3996 | +3.70% |
| 500KB | **0.3096** | 0.3350 | 0.3447 | +3.51% |
| 1MB | **0.2687** | 0.2935 | 0.3051 | +3.64% |

---

## Implementation Limitations

### 1. Type Register disabled

The paper's Type Register distinguishes prefetch from demand accesses,
allowing RLR to deprioritize unreused prefetched lines. libCacheSim's
`request_t` has no prefetch flag and its `req_op_e` enum does not model
hardware prefetch traffic. As a result `type_register = 1` for all
accesses, effectively disabling this feature. The paper reports this
causes approximately **30% reduction in IPC speedup** over LRU
(Section IV-B).

### 2. Associativity hardcoded to 16

`common_cache_params_t` in libCacheSim does not expose an associativity
field. We hardcode 16-way associativity matching the paper's evaluated
LLC configuration (2MB, 16-way, 26-cycle latency per Table III).

### 3. Age Counter stored as uint8_t

The paper uses a 5-bit saturating hardware counter. In software we store
it as `uint8_t` (8 bits) for simplicity, capping at 31 in the priority
computation to match the paper's 5-bit behavior.

### 4. SPEC CPU 2006 trace incompatibility

As documented above, ChampSim traces cannot be used directly in
libCacheSim due to simulation level mismatch. Results are therefore
reported on storage and KV cache traces rather than CPU benchmarks.

---

## Storage Overhead Analysis

Per the paper (Section IV-B), each cache line carries:
- 5-bit Age Counter (stored as `uint8_t` in software)
- 1-bit Hit Register
- 1-bit Type Register

Plus a 3-bit per-set miss counter for the Age Counter optimization.

For a 2MB 16-way LLC with 64B cache lines:
- Total cache lines: 2MB / 64B = 32,768 lines
- Bits per line: 7 bits
- Total line overhead: 32,768 × 7 = 229,376 bits = 28 KB
- Per-set counter: (32,768 / 16) × 3 = 6,144 bits = 0.75 KB
- **Total overhead: ~28.75 KB**

The paper reports 16.75 KB using optimized hardware encoding. Our figure
is slightly higher due to byte-aligned software storage.

## Status

| Task | Description | Status |
|---|---|---|
| 1 | Clone and build libCacheSim | ✅ Done |
| 2 | Study libCacheSim plugin API | ✅ Done |
| 3 | Re-read paper algorithm sections | ✅ Done |
| 4 | Trace format verification | ✅ Done (with documented limitations) |
| 5 | Create RLR.c with per-line metadata struct | ✅ Done |
| 6 | Implement RD accumulator | ✅ Done |
| 7 | Implement Age Counter update | ✅ Done |
| 8 | Implement priority computation | ✅ Done |
| 9 | Implement victim selection | ✅ Done |
| 10 | Implement cache bypass | ✅ Done |
| 11 | Register RLR in CMakeLists and policy registry | ✅ Done |
| 12 | Build and verify | ✅ Done |
| 13 | Smoke test | ✅ Done |
| 14 | Run benchmark traces | ✅ Done |
| 15 | Verify results against paper | ✅ Done |
| 16 | Document storage overhead | ✅ Done |
| 17 | Write results report | ✅ Done |

---

## Smoke Test Results

Initial validation was run on the `cloudPhysicsIO.vscsi` sample trace
included with libCacheSim, using a 1MB cache size.

| Policy | Miss Ratio | Byte Miss Ratio |
|--------|------------|-----------------|
| FIFO   | 0.8766     | 0.9827          |
| LRU    | 0.8646     | 0.9813          |
| RLR    | **0.8514** | **0.9800**      |

RLR achieves the lowest miss ratio of all three policies on this trace,
consistent with the paper's claim that RLR outperforms LRU and FIFO.

> **Note on throughput:** RLR runs at 1.67 MQPS vs LRU's 2.47 MQPS in
> simulation. This is expected — RLR scans all lines in a set to find the
> lowest priority victim, whereas LRU simply pops the tail of a linked
> list. This overhead is a simulation artifact and would not apply in a
> real hardware implementation where priority computation runs in parallel
> with tag comparison.

## Reference

Paper: *Cost Effective Cache Replacement Policy Using ML*
Simulation framework: [libCacheSim](https://github.com/1a1a11a/libCacheSim)
