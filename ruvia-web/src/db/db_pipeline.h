#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

// Pipelining of buffered statements outside transactions, shared by the
// drivers whose protocol lets one connection carry several statements at
// once. A statement joins a pipeline group: one leased connection that
// several statements share, each sending its own flight so that a failed
// statement affects no other. Results are read in send order by one member at
// a time, the reader, which hands each finished statement's outcome to its
// member and passes the role on when its own statement completes. Members wait
// in their own coroutine frames; the group's bookkeeping is owned by the
// connection's slot and the pool, because a member that is cancelled leaves
// before its results arrive.
//
// A driver supplies an io_type with its protocol: what a flight and a group
// keep (flight_type, group_state), the statement its caller prepared
// (statement_type), and the hooks that open a group, queue a flight, read
// results without waiting, wait for and exchange I/O, and judge whether a
// connection can be reused once its group dissolves. Everything else --
// admission in arrival order, room, the reader role, deadlines, cancellation
// and connection failure -- is this one implementation.
//
// A statement that changes session state (SET, USE, BEGIN, LOCK, ...) changes
// it for every statement sharing the connection after it; such statements
// belong in a transaction. A group dissolving with its connection left inside
// a transaction block closes that connection.

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
template <typename io_type>
class db_pipeline_state;
template <typename io_type>
struct db_pipeline_record;

// Statements one connection keeps in flight at once. Locally, 16 reached about
// six times the throughput of one statement per round trip on PostgreSQL, 8
// somewhat less and 32 no more; a deeper pipeline only adds queueing latency.
inline constexpr std::size_t db_pipeline_depth = 16;

enum class db_pipeline_phase : std::uint8_t {
    // Holds nothing.
    admitting,
    // Waits in the pool's arrival order for room in a group or a connection.
    parked,
    // Holds a lease and founds a group on it.
    leased,
    // A member holding room for a flight it has not sent yet.
    reserved,
    // A member whose flight is in the group.
    pending,
    // A member whose statement has an outcome, or was abandoned.
    settled,
};

// What woke a waiting member.
enum class db_pipeline_event : std::uint8_t {
    none,
    // Admission: a connection was leased for it, or room in a group reserved.
    leased,
    joined,
    // It reads the group's results now.
    reader,
    // Its statement finished; failure_ tells how.
    completed,
    // Its statement must be sent once more (a stale cached statement).
    retry,
    // Its group's connection failed with failure_.
    failed,
    timed_out,
    cancelled,
    closing,
};

// How the head flight of a group ended, as the driver read it.
struct db_pipeline_outcome final {
    std::exception_ptr failure_{};
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    // The statement runs once more instead of completing.
    bool retry_{false};
};

// One pipelined statement. It lives in its caller's coroutine frame; the
// group refers to it only while it is a member, through records that forget
// it when it leaves. Construction binds its stop token and makes it findable
// for cancellation; destruction gives back whatever it still holds -- a parked
// position, a lease, room or a flight in a group -- so every exit path of the
// statement shares one cleanup.
template <typename io_type>
class db_pipeline_member final {
public:
    using state_type = db_pipeline_state<io_type>;

    db_pipeline_member(
        state_type& state, db_rows* rows, const stop_token& stop_token_value);
    ~db_pipeline_member();

    db_pipeline_member(const db_pipeline_member&) = delete;
    db_pipeline_member& operator=(const db_pipeline_member&) = delete;

    // Suspends until an event arrives, unless one already has.
    struct wait final {
        db_pipeline_member& member_;

        [[nodiscard]] bool await_ready() const noexcept {
            return member_.event_ != db_pipeline_event::none;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            member_.continuation_ = handle;
        }
        [[nodiscard]] db_pipeline_event await_resume() noexcept {
            return member_.take_event();
        }
    };

    [[nodiscard]] db_pipeline_event take_event() noexcept {
        return std::exchange(event_, db_pipeline_event::none);
    }
    // Its stop token was triggered, even if the cancellation has not been
    // dispatched yet.
    [[nodiscard]] bool stopped() const noexcept {
        return cancel_requested_ || stop_token_.stop_requested();
    }

    state_type& state_;
    // The caller's result for query(); null for execute().
    db_rows* rows_;
    // The caller's options outlive the statement's frame.
    const stop_token& stop_token_;
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    std::exception_ptr failure_{};
    // The statement deadline, from its first send.
    std::optional<std::chrono::steady_clock::time_point> deadline_{};
    std::coroutine_handle<> continuation_{};
    // The record of its flight while pending.
    db_pipeline_record<io_type>* record_{nullptr};
    // Its place in the pool's arrival order, shared with the callers waiting
    // for a lease.
    std::uint64_t arrival_{0};
    std::size_t slot_{0};
    db_pipeline_phase phase_{db_pipeline_phase::admitting};
    db_pipeline_event event_{db_pipeline_event::none};
    // Set when its cancellation is dispatched, which also unlinks it from the
    // statements a cancellation searches.
    bool cancel_requested_{false};
    bool connecting_{false};
    bool may_retry_{true};
    // The pool's FIFO of parked statements.
    db_pipeline_member* previous_parked_{nullptr};
    db_pipeline_member* next_parked_{nullptr};
    // The pool's cancellable statements, newest first.
    db_pipeline_member* previous_{nullptr};
    db_pipeline_member* next_{nullptr};
    worker_timer_registration admission_timer_;
    std::uint64_t cancellation_id_{0};
    worker_cancellation_registration<worker_cancellation_target<state_type>> cancellation_;
};

