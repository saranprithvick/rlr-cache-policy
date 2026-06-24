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

## Status

| Task | Description | Status |
|---|---|---|
| 1 | Clone and build libCacheSim | ✅ Done |
| 2 | Study libCacheSim plugin API | ✅ Done |
| 3 | Re-read paper algorithm sections | ✅ Done |
| 4 | Trace format verification | ⏳ Deferred to evaluation phase |
| 5 | Create RLR.c with per-line metadata struct | ✅ Done |
| 6 | Implement RD accumulator | ✅ Done |
| 7 | Implement Age Counter update | ✅ Done |
| 8 | Implement priority computation | ✅ Done |
| 9 | Implement victim selection | ✅ Done |
| 10 | Implement cache bypass | ✅ Done |
| 11 | Register RLR in CMakeLists and policy registry | ✅ Done |
| 12 | Build and verify | ✅ Done |
| 13 | Smoke test | ✅ Done |
| 14 | Run benchmark traces | 🔲 Pending |
| 15 | Verify results against paper | 🔲 Pending |
| 16 | Document storage overhead | 🔲 Pending |
| 17 | Write results report | 🔲 Pending |

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
