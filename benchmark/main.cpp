#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <gbt/elastic_flat_map>
#include <gbt/funnel_flat_map>

namespace gbt_benchmark
{
	struct record_key
	{
		std::uint64_t tenant;
		std::uint64_t object;
	};

	bool operator==(const record_key& left, const record_key& right)
	{
		return left.tenant == right.tenant && left.object == right.object;
	}

	std::uint64_t Mix64(std::uint64_t value)
	{
		value += 0x9E3779B97F4A7C15ULL;
		value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
		return value ^ (value >> 31);
	}
} // namespace gbt_benchmark

namespace std
{
	template <>
	struct hash<gbt_benchmark::record_key>
	{
		std::size_t operator()(const gbt_benchmark::record_key& key) const
		{
			return static_cast<std::size_t>(gbt_benchmark::Mix64(key.tenant) ^ gbt_benchmark::Mix64(key.object + 0xD1B54A32D192ED03ULL));
		}
	};
} // namespace std

namespace
{
	volatile std::uint64_t g_sink = 0;

	using Clock = std::chrono::steady_clock;
	using gbt_benchmark::record_key;

	constexpr int kRepeats = 3;
	constexpr int kWarmups = 1;

	struct Sample
	{
		double milliseconds;
		std::uint64_t checksum;
		std::size_t memoryBytes;
	};

	struct Result
	{
		double averageNsPerOp;
		double minNsPerOp;
		double maxNsPerOp;
		std::uint64_t checksum;
		std::size_t memoryBytes;
	};

	struct BenchmarkRow
	{
		std::string_view scenario;
		std::size_t count;
		std::string_view container;
		Result result;
	};

	enum class OperationKind
	{
		Lookup,
		Insert,
		Erase
	};

	template <typename Key>
	struct Entry
	{
		Key key;
		int value;
	};

	template <typename Key>
	struct Operation
	{
		OperationKind kind;
		Key key;
		int value;
	};

	int ValueForOrdinal(std::uint64_t ordinal)
	{
		return static_cast<int>((ordinal * 2 + 1) & 0x7FFFFFFFULL);
	}

	std::uint64_t MakeUInt64Key(std::uint64_t ordinal)
	{
		return gbt_benchmark::Mix64(ordinal);
	}

	std::string MakeStringKey(std::uint64_t ordinal)
	{
		return "key:" + std::to_string(ordinal) + ':' + std::to_string(gbt_benchmark::Mix64(ordinal));
	}

	record_key MakeRecordKey(std::uint64_t ordinal)
	{
		return {gbt_benchmark::Mix64(ordinal), gbt_benchmark::Mix64(ordinal + 0xA0761D6478BD642FULL)};
	}

	template <typename Key>
	std::uint64_t MixKeyValue(const Key& key, int value)
	{
		const auto keyHash = static_cast<std::uint64_t>(std::hash<Key>{}(key));
		return gbt_benchmark::Mix64(keyHash ^ (static_cast<std::uint64_t>(value) * 0x9E3779B97F4A7C15ULL));
	}

	template <typename Key>
	std::size_t DynamicKeyBytes(const Key&)
	{
		return 0;
	}

	std::size_t DynamicKeyBytes(const std::string& key)
	{
		return key.size() > 15 ? key.capacity() + 1 : 0;
	}

	template <typename Map>
	std::size_t DynamicKeyBytesForMap(const Map& map)
	{
		std::size_t bytes = 0;
		for (const auto& [key, value] : map)
		{
			(void)value;
			bytes += DynamicKeyBytes(key);
		}
		return bytes;
	}

	template <typename Value>
	struct GbtSlotEstimate
	{
		std::aligned_storage_t<sizeof(Value), alignof(Value)> storage;
		std::uint32_t hash;
		std::uint8_t state;
	};

	double ElapsedMilliseconds(Clock::time_point start, Clock::time_point end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}

