# cpp-cache

A general-purpose, high-performance concurrent cache library for C++20.

**Sponsored by [SIMYL RESEARCH](https://simylresearch.com/en/)**

## Key Features

- **Single-flight per key** — at most one computation per key at any time; concurrent requests for the same key wait and share the result (thundering herd prevention)
- **Positive and negative caching** — successful results and failures are cached with independent TTLs
- **Safe value lifetime** — values are managed via `shared_ptr<Value>`; no raw pointers exposed
- **Lazy invalidation** — `invalidate()` marks entries as invalid without immediate physical removal
- **LRU eviction** — entries in `Computing` state are never evicted
- **External mutability** — cached values can be mutated through the returned `shared_ptr`
- **Thread-safe** — global mutex protects structure; per-entry mutex + condition variable for single-flight coordination. TSAN verified.

## Architecture

```
include/cache/
  entry_state.H    — EntryState enum (Empty, Computing, Ready, Failed, Invalidated)
  cache_result.H   — CacheResult<Value> typed result with origin tracking
  cache.H          — Cache<Key, Value, Cmp> main class

tests/unit/            — Basic functionality tests
tests/concurrency/     — Single-flight, parallel progress, race, stress tests
```

Reuses [Aleph-w](https://github.com/lrleon/Aleph-w):
- `OLhashTable` — open-addressing hash table with linear probing
- `Dlink` — doubly-linked list for LRU ordering

## API

```cpp
#include <cache/cache.H>

using namespace CppCache;

// Miss solver: returns shared_ptr on success, nullptr on cacheable failure
auto solver = [](const std::string & key) -> std::shared_ptr<MyData> {
    auto data = fetch_from_backend(key);
    if (!data) return nullptr;  // negative cache
    return std::make_shared<MyData>(std::move(*data));
};

Cache<std::string, MyData> cache(
    1024,               // capacity
    std::chrono::seconds(300),  // positive TTL
    std::chrono::seconds(30),   // negative TTL
    solver
);

// Main operation: get or compute (single-flight per key)
CacheResult<MyData> result = cache.get_or_compute("my-key");

if (result.is_positive()) {
    use(*result.value());           // shared_ptr<MyData>
    result.value()->mutate();       // external mutation OK
}

if (result.is_negative()) {
    // cached failure — solver won't be retried until negative TTL expires
}

// Inspect origin
result.is_hit();        // true if served from cache
result.was_computed();  // true if solver ran this call

// Other operations
cache.find("key");          // non-blocking lookup, no computation
cache.invalidate("key");    // lazy invalidation
cache.touch("key");         // refresh TTL, move to MRU
cache.remove("key");        // eager physical removal
cache.has("key");           // existence check
cache.size();               // current entry count
cache.capacity();           // max entries
cache.stats();              // hits, misses, negative_hits, evictions
```

## Entry States

| State | Meaning |
|-------|---------|
| `Empty` | Slot available, no active key |
| `Computing` | Miss solver running; not evictable; waiters block on this |
| `Ready` | Positive result cached and valid |
| `Failed` | Negative result cached and valid |
| `Invalidated` | Logically dead; physical removal deferred to LRU eviction |

## Concurrency Model

Follows the same proven model as [gateway_cache](https://github.com/lrleon/gateway_cache) (6 years in production):

1. **Global mutex** protects the hash table and LRU list (short critical sections)
2. **Per-entry mutex + condition_variable** implements single-flight:
   - First thread sets state to `Computing`, releases global lock, runs solver
   - Subsequent threads for the same key lock the entry, see `Computing`, wait on the CV
   - When solver completes, all waiters are notified and share the result

Different keys never block each other beyond the brief global lock for table lookup.

### Solver Contract

While the miss solver is running, it must not call any API on the **same**
`Cache` instance. Doing so can create a logical self-deadlock on an entry in
`Computing`. This implementation treats that as a fatal programming error and
aborts immediately instead of blocking indefinitely.

Calling a different `Cache` instance from the solver remains valid.

## Safety Properties

1. Never two simultaneous computations for the same key
2. Never deliver a value whose lifetime isn't guaranteed (`shared_ptr`)
3. Never evict an entry in `Computing` state
4. Never confuse a negative result with a positive one (typed `CacheResult`)
5. Never corrupt LRU under concurrency (global mutex)
6. Lazy invalidation never destroys coherence (state-checked on every access)

## Building

Requires: C++20, [Aleph-w](https://github.com/lrleon/Aleph-w), Google Test, GSL

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make cache_unit_test cache_concurrency_test

./cache_unit_test          # 20 tests
./cache_concurrency_test   # 9 tests
```

### With sanitizers

```bash
cmake .. -DSANITIZE=thread    # ThreadSanitizer
cmake .. -DSANITIZE=address   # AddressSanitizer
cmake .. -DSANITIZE=undefined # UBSan
```

## Continuous Integration & AI Reviews

GitHub Actions runs automatically on pushes and pull requests:

- `.github/workflows/ci.yml` builds both test suites with Ninja, pulling
  `Aleph-w` as a sibling checkout, and executes `ctest`.
- `.github/workflows/coderabbit.yml` invokes CodeRabbit for incremental PR
  reviews. Requires repository secrets: `OPENAI_API_KEY` (OpenAI) and the
  default `GITHUB_TOKEN`.
- `.github/workflows/codex-review.yml` runs OpenAI Codex with a concurrency/
  cache-focused prompt and posts findings back to the PR. Reuses the same
  `OPENAI_API_KEY` secret.

To add GitHub Copilot as a reviewer, create a repository ruleset under
**Settings → Code and automation → Rules → Rulesets**, enable **Automatically
request Copilot code review**, and optionally allow reviews on drafts / new
pushes. Copilot follows repository-level instructions in `.github/instructions/`
if present.

## Design Decisions

- **`shared_ptr<Value>` ownership** — separates logical validity in the cache from physical lifetime of the value object. External holders keep the value alive even after eviction/invalidation.
- **No compression in core** — the user can compress/serialize their `Value` type externally.
- **No `void*` cookie** — the miss solver is a `std::function<shared_ptr<Value>(const Key&)>`; capture any context in the lambda.
- **No `gtest` in public headers** — no `FRIEND_TEST`, no `using namespace` pollution.
- **`steady_clock` for TTL** — monotonic clock, immune to system time adjustments.
- **Graceful saturation** — if all entries are `Computing` and a new key arrives, it computes directly without caching rather than blocking indefinitely.
