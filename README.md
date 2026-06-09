## gbt::flat_map

`gbt::flat_map` is a header-only C++ flat hash map library. Its first implementations, `gbt::funnel_flat_map` and `gbt::elastic_flat_map`, use open addressing and are inspired by the paper "[Optimal Bounds for Open Addressing Without Reordering](https://arxiv.org/pdf/2501.02305)" by Martin Farach-Colton, Andrew Krapivin, and William Kuszmaul (2025).

The implementation is experimental and still very crude, use in production only at your own risk.

## Requirements

- CMake 3.15+
- A C++17 compiler
- Conan 2 only for the optional benchmark dependencies

## Install

```sh
cmake -B build
cmake --build build
cmake --install build
```

By default, CMake installs to its standard prefix for the platform, such as `/usr/local` on Unix-like systems. Choose a custom prefix using `CMAKE_INSTALL_PREFIX` only when you intentionally want a local install.

Use it from another CMake project:

```cmake
find_package(gbt_flat_map CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE gbt::flat_map)
```

Use it from C++:

```cpp
#include <gbt/funnel_flat_map>
#include <gbt/elastic_flat_map>

gbt::funnel_flat_map<int, int> table;
table.emplace(1, 2);
int value = table.at(1);

gbt::elastic_flat_map<int, int> elastic_table;
elastic_table.emplace(3, 4);
```

## Benchmark

The benchmark compares `gbt::funnel_flat_map`, `gbt::elastic_flat_map`, `std::unordered_map`, and `absl::flat_hash_map`. It depends on Abseil through Conan.
The benchmark target lives in `benchmark/` and is disabled by default.

```sh
conan install . --output-folder=build --profile=release_profile --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DGBT_FLAT_MAP_BUILD_BENCHMARK=ON
cmake --build build
./build/benchmark/gbt_flat_map_benchmark
```

The benchmark uses a fixed deterministic suite grouped by key type: 1M `uint64_t` keys, 500k medium `std::string` keys, and 750k 16-byte record keys. Each key type runs the same scenarios: random insertion with and without `reserve()`, random hit lookup, random miss lookup, full iteration, random erase, and a mixed 70% lookup / 15% insert / 15% erase workload. The table also estimates the final container-owned memory after each scenario.

Run the benchmark executable after building it:

```sh
./build/benchmark/gbt_flat_map_benchmark
```

The benchmark prints `avg ns/op`, best-relative speed, estimated memory, and a result check for every scenario. Lower `ns/op` is better.

Example summary from one local run:

| Key type | Scenario | Best gbt result | Baseline context |
| --- | --- | ---: | --- |
| `uint64_t` | reserved random insert | `gbt::funnel_flat_map`: 50.6 ns/op | faster than `std::unordered_map` at 87.6 ns/op; slower than Abseil at 36.3 ns/op |
| `uint64_t` | random erase | `gbt::funnel_flat_map`: 31.1 ns/op | close to Abseil at 28.6 ns/op; much faster than `std::unordered_map` at 235.9 ns/op |
| `uint64_t` | random hit lookup | `gbt::elastic_flat_map`: 24.6 ns/op | slightly faster than `gbt::funnel_flat_map` at 25.3 ns/op; slower than Abseil at 12.7 ns/op |
| `std::string` | random hit lookup | `gbt::funnel_flat_map`: 137.5 ns/op | close to Abseil at 134.1 ns/op and faster than `std::unordered_map` at 145.0 ns/op |
| `std::string` | random miss lookup | `gbt::elastic_flat_map`: 95.0 ns/op | roughly tied with `gbt::funnel_flat_map` at 95.3 ns/op; slower than Abseil at 51.9 ns/op |
| `std::string` | random erase | `gbt::funnel_flat_map`: 351.8 ns/op | slightly faster than Abseil at 360.3 ns/op |
| 16-byte record | mixed 70/15/15 | `gbt::funnel_flat_map`: 41.9 ns/op | faster than `std::unordered_map` at 59.1 ns/op; slower than Abseil at 22.3 ns/op |

Current takeaway: the gbt maps are already competitive with `std::unordered_map` in many workloads and can be close to Abseil on erase and string hit lookup. `gbt::elastic_flat_map` is not consistently faster yet: the current implementation is a practical ranked-probe approximation, not the full non-greedy elastic placement described in the paper. It preserves simple lookup semantics, but that also limits the main advantage elastic hashing is supposed to have at higher load factors. The largest remaining gaps are negative lookups, insertion throughput, and memory footprint, especially compared with Abseil's Swiss-table layout.

## Planned improvements

The current implementations are experimental. The main areas for improvement are:

### Shared layout

- Separate control metadata from key/value storage.
- Store compact state bytes and hash fingerprints for faster negative lookups.
- Add Swiss-style grouped control-byte probing.
- Reduce per-slot memory overhead and improve cache locality.
- Tune default load factors for integer, string, and record-like keys.
- Improve tombstone cleanup and reuse after erase-heavy workloads.
- Add focused correctness tests for insertion, overwrite, lookup, erase, rehash, iteration, and mutable-key storage.

### `gbt::funnel_flat_map`

- Tune funnel geometry: level widths, bucket sizes, hash-bit shifts, and fallback probing.
- Evaluate small buckets per funnel level instead of the current single-slot level probe.
- Reduce fallback-path usage in common insert and lookup workloads.
- Explore precomputed probe metadata for each capacity.
- Keep the implementation aligned with the paper's greedy funnel-hashing model where practical.

### `gbt::elastic_flat_map`

- Replace the current ranked-probe approximation with a more faithful elastic insertion policy.
- Add metadata or probe bounds needed for safe non-greedy placement.
- Tune insertion ranks separately from lookup ranks.
- Add probe-count instrumentation for average, tail, and maximum probe lengths.
- Compare elastic behavior against funnel behavior at higher load factors and mixed workloads.

### Project

- Document non-`std::unordered_map` semantics, especially mutable keys through iterators.
- Add CI for formatting, CMake configure/build, tests, and benchmark build.

## Contributing

If you have any questions, suggestions, or just have something to say, you can write to my Telegram channel: https://t.me/partypooper_cpp. Also, feel free to make pool requests if you want to help with development.

## Links

- https://github.com/ascv0228/elastic-funnel-hashing
- https://github.com/sternma/optopenhash
- https://github.com/royvanrijn/optimalopen