	template <typename Key, typename KeyFactory>
	std::vector<Entry<Key>> MakeSequentialEntries(std::size_t size, std::uint64_t firstOrdinal, const KeyFactory& makeKey)
	{
		std::vector<Entry<Key>> entries;
		entries.reserve(size);
		for (std::size_t i = 0; i < size; ++i)
		{
			const auto ordinal = firstOrdinal + static_cast<std::uint64_t>(i);
			entries.push_back({makeKey(ordinal), ValueForOrdinal(ordinal)});
		}
		return entries;
	}

	template <typename Key>
	std::vector<Entry<Key>> MakeShuffledEntries(std::vector<Entry<Key>> entries, std::uint32_t seed)
	{
		std::shuffle(entries.begin(), entries.end(), std::mt19937{seed});
		return entries;
	}

	template <typename Key, typename KeyFactory>
	std::vector<Key> MakeShuffledKeys(std::size_t size, std::uint64_t firstOrdinal, const KeyFactory& makeKey, std::uint32_t seed)
	{
		std::vector<Key> keys;
		keys.reserve(size);
		for (std::size_t i = 0; i < size; ++i)
		{
			keys.push_back(makeKey(firstOrdinal + static_cast<std::uint64_t>(i)));
		}
		std::shuffle(keys.begin(), keys.end(), std::mt19937{seed});
		return keys;
	}

	template <typename Key, typename KeyFactory>
	std::vector<Operation<Key>> MakeMixedOperations(std::size_t operationCount, const std::vector<Entry<Key>>& initialEntries, std::uint64_t nextInsertOrdinal,
													const KeyFactory& makeKey)
	{
		std::vector<Operation<Key>> operations;
		operations.reserve(operationCount);

		std::mt19937 rng{0xBADC0DEU + static_cast<std::uint32_t>(operationCount)};
		std::uniform_int_distribution<std::size_t> existingIndex(0, initialEntries.size() - 1);

		for (std::size_t i = 0; i < operationCount; ++i)
		{
			const auto bucket = rng() % 100;
			if (bucket < 70)
			{
				const auto& entry = initialEntries[existingIndex(rng)];
				operations.push_back({OperationKind::Lookup, entry.key, 0});
			}
			else if (bucket < 85)
			{
				operations.push_back({OperationKind::Insert, makeKey(nextInsertOrdinal), ValueForOrdinal(nextInsertOrdinal)});
				++nextInsertOrdinal;
			}
			else
			{
				const auto& entry = initialEntries[existingIndex(rng)];
				operations.push_back({OperationKind::Erase, entry.key, 0});
			}
		}

		return operations;
	}

	template <typename SampleFactory>
	Result RunRepeated(std::size_t operations, int repeats, int warmups, SampleFactory makeSample)
	{
		std::vector<double> samples;
		samples.reserve(static_cast<std::size_t>(repeats));

		std::uint64_t checksum = 0;
		std::size_t memoryBytes = 0;
		for (int i = 0; i < warmups + repeats; ++i)
		{
			const Sample sample = makeSample();
			if (i >= warmups)
			{
				const auto nsPerOp = (sample.milliseconds * 1'000'000.0) / static_cast<double>(operations);
				samples.push_back(nsPerOp);
				checksum ^= sample.checksum + 0x9E3779B97F4A7C15ULL + (checksum << 6) + (checksum >> 2);
				memoryBytes = sample.memoryBytes;
			}
		}

		g_sink = checksum;

		const auto [minIt, maxIt] = std::minmax_element(samples.begin(), samples.end());
		const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
		return {total / static_cast<double>(samples.size()), *minIt, *maxIt, checksum, memoryBytes};
	}

	template <typename Key>
	struct StdUnorderedMap
	{
		using Map = std::unordered_map<Key, int, std::hash<Key>, std::equal_to<Key>>;

		static std::string_view Name()
		{
			return "std::unordered_map";
		}

		static void Reserve(Map& map, std::size_t size)
		{
			map.reserve(size);
		}

		static void Insert(Map& map, const Key& key, int value)
		{
			map.emplace(key, value);
		}

		static int Get(const Map& map, const Key& key)
		{
			const auto it = map.find(key);
			return it == map.end() ? 0 : it->second;
		}

