#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/fnv1a.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

// Names one entry of a db_statement_cache by position and generation, so
// forgetting or replacing an entry never redirects another statement's handle
// to a different statement; a stale handle finds nothing.
struct db_statement_handle final {
    std::size_t index_;
    std::uint64_t generation_;
};

// The server-side prepared statements of one connection, shared by the drivers
// that cache them. A statement is keyed by its SQL and the type codes of its
// parameters, since a server fixes parameter (and with them result) types when
// it prepares. A statement is prepared on its second use: the first runs
// unprepared, so SQL that never repeats (literals, generated lists) costs no
// more than without the cache and does not evict reused statements. Entries
// live as long as the connection; the least recently used one makes room for
// a new one, and its payload goes back to the driver to release on the server.
template <typename payload_type>
class db_statement_cache final {
public:
    static constexpr std::size_t capacity = 128;

    using handle = db_statement_handle;

    struct lookup final {
        // Absent: run the statement unprepared.
        std::optional<handle> statement_{};
        // The statement is new to the connection: prepare it first.
        bool inserted_{false};
        // The payload of an entry evicted to make room.
        std::optional<payload_type> evicted_{};
        // The statement's key_hash().
        std::size_t hash_{0};
        // Not cached, used before, and not inserted only because the caller
        // may not insert it now.
        bool repeated_{false};
    };

    explicit db_statement_cache(std::pmr::memory_resource* resource)
        : entries_(pmr_resource_or_default(resource)) {}

    // Counts a use of the statement. A cached one is returned. One used
    // before is inserted when the caller `may_insert` it now, with the payload
    // make() builds; the whole entry is built before the table changes, so a
    // failed allocation leaves every entry intact. Otherwise the use is
    // remembered and nothing returned.
    template <typename factory_type>
    [[nodiscard]] lookup find(
        std::string_view sql, std::span<const unsigned int> types, bool may_insert, factory_type&& make) {
        const auto hash = key_hash(sql, types);
        ++clock_;
        const auto cached = entry_index_.find(hash, [&](std::size_t index) noexcept {
            const auto& existing = entries_[index];
            return existing.generation_ != 0 && existing.hash_ == hash && existing.sql_ == sql &&
                   std::ranges::equal(existing.types_, types);
        });
        if (cached.has_value()) {
            auto& existing = entries_[*cached];
            existing.last_use_ = clock_;
            return {handle{*cached, existing.generation_}, false, std::nullopt, hash};
        }
        const auto first_use_index = find_first_use(hash);
        if (!first_use_index.has_value()) {
            remember(hash);
            return {std::nullopt, false, std::nullopt, hash};
        }
        if (!may_insert) {
            return {std::nullopt, false, std::nullopt, hash, true};
        }
        auto* resource = entries_.get_allocator().resource();
        const auto free_entry = std::ranges::find(entries_, std::uint64_t{0}, &entry::generation_);
        const bool has_free_entry = free_entry != entries_.end();
        if (!has_free_entry && entries_.size() < capacity && entries_.size() == entries_.capacity()) {
            entries_.reserve(std::min(capacity, std::max<std::size_t>(entries_.size() * 2, 8)));
        }
        std::pmr::string key_sql(sql, resource);
        std::pmr::vector<unsigned int> key_types(types.begin(), types.end(), resource);
        std::optional<payload_type> created_payload;
        if constexpr (std::is_invocable_v<factory_type, std::string_view, std::span<const unsigned int>,
                          std::size_t, std::pmr::memory_resource*>) {
            created_payload = make(sql, types, hash, resource);
            if (!created_payload.has_value()) {
                return {std::nullopt, false, std::nullopt, hash, true};
            }
        } else {
            created_payload.emplace(make(sql, types, resource));
        }
        // Nothing below allocates: the key was copied above, `entries_` has
        // room, and every string and vector shares one resource, so swaps and
        // moves only exchange storage.
        const auto next_generation = generation_ + 1;
        auto index = entries_.size();
        std::optional<payload_type> evicted;
        if (has_free_entry || index == capacity) {
            if (has_free_entry) {
                // A forgotten entry is reused before the table grows.
                index = static_cast<std::size_t>(free_entry - entries_.begin());
            } else {
                // Only a full table evicts a statement the server still holds.
                const auto oldest = std::ranges::min_element(entries_, {}, &entry::last_use_);
                index = static_cast<std::size_t>(oldest - entries_.begin());
                entry_index_.erase(index, oldest->hash_, [this](std::size_t position) noexcept { return entries_[position].hash_; });
                evicted.emplace(std::move(oldest->payload_));
            }
            auto& reused = entries_[index];
            reused.sql_.swap(key_sql);
            reused.types_.swap(key_types);
            reused.hash_ = hash;
            reused.last_use_ = clock_;
            reused.generation_ = next_generation;
            reused.payload_ = std::move(*created_payload);
        } else {
            entries_.push_back(
                entry{std::move(key_sql), std::move(key_types), hash, clock_, next_generation, std::move(*created_payload)});
        }
        entry_index_.insert(index, hash);
        clear_first_use(*first_use_index, hash);
        ++generation_;
        return {handle{index, next_generation}, true, std::move(evicted), hash};
    }

    [[nodiscard]] payload_type* payload(handle statement) noexcept {
        auto* found = live(statement);
        return found == nullptr ? nullptr : &found->payload_;
    }