// One flight in a group, in send order.
template <typename io_type>
struct db_pipeline_record final {
    // Null once the member abandoned the statement; its results are discarded.
    db_pipeline_member<io_type>* member_{nullptr};
    typename io_type::flight_type flight_{};
    bool returns_rows_{false};
};

// The pipeline group of one connection; it exists while it has members. The
// records form a ring of the flights in send order; the head is the one whose
// results are read next.
template <typename io_type>
struct db_pipeline_group final {
    [[nodiscard]] db_pipeline_record<io_type>& record(std::size_t offset) noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    [[nodiscard]] const db_pipeline_record<io_type>& record(std::size_t offset) const noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    // Its oldest flight was abandoned: a statement sent now would wait behind
    // one whose caller already gave up on it, so the group takes nobody new
    // until that flight's results have been read.
    [[nodiscard]] bool stalled() const noexcept {
        return pending_ != 0 && records_[head_].member_ == nullptr;
    }

    std::array<db_pipeline_record<io_type>, db_pipeline_depth> records_{};
    // The driver's protocol state of the connection's flights.
    typename io_type::group_state io_{};
    // Why the connection failed, once closing_.
    std::exception_ptr failure_{};
    // When the reader's current socket wait ends at the latest.
    std::optional<std::chrono::steady_clock::time_point> wait_deadline_{};
    // Start of the connection's current stretch without progress -- no socket
    // readiness and no flight completed -- while flights are outstanding. The
    // I/O timeout counts from it whichever member holds the reader role.
    std::optional<std::chrono::steady_clock::time_point> stretch_start_{};
    db_pipeline_member<io_type>* reader_{nullptr};
    std::size_t head_{0};
    std::size_t pending_{0};
    // Room promised to members that have not sent their flight yet.
    std::size_t reserved_{0};
    std::size_t members_{0};
    // The connection failed; the group takes no members and dissolves.
    bool closing_{false};
};

// Per pool: admission into groups, in arrival order with the callers waiting
// for a lease. A statement founds a group on a free connection, else joins
// the group with the most room, else parks in a FIFO served by freed room and
// by released connections. A statement never joins a group ahead of an older
// caller still waiting for a lease: the groups drain to it, and a released
// connection goes to it. A parked statement older than every caller waiting
// for a lease takes a released connection itself, before the lease scheduler
// can hand it on.
template <typename io_type>
class db_pipeline_state final {
public:
    using driver_type = typename io_type::driver_type;
    using pool_type = db_pool<driver_type>;
    using member_type = db_pipeline_member<io_type>;
    using group_type = db_pipeline_group<io_type>;
    using record_type = db_pipeline_record<io_type>;
    using statement_type = typename io_type::statement_type;
    using cancellation_target_type = worker_cancellation_target<db_pipeline_state>;

    db_pipeline_state(pool_type& pool, const worker_ref& worker_value);
    ~db_pipeline_state();

    db_pipeline_state(const db_pipeline_state&) = delete;
    db_pipeline_state& operator=(const db_pipeline_state&) = delete;