		static void Erase(Map& map, const Key& key)
		{
			map.erase(key);
		}

		static std::uint64_t Checksum(Map& map)
		{
			std::uint64_t checksum = 0;
			for (const auto& [key, value] : map)
			{
				checksum += MixKeyValue(key, value);
			}
			return checksum;
		}

		static std::size_t MemoryBytes(const Map& map)
		{
			const auto bucketBytes = map.bucket_count() * sizeof(void*);
			const auto nodeBytes = map.size() * (sizeof(typename Map::value_type) + 2 * sizeof(void*));
			return bucketBytes + nodeBytes + DynamicKeyBytesForMap(map);
		}
	};

	template <typename Key>
	struct AbslFlatHashMap
	{
		using Map = absl::flat_hash_map<Key, int, std::hash<Key>, std::equal_to<Key>>;

		static std::string_view Name()
		{
			return "absl::flat_hash_map";
		}

		static void Reserve(Map& map, std::size_t size)
		{
			map.reserve(size);
		}

		static void Insert(Map& map, const Key& key, int value)
		{
			map.emplace(key, value);
		}

		static int Get(const Map& map, const Key& key)
		{
			const auto it = map.find(key);
			return it == map.end() ? 0 : it->second;
		}

		static void Erase(Map& map, const Key& key)
		{
			map.erase(key);
		}

		static std::uint64_t Checksum(Map& map)
		{
			std::uint64_t checksum = 0;
			for (const auto& [key, value] : map)
			{
				checksum += MixKeyValue(key, value);
			}
			return checksum;
		}

		static std::size_t MemoryBytes(const Map& map)
		{
			const auto capacity = map.capacity();
			const auto slotBytes = capacity * sizeof(typename Map::value_type);
			const auto controlBytes = capacity + 16;
			return slotBytes + controlBytes + DynamicKeyBytesForMap(map);
		}
	};

	template <typename Key>
	struct GbtFunnelFlatMap
	{
		using Map = gbt::funnel_flat_map<Key, int, std::hash<Key>, std::equal_to<Key>>;

		static std::string_view Name()
		{
			return "gbt::funnel_flat_map";
		}

		static void Reserve(Map& map, std::size_t size)
		{
			map.reserve(size);
		}

		static void Insert(Map& map, const Key& key, int value)
		{
			map.emplace(key, value);
		}

		static int Get(const Map& map, const Key& key)
		{
			const auto it = map.find(key);
			return it == map.end() ? 0 : it->second;
		}

		static void Erase(Map& map, const Key& key)
		{
			map.erase(key);
		}

		static std::uint64_t Checksum(Map& map)
		{
			std::uint64_t checksum = 0;
			for (const auto& [key, value] : map)
			{
				checksum += MixKeyValue(key, value);
			}
			return checksum;
		}

		static std::size_t MemoryBytes(const Map& map)
		{
			return map.bucket_count() * sizeof(GbtSlotEstimate<typename Map::value_type>) + DynamicKeyBytesForMap(map);
		}
	};

	template <typename Key>
	struct GbtElasticFlatMap
	{
		using Map = gbt::elastic_flat_map<Key, int, std::hash<Key>, std::equal_to<Key>>;

		static std::string_view Name()
		{
			return "gbt::elastic_flat_map";
		}

		static void Reserve(Map& map, std::size_t size)
		{
			map.reserve(size);
		}

		static void Insert(Map& map, const Key& key, int value)
		{
			map.emplace(key, value);
		}

		static int Get(const Map& map, const Key& key)
		{
			const auto it = map.find(key);
			return it == map.end() ? 0 : it->second;
		}

		static void Erase(Map& map, const Key& key)
		{
			map.erase(key);
		}

		static std::uint64_t Checksum(Map& map)
		{
			std::uint64_t checksum = 0;
			for (const auto& [key, value] : map)
			{
				checksum += MixKeyValue(key, value);
			}
			return checksum;
		}

		static std::size_t MemoryBytes(const Map& map)
		{
			return map.bucket_count() * sizeof(GbtSlotEstimate<typename Map::value_type>) + DynamicKeyBytesForMap(map);
		}
	};

