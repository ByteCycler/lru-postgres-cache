# cpp-postgres-lru-engine

A thread-safe C++17 read-through / write-through storage engine that sits
in front of PostgreSQL and serves hot reads from an in-memory **O(1) LRU
cache**, falling back to disk only on a cache miss.

```
                 ┌────────────────────────┐
   get(id) ───▶  │       LRUCache          │  O(1) hash-map lookup
                 │  (own internal mutex)   │  O(1) list splice to MRU
                 │  unordered_map<int,     │  hit/miss/eviction counters
                 │   list<Record>::iter>   │  concurrent-safe: many
                 │  +  list<Record>        │  threads can hit it at once
                 └──────────┬──────────────┘
                             │ miss
                             ▼
                 ┌────────────────────────┐
                 │  single pqxx::connection│  DB calls serialized behind
                 │  guarded by dbMutex_    │  one mutex (see below)
                 └──────────┬──────────────┘
                             ▼
                 ┌────────────────────────┐
                 │       PostgreSQL        │  disk-backed source of truth
                 │    (accounts table)     │  parameterized queries
                 └────────────────────────┘
```

## Why this exists

Hitting a disk-backed database (PostgreSQL, MySQL, …) on every read adds
single-digit-to-tens-of-milliseconds of latency. In latency-sensitive
systems (trading engines, payment gateways, high-QPS APIs) that cost adds
up fast. This project implements the classic fix from first principles:
an **LRU cache** built on a hash map + doubly linked list, made
**thread-safe**, and wired into a **write-through / read-through**
persistence layer — with real, measured numbers to back up the design,
not just an asserted complexity class.

- **Write-through:** every write lands in PostgreSQL first (durability),
  then updates the cache, so the cache is never the source of truth.
- **Read-through:** reads check the cache first; on a miss, the engine
  queries PostgreSQL, returns the result, and repopulates the cache for
  the next request.

## Data structures & complexity

| Structure | Purpose | Complexity |
|---|---|---|
| `std::unordered_map<int, list<Record>::iterator>` | key → node lookup | O(1) average |
| `std::list<Record>` (doubly linked list) | maintains recency order (front = MRU, back = LRU) | O(1) insert/remove/move via `splice` |
| **`get(id)`** | cache lookup + promote to MRU | **O(1)** amortized, serialized by the cache's own internal mutex |
| **`put(record)`** | insert/update + evict LRU if full | **O(1)** amortized, serialized by the cache's own internal mutex |

The `std::list::splice` call is the key trick: moving an already-owned
node to the front of the list is a pointer relink, not a copy — so
"promote to most-recently-used" costs O(1) regardless of cache size.

## Concurrency model — and its honest limit

Two independent locks exist in this codebase, protecting two different
things:

1. **`LRUCache`'s internal mutex** protects the hash map + linked list.
   A cache *hit* still mutates the structure (it splices the accessed
   node to the front to update recency), so `get()` can't use a
   shared/read lock — every cache operation takes the same exclusive
   mutex. **Cache hits — the fast, common-case path this whole design
   exists for — are fully concurrent**: many threads can call `get()`/
   `put()` against a shared cache at once, each waiting only briefly on
   this lock. Verified data-race-free under ThreadSanitizer.

2. **`DatabaseManager`'s `dbMutex_`** guards a single, long-lived
   `pqxx::connection`. libpqxx connections are not safe to use from
   multiple threads at once, so every DB call — every cache *miss*, and
   every write — is serialized behind this one mutex: only one thread
   talks to Postgres at a time.