    // The SQL of a live entry, owned by the cache. It stays in place until the
    // next find() or clear().
    [[nodiscard]] std::string_view sql(handle statement) const noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return {};
        }
        return entries_[statement.index_].sql_;
    }

    // Drops an entry the server does not hold (a failed prepare) or no longer
    // holds as cached (a stale statement), unless it was replaced since, and
    // hands its payload back. With `use_again` its next use prepares it
    // straight away, as for a statement used before.
    std::optional<payload_type> forget(handle statement, bool use_again) noexcept {
        auto* found = live(statement);
        if (found == nullptr) {
            return std::nullopt;
        }
        const auto old_hash = found->hash_;
        entry_index_.erase(statement.index_, old_hash, [this](std::size_t index) noexcept { return entries_[index].hash_; });
        found->generation_ = 0;
        if (use_again) {
            remember(old_hash);
        }
        return std::move(found->payload_);
    }

    // Calls visit() with the payload of every live entry.
    template <typename visitor_type>
    void for_each(visitor_type&& visit) {
        for (auto& existing : entries_) {
            if (existing.generation_ != 0) {
                visit(existing.payload_);
            }
        }
    }

    // A new connection starts without server-side statements.
    void clear() noexcept {
        entries_.clear();
        entry_index_.clear();
        first_uses_.fill(0);
        first_use_index_.clear();
    }

    // The hash a key is kept under; never zero, which marks a free position.
    [[nodiscard]] static std::size_t key_hash(std::string_view sql, std::span<const unsigned int> types) noexcept {
        auto hash = std::hash<std::string_view>{}(sql) ^ types.size();
        for (const auto type : types) {
            hash = (hash ^ type) * fnv1a_prime;
        }
        return hash == 0 ? 1 : hash;
    }

private:
    struct entry final {
        std::pmr::string sql_;
        std::pmr::vector<unsigned int> types_;
        std::size_t hash_;
        std::uint64_t last_use_;
        // Zero marks a forgotten entry, free for reuse. Never reused across
        // clear(), so no handle outlives its entry unnoticed.
        std::uint64_t generation_;
        payload_type payload_;
    };

    [[nodiscard]] entry* live(handle statement) noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return nullptr;
        }
        return &entries_[statement.index_];
    }

    static constexpr std::size_t index_capacity = 256;
    static_assert((index_capacity & (index_capacity - 1)) == 0 && index_capacity >= capacity * 2);

    // An open-addressing index over at most `capacity` positions, stored as
    // position + 1 so zero marks an empty slot: probed linearly from the
    // hash, deleted by backward shift. `hash_of(position)` reads the hash a
    // stored position was inserted under.
    class probe_index final {
    public:
        template <typename match_type>
        [[nodiscard]] std::optional<std::size_t> find(std::size_t hash, match_type&& match) const noexcept {
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return std::nullopt;
                }
                if (const auto position = static_cast<std::size_t>(code - 1); match(position)) {
                    return position;
                }
            }
            return std::nullopt;
        }

        void insert(std::size_t position, std::size_t hash) noexcept {
            std::size_t slot = hash & mask;
            while (slots_[slot] != 0) {
                slot = (slot + 1) & mask;
            }
            slots_[slot] = static_cast<std::uint8_t>(position + 1);
        }

        template <typename hash_of_type>
        void erase(std::size_t position, std::size_t hash, hash_of_type&& hash_of) noexcept {
            const auto target = static_cast<std::uint8_t>(position + 1);
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return;
                }
                if (code == target) {
                    std::size_t empty = slot;
                    for (std::size_t next = (slot + 1) & mask; slots_[next] != 0; next = (next + 1) & mask) {
                        const auto ideal = hash_of(static_cast<std::size_t>(slots_[next] - 1)) & mask;
                        if (((next - ideal) & mask) >= ((next - empty) & mask)) {
                            slots_[empty] = slots_[next];
                            empty = next;
                        }
                    }
                    slots_[empty] = 0;
                    return;
                }
            }
        }

        void clear() noexcept {
            slots_.fill(0);
        }

    private:
        static constexpr std::size_t mask = index_capacity - 1;
        std::array<std::uint8_t, index_capacity> slots_{};
    };

    [[nodiscard]] std::optional<std::size_t> find_first_use(std::size_t hash) const noexcept {
        return first_use_index_.find(hash, [&](std::size_t position) noexcept { return first_uses_[position] == hash; });
    }

    void clear_first_use(std::size_t position, std::size_t hash) noexcept {
        first_use_index_.erase(position, hash, [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        first_uses_[position] = 0;
    }

    void remember(std::size_t hash) noexcept {
        if (find_first_use(hash).has_value()) {
            return;
        }
        const auto position = next_first_use_;
        if (first_uses_[position] != 0) {
            first_use_index_.erase(position, first_uses_[position], [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        }
        first_uses_[position] = hash;
        first_use_index_.insert(position, hash);
        next_first_use_ = (next_first_use_ + 1) % first_uses_.size();
    }

    std::pmr::vector<entry> entries_;
    probe_index entry_index_;
    // Key hashes of recent first uses; zero marks a free position. A hash
    // collision only prepares a statement on its first use.
    std::array<std::size_t, capacity> first_uses_{};
    probe_index first_use_index_;
    std::size_t next_first_use_{0};
    std::uint64_t clock_{0};
    std::uint64_t generation_{0};
};

}  // namespace ruvia::detail