	template <typename Adapter, typename Key>
	void FillMap(typename Adapter::Map& map, const std::vector<Entry<Key>>& entries, bool reserve)
	{
		if (reserve)
		{
			Adapter::Reserve(map, entries.size());
		}
		for (const auto& entry : entries)
		{
			Adapter::Insert(map, entry.key, entry.value);
		}
	}

	template <typename Adapter, typename Key>
	Sample MeasureInsert(const std::vector<Entry<Key>>& entries, bool reserve)
	{
		typename Adapter::Map map;
		if (reserve)
		{
			Adapter::Reserve(map, entries.size());
		}

		const auto start = Clock::now();
		for (const auto& entry : entries)
		{
			Adapter::Insert(map, entry.key, entry.value);
		}
		const auto end = Clock::now();

		const auto checksum = Adapter::Checksum(map);
		return {ElapsedMilliseconds(start, end), checksum, Adapter::MemoryBytes(map)};
	}

	template <typename Adapter, typename Key>
	Sample MeasureHitLookup(const std::vector<Entry<Key>>& entries, const std::vector<Entry<Key>>& queries)
	{
		typename Adapter::Map map;
		FillMap<Adapter>(map, entries, true);

		std::uint64_t checksum = 0;
		const auto start = Clock::now();
		for (const auto& query : queries)
		{
			checksum += static_cast<std::uint64_t>(Adapter::Get(map, query.key));
		}
		const auto end = Clock::now();

		return {ElapsedMilliseconds(start, end), checksum, Adapter::MemoryBytes(map)};
	}

	template <typename Adapter, typename Key>
	Sample MeasureMissLookup(const std::vector<Entry<Key>>& entries, const std::vector<Key>& queries)
	{
		typename Adapter::Map map;
		FillMap<Adapter>(map, entries, true);

		std::uint64_t checksum = 0;
		const auto start = Clock::now();
		for (const auto& key : queries)
		{
			const int value = Adapter::Get(map, key);
			checksum += value == 0 ? 1 : static_cast<std::uint64_t>(value);
		}
		const auto end = Clock::now();

		return {ElapsedMilliseconds(start, end), checksum, Adapter::MemoryBytes(map)};
	}

	template <typename Adapter, typename Key>
	Sample MeasureIteration(const std::vector<Entry<Key>>& entries)
	{
		typename Adapter::Map map;
		FillMap<Adapter>(map, entries, true);

		const auto start = Clock::now();
		const auto checksum = Adapter::Checksum(map);
		const auto end = Clock::now();

		return {ElapsedMilliseconds(start, end), checksum, Adapter::MemoryBytes(map)};
	}

	template <typename Adapter, typename Key>
	Sample MeasureErase(const std::vector<Entry<Key>>& entries, const std::vector<Entry<Key>>& eraseOrder)
	{
		typename Adapter::Map map;
		FillMap<Adapter>(map, entries, true);

		std::uint64_t checksum = 0;
		const auto start = Clock::now();
		for (const auto& entry : eraseOrder)
		{
			Adapter::Erase(map, entry.key);
			checksum += MixKeyValue(entry.key, entry.value);
		}
		const auto end = Clock::now();

		const auto finalChecksum = checksum + Adapter::Checksum(map);
		return {ElapsedMilliseconds(start, end), finalChecksum, Adapter::MemoryBytes(map)};
	}

	template <typename Adapter, typename Key>
	Sample MeasureMixed(const std::vector<Entry<Key>>& initialEntries, const std::vector<Operation<Key>>& operations)
	{
		typename Adapter::Map map;
		Adapter::Reserve(map, initialEntries.size() + operations.size() / 5);
		for (const auto& entry : initialEntries)
		{
			Adapter::Insert(map, entry.key, entry.value);
		}

		std::uint64_t checksum = 0;
		const auto start = Clock::now();
		for (const auto& operation : operations)
		{
			switch (operation.kind)
			{
				case OperationKind::Lookup:
				{
					const int value = Adapter::Get(map, operation.key);
					checksum += value == 0 ? 1 : static_cast<std::uint64_t>(value);
					break;
				}
				case OperationKind::Insert:
					Adapter::Insert(map, operation.key, operation.value);
					break;
				case OperationKind::Erase:
					Adapter::Erase(map, operation.key);
					break;
			}
		}
		const auto end = Clock::now();

		const auto finalChecksum = checksum + Adapter::Checksum(map);
		return {ElapsedMilliseconds(start, end), finalChecksum, Adapter::MemoryBytes(map)};
	}

