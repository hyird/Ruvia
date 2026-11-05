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
        constexpr std::size_t mask = index_capacity - 1;
        for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
            const auto code = entry_slots_[slot];
            if (code == 0) {
                break;
            }
            const std::size_t index = static_cast<std::size_t>(code - 1);
            auto& existing = entries_[index];
            if (existing.generation_ != 0 && existing.hash_ == hash && existing.sql_ == sql &&
                std::ranges::equal(existing.types_, types)) {
                existing.last_use_ = clock_;
                return {handle{index, existing.generation_}, false, std::nullopt, hash};
            }
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
        if (!has_free_entry && entries_.size() < capacity) {
            entries_.reserve(entries_.size() + 1);
        }
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
        const auto next_generation = generation_ + 1;
        // A forgotten entry is reused before the table grows; only a full
        // table evicts a statement the server still holds.
        if (has_free_entry) {
            const auto index = static_cast<std::size_t>(free_entry - entries_.begin());
            free_entry->sql_.assign(sql);
            free_entry->types_.assign(types.begin(), types.end());
            free_entry->hash_ = hash;
            free_entry->last_use_ = clock_;
            free_entry->generation_ = next_generation;
            free_entry->payload_ = std::move(*created_payload);
            insert_entry_slot(index, hash);
            clear_first_use(*first_use_index, hash);
            ++generation_;
            return {handle{index, next_generation}, true, std::nullopt, hash};
        }
        if (entries_.size() < capacity) {
            entry added{std::pmr::string(sql, resource),
                std::pmr::vector<unsigned int>(types.begin(), types.end(), resource), hash, clock_, next_generation,
                std::move(*created_payload)};
            const auto index = entries_.size();
            entries_.push_back(std::move(added));
            insert_entry_slot(index, hash);
            clear_first_use(*first_use_index, hash);
            ++generation_;
            return {handle{index, next_generation}, true, std::nullopt, hash};
        }
        const auto oldest = std::ranges::min_element(entries_, {}, &entry::last_use_);
        const auto oldest_index = static_cast<std::size_t>(oldest - entries_.begin());
        erase_entry_slot(oldest_index, oldest->hash_);
        oldest->sql_.assign(sql);
        oldest->types_.assign(types.begin(), types.end());
        std::optional<payload_type> evicted(std::move(oldest->payload_));
        oldest->hash_ = hash;
        oldest->last_use_ = clock_;
        oldest->generation_ = next_generation;
        oldest->payload_ = std::move(*created_payload);
        insert_entry_slot(oldest_index, hash);
        clear_first_use(*first_use_index, hash);
        ++generation_;
        return {handle{oldest_index, next_generation}, true, std::move(evicted), hash};
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
        erase_entry_slot(statement.index_, old_hash);
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
        entry_slots_.fill(0);
        first_uses_.fill(0);
        first_use_slots_.fill(0);
    }

    // The hash a key is kept under; never zero, which marks a free position.
    [[nodiscard]] static std::size_t key_hash(std::string_view sql, std::span<const unsigned int> types) noexcept {
        auto hash = std::hash<std::string_view>{}(sql) ^ types.size();
        for (const auto type : types) {
            hash = (hash ^ type) * 0x100000001b3ULL;
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

    void insert_entry_slot(std::size_t index, std::size_t hash) noexcept {
        constexpr std::size_t mask = index_capacity - 1;
        std::size_t slot = hash & mask;
        while (entry_slots_[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        entry_slots_[slot] = static_cast<std::uint8_t>(index + 1);
    }

    void erase_entry_slot(std::size_t index, std::size_t hash) noexcept {
        constexpr std::size_t mask = index_capacity - 1;
        const auto target = static_cast<std::uint8_t>(index + 1);
        for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
            const auto code = entry_slots_[slot];
            if (code == 0) {
                return;
            }
            if (code == target) {
                std::size_t empty = slot;
                for (std::size_t next = (slot + 1) & mask; entry_slots_[next] != 0; next = (next + 1) & mask) {
                    const auto next_index = static_cast<std::size_t>(entry_slots_[next] - 1);
                    const auto ideal = entries_[next_index].hash_ & mask;
                    if (((next - ideal) & mask) >= ((next - empty) & mask)) {
                        entry_slots_[empty] = entry_slots_[next];
                        empty = next;
                    }
                }
                entry_slots_[empty] = 0;
                return;
            }
        }
    }

    [[nodiscard]] std::optional<std::size_t> find_first_use(std::size_t hash) const noexcept {
        constexpr std::size_t mask = index_capacity - 1;
        for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
            const auto code = first_use_slots_[slot];
            if (code == 0) {
                return std::nullopt;
            }
            const std::size_t pos = static_cast<std::size_t>(code - 1);
            if (first_uses_[pos] == hash) {
                return pos;
            }
        }
        return std::nullopt;
    }

    void insert_first_use_slot(std::size_t pos, std::size_t hash) noexcept {
        constexpr std::size_t mask = index_capacity - 1;
        std::size_t slot = hash & mask;
        while (first_use_slots_[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        first_use_slots_[slot] = static_cast<std::uint8_t>(pos + 1);
    }

    void erase_first_use_slot(std::size_t pos, std::size_t hash) noexcept {
        constexpr std::size_t mask = index_capacity - 1;
        const auto target = static_cast<std::uint8_t>(pos + 1);
        for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
            const auto code = first_use_slots_[slot];
            if (code == 0) {
                return;
            }
            if (code == target) {
                std::size_t empty = slot;
                for (std::size_t next = (slot + 1) & mask; first_use_slots_[next] != 0; next = (next + 1) & mask) {
                    const auto next_pos = static_cast<std::size_t>(first_use_slots_[next] - 1);
                    const auto ideal = first_uses_[next_pos] & mask;
                    if (((next - ideal) & mask) >= ((next - empty) & mask)) {
                        first_use_slots_[empty] = first_use_slots_[next];
                        empty = next;
                    }
                }
                first_use_slots_[empty] = 0;
                return;
            }
        }
    }

    void clear_first_use(std::size_t pos, std::size_t hash) noexcept {
        erase_first_use_slot(pos, hash);
        first_uses_[pos] = 0;
    }

    void remember(std::size_t hash) noexcept {
        if (const auto existing = find_first_use(hash); existing.has_value()) {
            return;
        }
        const auto pos = next_first_use_;
        if (first_uses_[pos] != 0) {
            erase_first_use_slot(pos, first_uses_[pos]);
        }
        first_uses_[pos] = hash;
        insert_first_use_slot(pos, hash);
        next_first_use_ = (next_first_use_ + 1) % first_uses_.size();
    }

    std::pmr::vector<entry> entries_;
    std::array<std::uint8_t, index_capacity> entry_slots_{};
    // Key hashes of recent first uses; zero marks a free position. A hash
    // collision only prepares a statement on its first use.
    std::array<std::size_t, capacity> first_uses_{};
    std::array<std::uint8_t, index_capacity> first_use_slots_{};
    std::size_t next_first_use_{0};
    std::uint64_t clock_{0};
    std::uint64_t generation_{0};
};

}  // namespace ruvia::detail