**This is a deliberate simplification, not an oversight.** It keeps the
database-access code small (one connection, one mutex, no pool
bookkeeping) at a known cost: throughput under a **miss-heavy**
concurrent workload is bottlenecked on a single DB connection. The
cache layer's own concurrency is unaffected — only the fallback path is
serialized. The natural fix, if that path became a bottleneck, is a
connection pool (N pre-opened connections handed out via an RAII
checkout), which would let up to N threads reach Postgres concurrently
instead of 1. That's noted as the first item under
[Possible extensions](#possible-extensions) rather than built in, to
keep this version's surface area easy to explain end-to-end.

## Project layout

```
cpp-postgres-lru-engine/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── .gitignore
├── .github/workflows/ci.yml     # CI: build + tests + live-Postgres run + benchmark + TSan
├── include/
│   ├── Record.h
│   ├── LRUCache.h                # cache + CacheStats
│   └── DatabaseManager.h         # single mutex-guarded connection
├── src/
│   ├── LRUCache.cpp
│   ├── DatabaseManager.cpp
│   └── main.cpp
├── benchmarks/
│   └── benchmark.cpp             # measured latency/throughput, not just Big-O claims
└── tests/
    └── test_engine.cpp           # 10 unit tests incl. concurrency, no DB required
```

## Build & run

### Prerequisites
- CMake ≥ 3.16, a C++17 compiler, pthreads
- `libpqxx-dev` (PostgreSQL C++ client library)
- A running PostgreSQL instance (for the full engine and benchmark — the
  unit tests do **not** require a database)

```bash
# Ubuntu/Debian
sudo apt-get install libpqxx-dev cmake postgresql
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

This produces three binaries:
- `run_tests` — unit tests for the LRU cache logic, including a
  multi-threaded stress test (pure, no DB)
- `lru_db_engine` — the full demo engine (requires PostgreSQL)
- `benchmark` — measures real latency/throughput numbers (requires PostgreSQL)

### Run the unit tests

```bash
./run_tests
# or: ctest --output-on-failure
```

### Run the full engine / benchmark

Both connect by default to `host=localhost port=5432 dbname=postgres
user=postgres password=password`. Override with `DB_CONN_STRING`:

```bash
export DB_CONN_STRING="host=127.0.0.1 port=5432 dbname=postgres user=postgres password=password"
./lru_db_engine
./benchmark
```

## Verification: this was actually tested

Before publishing, the full pipeline was run against a **live local
PostgreSQL instance**, not just compiled:

**Unit tests (10/10 passing, including a multi-threaded stress test):**
```
=== Running LRUCache Unit Tests ===
[PASS] test_lru_insertion_and_get
[PASS] test_cache_miss_returns_nullopt
[PASS] test_lru_eviction_policy
[PASS] test_put_update_existing_key_promotes_to_mru
[PASS] test_get_promotes_recency_order
[PASS] test_zero_capacity_cache_never_stores
[PASS] test_size_tracks_entry_count
[PASS] test_stats_track_hits_misses_evictions
[PASS] test_reset_stats_clears_counters_not_entries
[PASS] test_concurrent_access_is_thread_safe
=== 10/10 tests passed ===
```

**ThreadSanitizer:** the unit tests and the full concurrent benchmark
(cache under 8 threads, plus the shared-connection DB path) were
rebuilt with `-fsanitize=thread` and run — **zero data races reported**.

**Full engine, live run against PostgreSQL** (cache capacity = 2,
demonstrating a hit, an eviction, and a fallback-then-recache):
```
--- Inserting Records ---
Inserted 101, 102 (write-through: Postgres + cache)

--- Querying Records ---
Record 101 -> Isha Gupta (balance $15000) [LRU CACHE HIT]

--- Adding 3rd Record (Triggers LRU Eviction) ---
Inserted 103 -> capacity (2) exceeded, LRU entry (102) evicted from cache

--- Verifying Eviction & Fallback ---
Record 101 -> Isha Gupta (balance $15000) [LRU CACHE HIT]
Record 102 -> Alex Smith (balance $8200.5) [POSTGRES FALLBACK]
Record 102 -> Alex Smith (balance $8200.5) [LRU CACHE HIT]

--- Cache Stats ---
hits=3 misses=1 evictions=2 hitRate=75%
```
Record 102 was correctly evicted (it was the LRU entry once record 103
was inserted into a capacity-2 cache) and, on the next request, the
engine correctly missed the cache, fell back to PostgreSQL, and
re-cached it — exactly the read-through behavior the design targets.

CI (`.github/workflows/ci.yml`) runs this same sequence — build, unit
tests, a live run against a PostgreSQL service container, the
benchmark, and a ThreadSanitizer rebuild — on every push and PR.

## Benchmark: measured numbers, not just Big-O

`benchmarks/benchmark.cpp` measures three things directly rather than
just asserting complexity classes. Representative results from this
machine (varies by hardware; re-run `./benchmark` on yours):

```
--- Part 1: Pure in-memory cache latency ---
[Pure cache] 500000 get() calls (all hits) in ~8-13 ms => 40-65M ops/sec, ~0.015-0.027 us/op avg

--- Part 2: Cache hit vs. PostgreSQL fallback latency ---
[Cache hit]  100000 getRecord() calls (all hits) in ~1.5-2 ms => ~0.015-0.02 us/op avg
[Cache miss] 20 forced Postgres fallback calls, ~0.17-0.21 ms/op avg
[Speedup]    cache hit is ~9,800-15,000x faster than a Postgres round-trip
[Stats]      hitRate ~100% for the hot key, evictions tracked for the filler keys

--- Part 3: Concurrent multi-threaded throughput ---
[Concurrent] 8 threads x 100000 ops (mixed get/put) = 800000 total ops
             in ~26-31 ms => ~26-31M ops/sec
```

Takeaways:
- A cache hit is roughly **4 orders of magnitude** faster than the same
  read served by PostgreSQL over a local TCP connection — this is the
  entire reason the cache layer exists, quantified rather than assumed.
- Part 3 exercises `LRUCache` directly under 8 concurrent threads (not
  the DB layer, which is intentionally serialized — see
  [Concurrency model](#concurrency-model--and-its-honest-limit)), and
  sustains tens of millions of ops/sec, confirming the cache's mutex
  isn't a meaningful bottleneck at these thread counts.
- Numbers above are from an unloaded local machine talking to a local
  Postgres instance (no network latency); a remote database would widen
  the relative speedup further, since the miss path's dominant cost
  (network round-trip) grows while the hit path's cost doesn't change.

## Test coverage

`tests/test_engine.cpp` covers the cache in isolation (no DB dependency,
so it also runs in any CI environment without infrastructure setup):

- basic insertion and hit
- miss on an unknown key
- eviction picks the true LRU entry, not insertion order
- updating an existing key refreshes its value **and** promotes it to MRU
- `get()` on an entry changes eviction order (recency, not just insertion order)
- a zero-capacity cache never stores anything (edge case)
- `size()` tracks distinct keys, not `put()` call count
- hit/miss/eviction counters are accurate (`CacheStats`)
- `resetStats()` clears counters without touching cached entries
- 8 threads doing 2,000 mixed get/put ops each against a shared cache —
  survives with no crashes/UB and the reported hit+miss count exactly
  matches the number of `get()` calls issued

## Design notes / trade-offs

- **Why `std::list` instead of a hand-rolled linked list?** `std::list`
  already gives O(1) splice/insert/erase with stable iterators, which is
  exactly what the hash map needs to store as its value — reimplementing
  it would add code without changing the complexity story.
- **Why `exec_params` instead of string concatenation?** Building SQL by
  concatenating strings is an injection risk. Every query uses
  `libpqxx`'s parameterized queries (`exec_params`).
- **Why one mutex for the cache instead of a `shared_mutex`?** A cache
  *hit* mutates the recency list (moves the node to the front), so it
  isn't a read-only operation at the data-structure level — a
  shared/read lock would still need to be upgraded to exclusive on
  every hit, which provides no benefit over a plain mutex while adding
  complexity.
- **Why one shared DB connection instead of a pool?** Simplicity. A pool
  is the "correct" production answer for a miss-heavy concurrent
  workload, but it's also more code and another thing to get right
  (checkout/return, exhaustion, connection health). This version
  optimizes for a small, fully-understandable codebase where the
  interesting concurrency story — the cache — is easy to isolate from
  DB plumbing. See [Concurrency model](#concurrency-model--and-its-honest-limit)
  for the exact trade-off this implies.
- **Write-through vs. write-back:** this engine writes to PostgreSQL
  synchronously before updating the cache, trading a bit of write
  latency for the guarantee that the cache can never diverge from durable
  storage after a crash. A write-back variant (cache first, flush to DB
  asynchronously) would be a natural extension.

## Possible extensions

- **Connection pool** (N pre-opened `pqxx::connection`s handed out via
  an RAII checkout) to parallelize the DB fallback path instead of
  serializing it behind one connection — the direct fix for the
  limitation described above.
- **Sharded cache** (N independent `LRUCache` instances keyed by
  `hash(id) % N`) to reduce lock contention further under heavy
  concurrent read load, at the cost of only approximate global LRU order.
- TTL-based expiry alongside LRU.
- Write-back mode with a background flush thread.
- Expose `CacheStats` over an HTTP `/metrics` endpoint (e.g. Prometheus
  text format) for real observability in a running service.

## License

MIT — see [LICENSE](LICENSE).