	void PrintTableHeader()
	{
		std::cout << std::left << std::setw(24) << "scenario" << std::right << std::setw(12) << "keys"
				  << "  " << std::left << std::setw(23) << "container" << std::right << std::setw(14) << "avg ns/op"
				  << "  " << std::setw(14) << "min ns/op"
				  << "  " << std::setw(14) << "max ns/op"
				  << "  " << std::setw(10) << "relative"
				  << "  " << std::setw(10) << "est MiB"
				  << "  " << std::setw(6) << "check" << '\n';
	}

	void PrintSeparator()
	{
		std::cout << std::string(133, '-') << '\n';
	}

	std::string FormatRelative(double nsPerOp, double bestNsPerOp)
	{
		if (nsPerOp <= bestNsPerOp * 1.005)
		{
			return "best";
		}

		std::ostringstream stream;
		stream << std::fixed << std::setprecision(2) << (nsPerOp / bestNsPerOp) << 'x';
		return stream.str();
	}

	double BytesToMib(std::size_t bytes)
	{
		return static_cast<double>(bytes) / (1024.0 * 1024.0);
	}

	void PrintResult(const BenchmarkRow& row, double bestNsPerOp, std::uint64_t expectedChecksum)
	{
		const auto check = row.result.checksum == expectedChecksum ? "ok" : "diff";
		std::cout << std::left << std::setw(24) << row.scenario << std::right << std::setw(12) << row.count << "  " << std::left << std::setw(23) << row.container << std::right
				  << std::setw(14) << std::fixed << std::setprecision(1) << row.result.averageNsPerOp << "  " << std::setw(14) << std::fixed << std::setprecision(1)
				  << row.result.minNsPerOp << "  " << std::setw(14) << std::fixed << std::setprecision(1) << row.result.maxNsPerOp << "  " << std::setw(10)
				  << FormatRelative(row.result.averageNsPerOp, bestNsPerOp) << "  " << std::setw(10) << std::fixed << std::setprecision(1) << BytesToMib(row.result.memoryBytes)
				  << "  " << std::setw(6) << check << '\n';
	}

	template <typename Adapter, typename SampleFactory>
	BenchmarkRow RunBenchmark(std::string_view scenario, std::size_t count, std::size_t operations, int repeats, int warmups, SampleFactory makeSample)
	{
		return {scenario, count, Adapter::Name(), RunRepeated(operations, repeats, warmups, makeSample)};
	}

	void PrintScenarioRows(std::initializer_list<BenchmarkRow> rows)
	{
		const auto bestIt = std::min_element(rows.begin(), rows.end(),
											 [](const BenchmarkRow& left, const BenchmarkRow& right) { return left.result.averageNsPerOp < right.result.averageNsPerOp; });
		const auto expectedChecksum = rows.begin()->result.checksum;

		for (const auto& row : rows)
		{
			PrintResult(row, bestIt->result.averageNsPerOp, expectedChecksum);
		}
		PrintSeparator();
	}