    // db_pool hooks.
    [[nodiscard]] bool take_released_slot(std::size_t slot) noexcept;
    void exclusive_waiter_left() noexcept;
    // Fails every parked statement with closing.
    void close() noexcept;
    // worker_cancellation_target hook.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target() const noexcept {
        return cancellation_target_;
    }

    // The statement's way through a group, in order. admit() places it, or
    // parks it to be woken with the event admitted() checks. A statement that
    // leased a connection connects it if needed and founds a group on it.
    [[nodiscard]] db_pipeline_event admit(member_type& member, const operation_deadline& admission_deadline);
    void admitted(db_pipeline_event event) const;
    [[nodiscard]] bool must_connect(member_type& member);
    void connected(member_type& member, const std::exception_ptr& failure);
    void found_group(member_type& member);
    // Queues the member's flight in its reserved room; it becomes the reader
    // when the group has none.
    void send(member_type& member, statement_type& statement);
    [[nodiscard]] bool reads(const member_type& member) const noexcept;
    // Turns the event that ended the member's wait into its outcome: false
    // once it completed, true when it must be sent once more; throws its
    // failure.
    [[nodiscard]] bool retries(member_type& member, db_pipeline_event event) const;

    // Services for the reader. read_buffered() reads the results available
    // without waiting and returns true once the reader's own statement
    // finished, after handing the role on.
    [[nodiscard]] bool read_buffered(member_type& reader);
    // Abandons the pending followers whose deadline passed.
    void expire_followers(member_type& reader, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_deadline(std::size_t slot) const noexcept;
    // Fails every pending member and closes the connection.
    void fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    // Services for the driver's hooks.
    [[nodiscard]] pool_type& pool() const noexcept {
        return pool_;
    }
    // Statements this pool runs now, in every phase from admission on.
    [[nodiscard]] std::size_t statements() const noexcept {
        return statements_;
    }
    [[nodiscard]] group_type& group(std::size_t slot) const noexcept;
    // Hands the head flight's outcome to its member, which the reader settles
    // itself and wakes any other; an abandoned flight's outcome is discarded.
    void complete_head(std::size_t slot, member_type& reader, db_pipeline_outcome outcome);
    [[nodiscard]] db_error cancelled() const;

private:
    friend class db_pipeline_member<io_type>;

    // Whether a statement of this arrival may take a connection or room: no
    // older caller still waits for a lease.
    [[nodiscard]] bool may_share(std::uint64_t arrival) const noexcept;
    // Places a statement whose turn it is: a free connection, else room in the
    // group with the most of it that is not stalled.
    [[nodiscard]] db_pipeline_event place(member_type& member);
    [[nodiscard]] std::optional<std::size_t> roomiest_group() const noexcept;
    void join(member_type& member, std::size_t slot) noexcept;
    void park(member_type& member, const operation_deadline& admission_deadline);
    void unpark(member_type& member) noexcept;
    // Serves parked statements in arrival order.
    void dispatch() noexcept;
    // Records what woke a member and posts its resumption.
    void settle(member_type& member, db_pipeline_event event) noexcept;
    void require_usable(std::size_t slot) const;
    void hand_off(member_type& reader) noexcept;
    void abandon(member_type& member) noexcept;
    void clear_flights(group_type& cleared) noexcept;
    void unlink(member_type& member) noexcept;
    void retire(member_type& member) noexcept;
    void leave(member_type& member) noexcept;
    void dissolve(std::size_t slot) noexcept;

    pool_type& pool_;
    const worker_ref& worker_;
    member_type* members_{nullptr};
    std::size_t statements_{0};
    member_type* parked_head_{nullptr};
    member_type* parked_tail_{nullptr};
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Names one entry of a db_statement_cache by position and generation, so
// forgetting or replacing an entry never redirects another statement's handle
// to a different statement; a stale handle finds nothing.
struct db_statement_entry final {
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

    using entry_ref = db_statement_entry;

    struct lookup final {
        // Absent: run the statement unprepared.
        std::optional<entry_ref> statement_{};
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

    db_statement_cache()
        : entries_() {}

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
            return {entry_ref{*cached, existing.generation_}, false, std::nullopt, hash};
        }
        const auto first_use_index = find_first_use(hash);
        if (!first_use_index.has_value()) {
            remember(hash);
            return {std::nullopt, false, std::nullopt, hash};
        }
        if (!may_insert) {
            return {std::nullopt, false, std::nullopt, hash, true};
        }
        const auto free_entry = std::ranges::find(entries_, std::uint64_t{0}, &entry::generation_);
        const bool has_free_entry = free_entry != entries_.end();
        if (!has_free_entry && entries_.size() < capacity && entries_.size() == entries_.capacity()) {
            entries_.reserve(std::min(capacity, std::max<std::size_t>(entries_.size() * 2, 8)));
        }
        std::string key_sql(sql);
        std::vector<unsigned int> key_types(types.begin(), types.end());
        std::optional<payload_type> created_payload;
        if constexpr (std::is_invocable_v<factory_type, std::string_view, std::span<const unsigned int>,
                          std::size_t>) {
            created_payload = make(sql, types, hash);
            if (!created_payload.has_value()) {
                return {std::nullopt, false, std::nullopt, hash, true};
            }
        } else {
            created_payload.emplace(make(sql, types));
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
        return {entry_ref{index, next_generation}, true, std::move(evicted), hash};
    }

    [[nodiscard]] payload_type* payload(entry_ref statement) noexcept {
        auto* found = live(statement);
        return found == nullptr ? nullptr : &found->payload_;
    }

    // The SQL of a live entry, owned by the cache. It stays in place until the
    // next find() or clear().
    [[nodiscard]] std::string_view sql(entry_ref statement) const noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return {};
        }
        return entries_[statement.index_].sql_;
    }

    // Drops an entry the server does not hold (a failed prepare) or no longer
    // holds as cached (a stale statement), unless it was replaced since, and
    // hands its payload back. With `use_again` its next use prepares it
    // straight away, as for a statement used before.
    std::optional<payload_type> forget(entry_ref statement, bool use_again) noexcept {
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
        std::string sql_;
        std::vector<unsigned int> types_;
        std::size_t hash_;
        std::uint64_t last_use_;
        // Zero marks a forgotten entry, free for reuse. Never reused across
        // clear(), so no handle outlives its entry unnoticed.
        std::uint64_t generation_;
        payload_type payload_;
    };

    [[nodiscard]] entry* live(entry_ref statement) noexcept {
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

    std::vector<entry> entries_;
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
