#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace gbt
{

	template <typename Key, typename T, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>>
	class elastic_flat_map
	{
	public:
		using key_type = Key;
		using mapped_type = T;
		using value_type = std::pair<Key, T>;
		using size_type = std::size_t;
		using difference_type = std::ptrdiff_t;
		using hasher = Hash;
		using key_equal = KeyEqual;
		using reference = value_type&;
		using const_reference = const value_type&;
		using pointer = value_type*;
		using const_pointer = const value_type*;

		class iterator;
		class const_iterator;

		static constexpr float default_max_load_factor = 0.5f;
		static constexpr size_type default_initial_capacity = 16;
		static constexpr size_type maximum_capacity = size_type{1} << 30;

	private:
		enum class slot_state : std::uint8_t
		{
			empty,
			occupied,
			erased
		};

		struct slot
		{
			using storage_type = std::aligned_storage_t<sizeof(value_type), alignof(value_type)>;

			storage_type storage;
			std::uint32_t hash = 0;
			slot_state state = slot_state::empty;

			slot() = default;

			slot(const slot& other) : hash(other.hash), state(other.state)
			{
				if (other.is_occupied())
				{
					construct(other.value());
				}
			}

			slot(slot&& other) : hash(other.hash), state(other.state)
			{
				if (other.is_occupied())
				{
					construct(std::move(other.value()));
				}
			}

			slot& operator=(const slot& other)
			{
				if (this != &other)
				{
					reset_empty();
					hash = other.hash;
					state = other.state;
					if (other.is_occupied())
					{
						construct(other.value());
					}
				}
				return *this;
			}

			slot& operator=(slot&& other)
			{
				if (this != &other)
				{
					reset_empty();
					hash = other.hash;
					state = other.state;
					if (other.is_occupied())
					{
						construct(std::move(other.value()));
					}
				}
				return *this;
			}

			~slot()
			{
				destroy_value();
			}

			bool is_empty() const
			{
				return state == slot_state::empty;
			}

			bool is_occupied() const
			{
				return state == slot_state::occupied;
			}

			bool is_erased() const
			{
				return state == slot_state::erased;
			}

			value_type& value()
			{
				return *std::launder(reinterpret_cast<value_type*>(&storage));
			}

			const value_type& value() const
			{
				return *std::launder(reinterpret_cast<const value_type*>(&storage));
			}

			template <typename... Args>
			void construct(Args&&... args)
			{
				::new (static_cast<void*>(std::addressof(storage))) value_type(std::forward<Args>(args)...);
			}

			void destroy_value()
			{
				if (is_occupied())
				{
					value().~value_type();
				}
			}

			void reset_empty()
			{
				destroy_value();
				hash = 0;
				state = slot_state::empty;
			}

			void reset_erased()
			{
				destroy_value();
				hash = 0;
				state = slot_state::erased;
			}
		};

		struct slot_search
		{
			size_type index;
			bool found;
		};

		static constexpr size_type npos = static_cast<size_type>(-1);
		static constexpr size_type elastic_attempt_count = 2;
		static_assert(default_max_load_factor > 0.0f, "gbt::elastic_flat_map max load factor must be positive");
		static_assert(elastic_attempt_count > 0, "gbt::elastic_flat_map attempt count must be positive");

		std::vector<slot> slots_;
		size_type size_ = 0;
		size_type erased_ = 0;
		float max_load_factor_ = default_max_load_factor;
		hasher hash_;
		key_equal equal_;

		static size_type next_power_of_two(size_type value)
		{
			size_type capacity = 1;
			while (capacity < value)
			{
				capacity <<= 1;
			}
			return capacity;
		}

		static size_type normalize_capacity(size_type requested)
		{
			const size_type minimum = std::max(requested, default_initial_capacity);
			return std::min(next_power_of_two(minimum), maximum_capacity);
		}

		static size_type mix_hash(size_type hash)
		{
			return hash ^ (hash >> (sizeof(size_type) * 4));
		}

		static size_type avalanche_hash(size_type hash)
		{
			if constexpr (sizeof(size_type) >= 8)
			{
				hash ^= hash >> 30;
				hash *= static_cast<size_type>(0xbf58476d1ce4e5b9ULL);
				hash ^= hash >> 27;
				hash *= static_cast<size_type>(0x94d049bb133111ebULL);
				hash ^= hash >> 31;
			}
			else
			{
				hash ^= hash >> 16;
				hash *= static_cast<size_type>(0x7feb352dU);
				hash ^= hash >> 15;
				hash *= static_cast<size_type>(0x846ca68bU);
				hash ^= hash >> 16;
			}
			return hash;
		}

		size_type hash_key(const key_type& key) const
		{
			return mix_hash(static_cast<size_type>(hash_(key)));
		}

		static std::uint32_t stored_hash(size_type hash)
		{
			if constexpr (sizeof(size_type) >= 8)
			{
				return static_cast<std::uint32_t>(hash ^ (hash >> 32));
			}
			else
			{
				return static_cast<std::uint32_t>(hash);
			}
		}

		size_type max_load_size(size_type capacity) const
		{
			return static_cast<size_type>(static_cast<double>(capacity) * max_load_factor_);
		}

		bool needs_growth_for_insert() const
		{
			return size_ + 1 > max_load_size(slots_.size()) || erased_ > slots_.size() / 4;
		}

		template <typename Callback>
		bool for_each_probe(size_type hash, Callback&& callback) const
		{
			const size_type capacity = slots_.size();
			const size_type mask = capacity - 1;

			// First try the cheapest candidate in every geometric level, then try
			// additional candidates. This keeps the common lookup path short while
			// preserving an elastic, rank-like probe order for harder insertions.
			size_type level_width = capacity >> 1;
			size_type offset = 0;
			size_type hash_shift = 0;

			while (level_width > 0)
			{
				const size_type index = offset + ((hash >> hash_shift) & (level_width - 1));
				if (!callback(index))
				{
					return false;
				}

				offset += level_width;
				level_width >>= 1;
				hash_shift += 7;
			}

			if constexpr (elastic_attempt_count > 1)
			{
				for (size_type attempt = 1; attempt < elastic_attempt_count; ++attempt)
				{
					level_width = capacity >> 1;
					offset = 0;
					hash_shift = 0;

					while (level_width > 0)
					{
						if (attempt < level_width)
						{
							const size_type level_mask = level_width - 1;
							const size_type start = (hash >> hash_shift) & level_mask;
							const size_type stride_hash = avalanche_hash(hash + hash_shift + attempt * 0x9E3779B97F4A7C15ULL);
							const size_type stride = (stride_hash | 1) & level_mask;
							const size_type step = stride == 0 ? 1 : stride;
							const size_type index = offset + ((start + attempt * step) & level_mask);

							if (!callback(index))
							{
								return false;
							}
						}

						offset += level_width;
						level_width >>= 1;
						hash_shift += 7;
					}
				}
			}

			const size_type fallback_hash = avalanche_hash(hash);
			const size_type start = fallback_hash & mask;
			const size_type stride = (fallback_hash | 1) & mask;
			const size_type step = stride == 0 ? 1 : stride;
			for (size_type i = 0; i < capacity; ++i)
			{
				if (!callback((start + i * step) & mask))
				{
					return false;
				}
			}

			return true;
		}

		size_type find_existing_slot(const key_type& key, size_type hash) const
		{
			size_type found_index = npos;
			const std::uint32_t hash_fingerprint = stored_hash(hash);
			for_each_probe(hash,
						   [&](size_type index)
						   {
							   const slot& current = slots_[index];
							   if (current.is_empty())
							   {
								   return false;
							   }
							   if (current.is_occupied() && current.hash == hash_fingerprint && equal_(current.value().first, key))
							   {
								   found_index = index;
								   return false;
							   }
							   return true;
						   });
			return found_index;
		}

		slot_search find_slot_for_insert(const key_type& key, size_type hash)
		{
			size_type target_index = npos;
			size_type first_erased = npos;
			const std::uint32_t hash_fingerprint = stored_hash(hash);
			bool found = false;

			for_each_probe(hash,
						   [&](size_type index)
						   {
							   slot& current = slots_[index];
							   if (current.is_occupied())
							   {
								   if (current.hash == hash_fingerprint && equal_(current.value().first, key))
								   {
									   target_index = index;
									   found = true;
									   return false;
								   }
								   return true;
							   }
							   if (current.is_erased())
							   {
								   if (first_erased == npos)
								   {
									   first_erased = index;
								   }
								   return true;
							   }

							   target_index = first_erased == npos ? index : first_erased;
							   return false;
						   });

			if (target_index == npos && first_erased != npos)
			{
				target_index = first_erased;
			}

			return {target_index, found};
		}

		template <typename KeyArg, typename... Args>
		iterator emplace_at(size_type index, size_type hash, KeyArg&& key, Args&&... args)
		{
			slot& current = slots_[index];
			const bool reused_erased_slot = current.is_erased();

			current.construct(std::piecewise_construct, std::forward_as_tuple(std::forward<KeyArg>(key)), std::forward_as_tuple(std::forward<Args>(args)...));
			current.hash = stored_hash(hash);
			current.state = slot_state::occupied;
			++size_;
			if (reused_erased_slot)
			{
				--erased_;
			}

			return iterator(this, index);
		}

		iterator emplace_value_at(size_type index, size_type hash, value_type&& value)
		{
			slot& current = slots_[index];
			const bool reused_erased_slot = current.is_erased();

			current.construct(std::move(value));
			current.hash = stored_hash(hash);
			current.state = slot_state::occupied;
			++size_;
			if (reused_erased_slot)
			{
				--erased_;
			}

			return iterator(this, index);
		}

		void erase_slot(size_type index)
		{
			slot& current = slots_[index];
			current.reset_erased();
			--size_;
			++erased_;
		}

		void insert_rehashed(value_type&& value)
		{
			const size_type hash = hash_key(value.first);
			slot_search search = find_slot_for_insert(value.first, hash);
			emplace_value_at(search.index, hash, std::move(value));
		}

	public:
		class iterator
		{
			friend class elastic_flat_map;
			struct unchecked_tag
			{
			};

		public:
			using iterator_category = std::forward_iterator_tag;
			using difference_type = typename elastic_flat_map::difference_type;
			using value_type = typename elastic_flat_map::value_type;
			using pointer = typename elastic_flat_map::pointer;
			using reference = typename elastic_flat_map::reference;

			iterator() = default;

			reference operator*() const
			{
				return map_->slots_[index_].value();
			}

			pointer operator->() const
			{
				return std::addressof(**this);
			}

			iterator& operator++()
			{
				++index_;
				advance_to_occupied();
				return *this;
			}

			iterator operator++(int)
			{
				iterator copy = *this;
				++(*this);
				return copy;
			}

			bool operator==(const iterator& other) const
			{
				return map_ == other.map_ && index_ == other.index_;
			}

			bool operator!=(const iterator& other) const
			{
				return !(*this == other);
			}

		private:
			iterator(elastic_flat_map* map, size_type index) : map_(map), index_(index)
			{
				advance_to_occupied();
			}

			iterator(elastic_flat_map* map, size_type index, unchecked_tag) : map_(map), index_(index)
			{
			}

			void advance_to_occupied()
			{
				while (map_ != nullptr && index_ < map_->slots_.size() && !map_->slots_[index_].is_occupied())
				{
					++index_;
				}
			}

			elastic_flat_map* map_ = nullptr;
			size_type index_ = 0;
		};

		class const_iterator
		{
			friend class elastic_flat_map;
			struct unchecked_tag
			{
			};

		public:
			using iterator_category = std::forward_iterator_tag;
			using difference_type = typename elastic_flat_map::difference_type;
			using value_type = typename elastic_flat_map::value_type;
			using pointer = typename elastic_flat_map::const_pointer;
			using reference = typename elastic_flat_map::const_reference;

			const_iterator() = default;

			const_iterator(iterator it) : map_(it.map_), index_(it.index_)
			{
			}

			reference operator*() const
			{
				return map_->slots_[index_].value();
			}

			pointer operator->() const
			{
				return std::addressof(**this);
			}

			const_iterator& operator++()
			{
				++index_;
				advance_to_occupied();
				return *this;
			}

			const_iterator operator++(int)
			{
				const_iterator copy = *this;
				++(*this);
				return copy;
			}

			bool operator==(const const_iterator& other) const
			{
				return map_ == other.map_ && index_ == other.index_;
			}

			bool operator!=(const const_iterator& other) const
			{
				return !(*this == other);
			}

		private:
			const_iterator(const elastic_flat_map* map, size_type index) : map_(map), index_(index)
			{
				advance_to_occupied();
			}

			const_iterator(const elastic_flat_map* map, size_type index, unchecked_tag) : map_(map), index_(index)
			{
			}

			void advance_to_occupied()
			{
				while (map_ != nullptr && index_ < map_->slots_.size() && !map_->slots_[index_].is_occupied())
				{
					++index_;
				}
			}

			const elastic_flat_map* map_ = nullptr;
			size_type index_ = 0;
		};

		elastic_flat_map() : slots_(default_initial_capacity)
		{
		}

		explicit elastic_flat_map(size_type bucket_count, const hasher& hash = hasher{}, const key_equal& equal = key_equal{})
			: slots_(normalize_capacity(bucket_count)), hash_(hash), equal_(equal)
		{
		}

		template <typename InputIt>
		elastic_flat_map(InputIt first, InputIt last) : elastic_flat_map()
		{
			insert(first, last);
		}

		elastic_flat_map(std::initializer_list<value_type> values) : elastic_flat_map()
		{
			reserve(values.size());
			insert(values.begin(), values.end());
		}

		elastic_flat_map(const elastic_flat_map&) = default;
		elastic_flat_map(elastic_flat_map&&) noexcept = default;
		elastic_flat_map& operator=(const elastic_flat_map&) = default;
		elastic_flat_map& operator=(elastic_flat_map&&) noexcept = default;
		~elastic_flat_map() = default;

		iterator begin()
		{
			return iterator(this, 0);
		}

		const_iterator begin() const
		{
			return const_iterator(this, 0);
		}

		const_iterator cbegin() const
		{
			return begin();
		}

		iterator end()
		{
			return iterator(this, slots_.size());
		}

		const_iterator end() const
		{
			return const_iterator(this, slots_.size());
		}

		const_iterator cend() const
		{
			return end();
		}

		bool empty() const
		{
			return size_ == 0;
		}

		size_type size() const
		{
			return size_;
		}

		size_type bucket_count() const
		{
			return slots_.size();
		}

		float load_factor() const
		{
			return slots_.empty() ? 0.0f : static_cast<float>(size_) / static_cast<float>(slots_.size());
		}

		float max_load_factor() const
		{
			return max_load_factor_;
		}

		void max_load_factor(float factor)
		{
			if (factor <= 0.0f)
			{
				throw std::invalid_argument("gbt::elastic_flat_map max_load_factor must be positive");
			}
			max_load_factor_ = factor;
			if (size_ > max_load_size(slots_.size()))
			{
				rehash(slots_.size());
			}
		}

		hasher hash_function() const
		{
			return hash_;
		}

		key_equal key_eq() const
		{
			return equal_;
		}

		void clear()
		{
			for (slot& current : slots_)
			{
				current.reset_empty();
			}
			size_ = 0;
			erased_ = 0;
		}

		void reserve(size_type count)
		{
			const auto required = static_cast<size_type>(static_cast<double>(count) / static_cast<double>(max_load_factor_) + 1.0);
			rehash(required);
		}

		void rehash(size_type count)
		{
			const size_type minimum_for_size = static_cast<size_type>(static_cast<double>(size_) / static_cast<double>(max_load_factor_) + 1.0);
			const size_type new_capacity = normalize_capacity(std::max(count, minimum_for_size));
			if (new_capacity == slots_.size() && erased_ == 0)
			{
				return;
			}

			std::vector<slot> old_slots = std::move(slots_);
			slots_.clear();
			slots_.resize(new_capacity);
			size_ = 0;
			erased_ = 0;

			for (slot& current : old_slots)
			{
				if (current.is_occupied())
				{
					insert_rehashed(std::move(current.value()));
				}
			}
		}

		iterator find(const key_type& key)
		{
			const size_type hash = hash_key(key);
			const size_type index = find_existing_slot(key, hash);
			return index == npos ? end() : iterator(this, index, typename iterator::unchecked_tag{});
		}

		const_iterator find(const key_type& key) const
		{
			const size_type hash = hash_key(key);
			const size_type index = find_existing_slot(key, hash);
			return index == npos ? end() : const_iterator(this, index, typename const_iterator::unchecked_tag{});
		}

		bool contains(const key_type& key) const
		{
			return find(key) != end();
		}

		mapped_type& at(const key_type& key)
		{
			const auto it = find(key);
			if (it == end())
			{
				throw std::out_of_range("gbt::elastic_flat_map::at");
			}
			return it->second;
		}

		const mapped_type& at(const key_type& key) const
		{
			const auto it = find(key);
			if (it == end())
			{
				throw std::out_of_range("gbt::elastic_flat_map::at");
			}
			return it->second;
		}

		template <typename... Args>
		std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args)
		{
			const size_type hash = hash_key(key);
			slot_search search = find_slot_for_insert(key, hash);
			if (search.found)
			{
				return {iterator(this, search.index, typename iterator::unchecked_tag{}), false};
			}

			if (needs_growth_for_insert() || search.index == npos)
			{
				rehash(slots_.size() * 2);
				search = find_slot_for_insert(key, hash);
			}

			return {emplace_at(search.index, hash, key, std::forward<Args>(args)...), true};
		}

		template <typename... Args>
		std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args)
		{
			const size_type hash = hash_key(key);
			slot_search search = find_slot_for_insert(key, hash);
			if (search.found)
			{
				return {iterator(this, search.index, typename iterator::unchecked_tag{}), false};
			}

			if (needs_growth_for_insert() || search.index == npos)
			{
				rehash(slots_.size() * 2);
				search = find_slot_for_insert(key, hash);
			}

			return {emplace_at(search.index, hash, std::move(key), std::forward<Args>(args)...), true};
		}

		std::pair<iterator, bool> insert(const value_type& value)
		{
			return try_emplace(value.first, value.second);
		}

		std::pair<iterator, bool> insert(value_type&& value)
		{
			const size_type hash = hash_key(value.first);
			slot_search search = find_slot_for_insert(value.first, hash);
			if (search.found)
			{
				return {iterator(this, search.index, typename iterator::unchecked_tag{}), false};
			}

			if (needs_growth_for_insert() || search.index == npos)
			{
				rehash(slots_.size() * 2);
				search = find_slot_for_insert(value.first, hash);
			}

			return {emplace_value_at(search.index, hash, std::move(value)), true};
		}

		template <typename... Args>
		std::pair<iterator, bool> emplace(Args&&... args)
		{
			value_type value(std::forward<Args>(args)...);
			return insert(std::move(value));
		}

		template <typename InputIt>
		void insert(InputIt first, InputIt last)
		{
			for (; first != last; ++first)
			{
				insert(*first);
			}
		}

		template <typename M>
		std::pair<iterator, bool> insert_or_assign(const key_type& key, M&& value)
		{
			auto [it, inserted] = try_emplace(key, std::forward<M>(value));
			if (!inserted)
			{
				it->second = std::forward<M>(value);
			}
			return {it, inserted};
		}

		template <typename M>
		std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& value)
		{
			auto [it, inserted] = try_emplace(std::move(key), std::forward<M>(value));
			if (!inserted)
			{
				it->second = std::forward<M>(value);
			}
			return {it, inserted};
		}

		mapped_type& operator[](const key_type& key)
		{
			return try_emplace(key).first->second;
		}

		mapped_type& operator[](key_type&& key)
		{
			return try_emplace(std::move(key)).first->second;
		}

		size_type erase(const key_type& key)
		{
			const size_type hash = hash_key(key);
			const size_type index = find_existing_slot(key, hash);
			if (index == npos)
			{
				return 0;
			}

			erase_slot(index);
			return 1;
		}

		iterator erase(iterator pos)
		{
			const size_type next_index = pos.index_ + 1;
			erase_slot(pos.index_);
			return iterator(this, next_index);
		}

		void swap(elastic_flat_map& other) noexcept(std::is_nothrow_swappable_v<hasher> && std::is_nothrow_swappable_v<key_equal>)
		{
			using std::swap;
			swap(slots_, other.slots_);
			swap(size_, other.size_);
			swap(erased_, other.erased_);
			swap(max_load_factor_, other.max_load_factor_);
			swap(hash_, other.hash_);
			swap(equal_, other.equal_);
		}
	};

	template <typename Key, typename T, typename Hash, typename KeyEqual>
	void swap(elastic_flat_map<Key, T, Hash, KeyEqual>& lhs, elastic_flat_map<Key, T, Hash, KeyEqual>& rhs) noexcept(noexcept(lhs.swap(rhs)))
	{
		lhs.swap(rhs);
	}

} // namespace gbt