	template <typename Key, typename KeyFactory>
	void RunBenchmarksForKeyType(std::string_view keyName, std::size_t count, const KeyFactory& makeKey, int repeats, int warmups)
	{
		const auto sequentialEntries = MakeSequentialEntries<Key>(count, 1, makeKey);
		const auto randomEntries = MakeShuffledEntries(sequentialEntries, 0xC0FFEEU + static_cast<std::uint32_t>(count));
		const auto missKeys = MakeShuffledKeys<Key>(count, static_cast<std::uint64_t>(count) * 8 + 1, makeKey, 0xDEADBEEFU + static_cast<std::uint32_t>(count));
		const auto mixedInitialEntries = MakeSequentialEntries<Key>(std::max<std::size_t>(count / 2, 1), 1, makeKey);
		const auto mixedOperations = MakeMixedOperations<Key>(count, mixedInitialEntries, static_cast<std::uint64_t>(mixedInitialEntries.size()) + 1, makeKey);

		std::cout << "# key_type=" << keyName << ", keys=" << count << '\n';
		PrintTableHeader();
		PrintSeparator();

		PrintScenarioRows(
			{RunBenchmark<StdUnorderedMap<Key>>("insert_grow_random", count, count, repeats, warmups, [&] { return MeasureInsert<StdUnorderedMap<Key>>(randomEntries, false); }),
			 RunBenchmark<AbslFlatHashMap<Key>>("insert_grow_random", count, count, repeats, warmups, [&] { return MeasureInsert<AbslFlatHashMap<Key>>(randomEntries, false); }),
			 RunBenchmark<GbtFunnelFlatMap<Key>>("insert_grow_random", count, count, repeats, warmups, [&] { return MeasureInsert<GbtFunnelFlatMap<Key>>(randomEntries, false); }),
			 RunBenchmark<GbtElasticFlatMap<Key>>("insert_grow_random", count, count, repeats, warmups,
												  [&] { return MeasureInsert<GbtElasticFlatMap<Key>>(randomEntries, false); })});

		PrintScenarioRows(
			{RunBenchmark<StdUnorderedMap<Key>>("insert_reserve_rand", count, count, repeats, warmups, [&] { return MeasureInsert<StdUnorderedMap<Key>>(randomEntries, true); }),
			 RunBenchmark<AbslFlatHashMap<Key>>("insert_reserve_rand", count, count, repeats, warmups, [&] { return MeasureInsert<AbslFlatHashMap<Key>>(randomEntries, true); }),
			 RunBenchmark<GbtFunnelFlatMap<Key>>("insert_reserve_rand", count, count, repeats, warmups, [&] { return MeasureInsert<GbtFunnelFlatMap<Key>>(randomEntries, true); }),
			 RunBenchmark<GbtElasticFlatMap<Key>>("insert_reserve_rand", count, count, repeats, warmups,
												  [&] { return MeasureInsert<GbtElasticFlatMap<Key>>(randomEntries, true); })});

		PrintScenarioRows({RunBenchmark<StdUnorderedMap<Key>>("lookup_hit_random", count, count, repeats, warmups,
															  [&] { return MeasureHitLookup<StdUnorderedMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<AbslFlatHashMap<Key>>("lookup_hit_random", count, count, repeats, warmups,
															  [&] { return MeasureHitLookup<AbslFlatHashMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<GbtFunnelFlatMap<Key>>("lookup_hit_random", count, count, repeats, warmups,
															   [&] { return MeasureHitLookup<GbtFunnelFlatMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<GbtElasticFlatMap<Key>>("lookup_hit_random", count, count, repeats, warmups,
																[&] { return MeasureHitLookup<GbtElasticFlatMap<Key>>(sequentialEntries, randomEntries); })});

		PrintScenarioRows({RunBenchmark<StdUnorderedMap<Key>>("lookup_miss_random", count, count, repeats, warmups,
															  [&] { return MeasureMissLookup<StdUnorderedMap<Key>>(sequentialEntries, missKeys); }),
						   RunBenchmark<AbslFlatHashMap<Key>>("lookup_miss_random", count, count, repeats, warmups,
															  [&] { return MeasureMissLookup<AbslFlatHashMap<Key>>(sequentialEntries, missKeys); }),
						   RunBenchmark<GbtFunnelFlatMap<Key>>("lookup_miss_random", count, count, repeats, warmups,
															   [&] { return MeasureMissLookup<GbtFunnelFlatMap<Key>>(sequentialEntries, missKeys); }),
						   RunBenchmark<GbtElasticFlatMap<Key>>("lookup_miss_random", count, count, repeats, warmups,
																[&] { return MeasureMissLookup<GbtElasticFlatMap<Key>>(sequentialEntries, missKeys); })});

		PrintScenarioRows(
			{RunBenchmark<StdUnorderedMap<Key>>("iterate_full", count, count, repeats, warmups, [&] { return MeasureIteration<StdUnorderedMap<Key>>(sequentialEntries); }),
			 RunBenchmark<AbslFlatHashMap<Key>>("iterate_full", count, count, repeats, warmups, [&] { return MeasureIteration<AbslFlatHashMap<Key>>(sequentialEntries); }),
			 RunBenchmark<GbtFunnelFlatMap<Key>>("iterate_full", count, count, repeats, warmups, [&] { return MeasureIteration<GbtFunnelFlatMap<Key>>(sequentialEntries); }),
			 RunBenchmark<GbtElasticFlatMap<Key>>("iterate_full", count, count, repeats, warmups, [&] { return MeasureIteration<GbtElasticFlatMap<Key>>(sequentialEntries); })});

		PrintScenarioRows({RunBenchmark<StdUnorderedMap<Key>>("erase_random", count, count, repeats, warmups,
															  [&] { return MeasureErase<StdUnorderedMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<AbslFlatHashMap<Key>>("erase_random", count, count, repeats, warmups,
															  [&] { return MeasureErase<AbslFlatHashMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<GbtFunnelFlatMap<Key>>("erase_random", count, count, repeats, warmups,
															   [&] { return MeasureErase<GbtFunnelFlatMap<Key>>(sequentialEntries, randomEntries); }),
						   RunBenchmark<GbtElasticFlatMap<Key>>("erase_random", count, count, repeats, warmups,
																[&] { return MeasureErase<GbtElasticFlatMap<Key>>(sequentialEntries, randomEntries); })});

		PrintScenarioRows({RunBenchmark<StdUnorderedMap<Key>>("mixed_70_15_15", count, mixedOperations.size(), repeats, warmups,
															  [&] { return MeasureMixed<StdUnorderedMap<Key>>(mixedInitialEntries, mixedOperations); }),
						   RunBenchmark<AbslFlatHashMap<Key>>("mixed_70_15_15", count, mixedOperations.size(), repeats, warmups,
															  [&] { return MeasureMixed<AbslFlatHashMap<Key>>(mixedInitialEntries, mixedOperations); }),
						   RunBenchmark<GbtFunnelFlatMap<Key>>("mixed_70_15_15", count, mixedOperations.size(), repeats, warmups,
															   [&] { return MeasureMixed<GbtFunnelFlatMap<Key>>(mixedInitialEntries, mixedOperations); }),
						   RunBenchmark<GbtElasticFlatMap<Key>>("mixed_70_15_15", count, mixedOperations.size(), repeats, warmups,
																[&] { return MeasureMixed<GbtElasticFlatMap<Key>>(mixedInitialEntries, mixedOperations); })});

		std::cout << '\n';
	}

} // namespace

int main()
{
	std::cout << "# gbt flat map benchmark\n";
	std::cout << "# fixed suite: key_types=uint64_t/std::string/record_key, repeats=" << kRepeats << ", warmups=" << kWarmups << '\n';
	std::cout << "# key cases: 1M uint64_t keys, 500k medium string keys, 750k 16-byte record keys.\n";
	std::cout << "# insert_reserve_rand calls reserve() before insertion for all containers.\n";
	std::cout << "# lower ns/op is better; relative is normalized to the best container in each scenario.\n";
	std::cout << "# est MiB estimates final container-owned memory after each scenario; allocator overhead is not included.\n";
	std::cout << "# check=ok means all containers produced the same observable result.\n\n";

	RunBenchmarksForKeyType<std::uint64_t>("uint64_t", 1'000'000, MakeUInt64Key, kRepeats, kWarmups);
	RunBenchmarksForKeyType<std::string>("std::string", 500'000, MakeStringKey, kRepeats, kWarmups);
	RunBenchmarksForKeyType<record_key>("record_key", 750'000, MakeRecordKey, kRepeats, kWarmups);

	g_sink = g_sink + 1;
	return 0;
}
