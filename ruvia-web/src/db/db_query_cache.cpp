#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/timer.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/db/db_session.h"
#include "ruvia/web/redis/redis_session.h"

#include "db/db_config_storage.h"
#include "db/db_digest.h"
#include "db/db_query_cache_state.h"
#include "integration/request_io_metrics.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_protocol.h"
namespace ruvia::detail {
namespace {
void number(std::string& out, std::uint64_t value) {
    std::array<char, 8> encoded;
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
    }
    out.append(encoded.data(), encoded.size());
}
void text(std::string& out, std::string_view value) {
    number(out, value.size());
    out.append(value);
}
[[noreturn]] void invalid_cache() {
    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid database query cache entry");
}
struct reader {
    std::string_view bytes_;
    std::uint64_t number() {
        if (bytes_.size() < 8) {
            invalid_cache();
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) {
            value |= std::uint64_t(static_cast<unsigned char>(bytes_[index])) << (index * 8);
        }
        bytes_.remove_prefix(8);
        return value;
    }
    std::string_view text() {
        const auto size = number();
        if (size > bytes_.size()) {
            invalid_cache();
        }
        auto result_value = bytes_.substr(0, static_cast<std::size_t>(size));
        bytes_.remove_prefix(static_cast<std::size_t>(size));
        return result_value;
    }
};

// Leads every entry, so a value another writer put under a cache key is
// refused before it is decoded.
constexpr std::string_view db_cache_entry_magic = "RUVIAQC";
constexpr std::string_view cache_prefix = "ruvia:qc:";
constexpr std::size_t digest_text_bytes = 2 * db_sha256::digest_size;

[[nodiscard]] std::string make_id_key_from_hex(std::string_view prefix, db_cache_shape shape,
    std::string_view digest_hex) {
    const auto shape_tag = db_cache_shape_tag(shape);
    std::string result;
    result.reserve(prefix.size() + 3 + shape_tag.size() + 1 + digest_hex.size());
    result.append(prefix).append("id:").append(shape_tag).push_back(':');
    result.append(digest_hex);
    return result;
}

[[nodiscard]] std::string make_id_key(std::string_view prefix, db_cache_shape shape,
    const db_sha256::digest_type& hash) {
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    return make_id_key_from_hex(prefix, shape, std::string_view(hex.data(), hex.size()));
}

void release_cache_storage(void* storage) noexcept {
    auto* const bytes = static_cast<std::string*>(storage);
    delete bytes;
}

// SQL and parameters can be large, so their byte encoding streams into the
// digest instead of being copied into a temporary string first.
void digest_text(db_sha256& digest, std::string_view value) {
    digest.update_u64_le(value.size());
    digest.update(value);
}

}  // namespace

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return query_db_backend(backend_, std::move(sql_), std::move(params_), options);
}

// Format: the entry magic, row count, column count, the column names once, then for
// every row a row marker followed, for every field, by a presence byte and its
// text when present. All rows of one result share a single schema, so the
// names are not repeated per row. The marker gives every row, including a
// zero-column one, a serialized cost, so a decoder bounds the row count by the
// entry size before it allocates.
constexpr char db_cache_row_marker = '\2';

std::string encode_db_cache_rows(const db_rows& rows) {
    const auto names = rows.column_names();
    std::size_t total_bytes = db_cache_entry_magic.size() + 8U + 8U;
    for (const auto& name : names) {
        total_bytes += 8U + name.size();
    }
    for (const auto& row : rows) {
        if (row.size() != names.size()) {
            invalid_cache();
        }
        total_bytes += 1U + row.size();
        for (std::size_t index = 0; index < row.size(); ++index) {
            if (const auto value = row[index].value()) {
                total_bytes += 8U + value->size();
            }
        }
    }
    std::string out;
    out.reserve(total_bytes);
    out.append(db_cache_entry_magic);
    number(out, rows.size());
    number(out, names.size());
    for (const auto& name : names) {
        text(out, name);
    }
    for (const auto& row : rows) {
        out.push_back(db_cache_row_marker);
        for (std::size_t index = 0; index < row.size(); ++index) {
            const auto value = row[index].value();
            out.push_back(value ? '\1' : '\0');
            if (value) {
                text(out, *value);
            }
        }
    }
    return out;
}
db_rows decode_db_cache_rows(std::string bytes_value) {
    if (!bytes_value.starts_with(db_cache_entry_magic)) {
        invalid_cache();
    }
    reader reader_value{std::string_view(bytes_value).substr(db_cache_entry_magic.size())};
    const auto count = reader_value.number();
    const auto columns = reader_value.number();
    if (columns > reader_value.bytes_.size() / 8) {
        invalid_cache();
    }
    auto result_value = db_result_access::make_result();
    auto& names = db_result_access::column_names(result_value);
    names.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t index = 0; index < columns; ++index) {
        names.emplace_back(reader_value.text());
    }
    // Every row takes at least its marker and one presence byte per field,
    // which also bounds count * columns by the remaining entry size.
    if (count > reader_value.bytes_.size() / (columns + 1)) {
        invalid_cache();
    }
    // Only fields can borrow the encoded storage. Empty results and
    // zero-column rows need no backing allocation after decoding.
    if (count != 0 && columns != 0) {
        // Rebind to the result's resource when Redis and the database use
        // different worker pools.
        const auto consumed = bytes_value.size() - reader_value.bytes_.size();
        auto storage = std::make_unique<std::string>(std::move(bytes_value));
        reader_value.bytes_ = std::string_view(*storage).substr(consumed);
        db_result_access::own_raw_result(result_value, storage.release(), &release_cache_storage);
    }
    auto& fields_value = db_result_access::fields(result_value);
    auto& rows = db_result_access::rows(result_value);
    fields_value.reserve(static_cast<std::size_t>(count * columns));
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t row_index = 0; row_index < count; ++row_index) {
        if (reader_value.bytes_.empty() || reader_value.bytes_.front() != db_cache_row_marker) {
            invalid_cache();
        }
        reader_value.bytes_.remove_prefix(1);
        const auto row_start = fields_value.size();
        for (std::uint64_t index = 0; index < columns; ++index) {
            if (reader_value.bytes_.empty()) {
                invalid_cache();
            }
            const char present = reader_value.bytes_.front();
            reader_value.bytes_.remove_prefix(1);
            if (present == '\0') {
                fields_value.push_back(db_result_access::null_field());
            } else if (present == '\1') {
                fields_value.push_back(db_result_access::borrowed_field(reader_value.text()));
            } else {
                invalid_cache();
            }
        }
        // The field storage was reserved up front, so borrowed rows stay valid.
        rows.push_back(db_result_access::borrowed_row(
            fields_value.data() + row_start, static_cast<std::size_t>(columns), names.data(), names.size()));
    }
    if (!reader_value.bytes_.empty()) {
        invalid_cache();
    }
    return result_value;
}
std::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config) {
    const auto server_name = config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{};
    db_sha256 identity;
    digest_text(identity, name_space);
    digest_text(identity, alias);
    identity.update_u64_le(static_cast<std::uint64_t>(config.driver_));
    digest_text(identity, config.host_);
    identity.update_u64_le(config.port_);
    digest_text(identity, config.database_);
    digest_text(identity, config.username_);
    digest_text(identity, server_name);
    std::string result_value;
    result_value.reserve(cache_prefix.size() + digest_text_bytes + 1);
    result_value.append(cache_prefix);
    http_append_lower_hex(result_value, identity.finish());
    result_value.push_back(':');
    return result_value;
}
void append_db_cache_id_keys(
    std::vector<std::string>& keys, std::string_view prefix, std::string_view id) {
    const auto hash = db_sha256_of(id);
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    const std::string_view hex_view(hex.data(), hex.size());
    for (const auto shape : db_cache_shapes) {
        keys.push_back(make_id_key_from_hex(prefix, shape, hex_view));
    }
}
std::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver) {
    if (!id.empty()) {
        return make_id_key(prefix, shape, db_sha256_of(id));
    }
    std::string result_value;
    result_value.reserve(prefix.size() + 4 + digest_text_bytes);
    result_value.append(prefix);
    db_sha256 input;
    input.update_u64_le(static_cast<std::uint64_t>(driver));
    digest_text(input, sql);
    input.update_u64_le(params.size());
    for (const auto& value : params) {
        const auto type = value.type();
        const auto type_byte = static_cast<char>(type);
        input.update({&type_byte, 1});
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                digest_text(input, value.text());
                break;
            case db_value_type::signed_value:
                input.update_u64_le(static_cast<std::uint64_t>(value.signed_value()));
                break;
            case db_value_type::unsigned_value:
                input.update_u64_le(value.unsigned_value());
                break;
            case db_value_type::double_value:
                input.update_u64_le(std::bit_cast<std::uint64_t>(value.double_value()));
                break;
            case db_value_type::bool_value:
                input.update_u64_le(value.bool_value());
                break;
        }
    }
    result_value.append("sql:");
    http_append_lower_hex(result_value, input.finish());
    return result_value;
}

namespace {

[[noreturn]] void throw_db_cache_timeout() {
    throw db_error(db_error::code_type::timeout, std::nullopt, "database query cache operation timed out");
}

// The options of one Redis operation under the deadline it shares with the
// query it caches.
[[nodiscard]] operation_options db_cache_options(const operation_options& base, const operation_deadline& timeout) {
    if (timeout.expired()) {
        throw_db_cache_timeout();
    }
    return db_remaining_options(base, timeout);
}

bool is_fatal_redis_cache_error(const redis_error& error) noexcept {
    return error.code() == redis_error::code_type::cancelled ||
           error.code() == redis_error::code_type::closing;
}

void rethrow_or_timeout_redis_cache_error(
    const redis_error& error, const operation_deadline& deadline, bool ignore_errors) {
    if (is_fatal_redis_cache_error(error)) {
        throw;
    }
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    if (!ignore_errors) {
        throw;
    }
}

// Rows of a cached entry, or nullopt for a corrupt entry the cache ignores.
// The bytes are released before the caller goes on to the database.
[[nodiscard]] std::optional<db_rows> decode_cached_rows(std::string bytes_value, const operation_deadline& deadline, bool ignore_errors) {
    std::optional<db_rows> rows;
    try {
        rows.emplace(decode_db_cache_rows(std::move(bytes_value)));
    } catch (const db_conversion_error&) {
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        if (!ignore_errors) {
            throw;
        }
        return std::nullopt;
    }
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    return rows;
}

task<void> finish_delete_key_batch(std::optional<task<std::int64_t>> deletion, const operation_deadline& deadline_value) {
    if (!deletion) {
        co_return;
    }
    try {
        (void)co_await std::move(*deletion);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
    }
    if (deadline_value.expired()) {
        throw_db_cache_timeout();
    }
}

task<void> delete_key_batch(const redis_session& redis, std::span<const std::string> keys, const operation_options& options, const operation_deadline& deadline_value) {
    if (keys.empty()) {
        return finish_delete_key_batch(std::nullopt, deadline_value);
    }
    try {
        static constexpr std::size_t inline_capacity = 64;
        if (keys.size() <= inline_capacity) {
            std::array<std::string_view, inline_capacity> inline_views;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                inline_views[i] = keys[i];
            }
            // DEL owns its arguments before returning its cold operation.
            auto deletion = redis.with_options(db_cache_options(options, deadline_value))
                                .del(std::span<const std::string_view>(inline_views.data(), keys.size()));
            return finish_delete_key_batch(std::move(deletion), deadline_value);
        }
        std::vector<std::string_view> heap_views;
        heap_views.reserve(keys.size());
        for (const auto& key : keys) {
            heap_views.emplace_back(key);
        }
        auto deletion = redis.with_options(db_cache_options(options, deadline_value)).del(heap_views);
        return finish_delete_key_batch(std::move(deletion), deadline_value);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        throw;
    }
}

task<void> remove_keys(redis_session redis, std::vector<std::string> keys, request_io_metrics* io_metrics,
    operation_options options, stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    auto deletion = delete_key_batch(redis, keys, options, deadline_value);
    // The command owns the keys; release their source before network IO.
    std::vector<std::string>(keys).swap(keys);
    co_await std::move(deletion);
}

task<void> clear_keys(
    redis_session redis, std::string pattern, request_io_metrics* io_metrics, operation_options options,
    stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_cache_options(options, deadline_value))
                    .scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        }
        if (deadline_value.expired()) {
            throw_db_cache_timeout();
        }
        cursor = result->next_cursor();
        auto deletion = delete_key_batch(redis, result->values(), options, deadline_value);
        // Preserve only the cursor while the owned DEL waits for its reply.
        result.reset();
        co_await std::move(deletion);
    } while (cursor);
}

}  // namespace

db_query_cache_state::db_query_cache_state(const redis_session& redis, const db_cache_config_storage& config,
    std::string prefix)
    : worker_(redis.worker()),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      max_entry_reply_bytes_(redis_client_runtime::handle_config(redis).max_reply_bytes_),
      prefix_(std::move(prefix)),
      redis_(redis.with_options({})),
      fills_() {}

db_query_cache_state::~db_query_cache_state() = default;

// The leader's hold on the fill it listed. Every exit -- a result, a failure,
// a cancellation or a timeout -- unlists the fill and ends it, so its waiters
// never outlive the leader's attempt.
class db_query_cache_state::fill_lead final {
public:
    fill_lead(db_query_cache_state& state, fill_pointer listed) noexcept
        : state_(state),
          fill_(std::move(listed)) {}
    fill_lead(const fill_lead&) = delete;
    fill_lead& operator=(const fill_lead&) = delete;
    ~fill_lead() {
        finish();
    }

    [[nodiscard]] fill& get() const noexcept {
        return *fill_;
    }
    void finish() noexcept {
        if (!finished_) {
            finished_ = true;
            state_.unlist_fill(*fill_);
            fill_->done_.request_stop();
        }
    }

private:
    db_query_cache_state& state_;
    fill_pointer fill_;
    bool finished_{false};
};

void db_query_cache_state::close_now() noexcept {
    closed_ = true;
    stop_.request_stop();
}

void db_query_cache_state::require_open() const {
    if (closed_) {
        throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
    }
}

void db_query_cache_state::unlist_fill(const fill& ended) noexcept {
    std::erase_if(fills_, [&ended](const fill_pointer& listed) noexcept {
        return listed.get() == &ended;
    });
}

void db_query_cache_state::unlist_fills(std::span<const std::string> keys) noexcept {
    std::erase_if(fills_, [keys](const fill_pointer& listed) noexcept {
        return std::ranges::find(keys, listed->key_) != keys.end();
    });
}

void db_query_cache_state::unlist_all_fills() noexcept {
    fills_.clear();
}

std::optional<std::string> db_query_cache_state::key(
    const db_query& query, const db_statement& statement, db_driver driver) {
    if (!query.cache_enabled().value_or(always_enabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return db_cache_key(prefix_, query.cache_id(), query.cache_shape(), statement.sql(), statement.params(), driver);
}

task<db_rows> db_query_cache_state::wrap(std::optional<std::chrono::milliseconds> duration, std::string key,
    db_cache_query database, operation_options options, operation_deadline deadline) {
    require_open();
    return query(std::move(key), duration.value_or(duration_), std::move(database), std::move(options), deadline);
}

// Key, deferred database statement and options belong to this cold frame. On
// a hit the statement is destroyed without starting. Decoded rows own their
// fields independently of the Redis reply and later cache operations.
//
// A miss joins the fill already running for the same key on this worker, if
// any (Pingora read_through), and waits for it within the caller's own
// deadline and stop token: the leader's published result answers it like a
// hit, and a leader that ended without one sends it back to the cache, where
// the first to miss again leads the next fill. Each worker fills a key at most
// once at a time; other workers and processes fill it independently.
task<db_rows> db_query_cache_state::query(std::string key, std::chrono::milliseconds duration,
    db_cache_query database, operation_options options, operation_deadline deadline) {
    stop_group cancellation{options.stop_token_, stop_.token()};
    options.stop_token_ = cancellation.token();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::optional<std::string> bytes_value;
        try {
            bytes_value = co_await redis_.with_options(db_cache_options(options, deadline)).get(key);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (bytes_value.has_value()) {
            if (auto rows = decode_cached_rows(std::move(*bytes_value), deadline, ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        const auto running = std::ranges::find_if(fills_, [&key](const fill_pointer& listed) noexcept {
            return listed->key_ == key;
        });
        if (running == fills_.end()) {
            break;
        }
        const auto joined = *running;
        joined->awaited_ = true;
        timer_sleep_result woken{};
        {
            stop_group wake{options.stop_token_, joined->done_.token()};
            woken = co_await sleep_until(
                worker_, deadline.deadline().value_or(std::chrono::steady_clock::time_point::max()), wake.token());
        }
        if (options.stop_token_.stop_requested()) {
            require_open();
            throw db_error(db_error::code_type::cancelled, std::nullopt, "database query cache operation cancelled");
        }
        if (joined->result_.has_value()) {
            if (auto rows = decode_cached_rows(std::string(*joined->result_), deadline,
                    ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        // Cut short with the fill still running: the worker is stopping.
        if (woken == timer_sleep_result::stop_requested && !joined->done_.stop_requested()) {
            throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
        }
    }

    auto listed = std::make_shared<fill>(key);
    fills_.push_back(listed);
    fill_lead lead(*this, std::move(listed));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    auto rows = co_await std::move(database)(db_remaining_options(options, deadline));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    const auto elapsed =
        std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    auto& filled = lead.get();
    const bool store = elapsed < duration;
    if (!store && !filled.awaited_) {
        co_return rows;
    }
    auto encoded_bytes = encode_db_cache_rows(rows);
    // An entry whose GET reply would exceed the store's reply limit would fail
    // every hit and cost a Redis connection each time; like Pingora's
    // ResponseTooLarge, the result stays valid but is not cached.
    const bool fits = !max_entry_reply_bytes_.has_value() ||
                      resp_bulk_serialized_size(encoded_bytes.size()) <= *max_entry_reply_bytes_;
    std::optional<task<redis_set_result>> write;
    if (store && fits) {
        try {
            // SET owns its arguments before returning the cold operation.
            write.emplace(redis_.with_options(db_cache_options(options, deadline))
                    .set(key, encoded_bytes, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
    }
    // Waiters take the result from memory; neither the key nor the serialized
    // rows are retained here while the SET waits on IO.
    if (filled.awaited_) {
        filled.result_ = std::move(encoded_bytes);
    }
    std::string(encoded_bytes).swap(encoded_bytes);
    std::string(key).swap(key);
    lead.finish();
    if (write.has_value()) {
        try {
            (void)co_await std::move(*write);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
    }
    co_return rows;
}

// A cache id names every result shape a repository builder derived from it.
task<void> db_query_cache_state::remove(
    std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options) {
    require_open();
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
    }
    std::vector<std::string> keys;
    keys.reserve(ids.size() * db_cache_shapes.size());
    for (const auto id : ids) {
        append_db_cache_id_keys(keys, prefix_, id);
    }
    unlist_fills(keys);
    return remove_keys(redis_, std::move(keys), io_metrics, std::move(options), stop_.token());
}

task<void> db_query_cache_state::clear(request_io_metrics* io_metrics, operation_options options) {
    require_open();
    std::string pattern;
    pattern.reserve(prefix_.size() + 1);
    pattern.append(prefix_);
    pattern.push_back('*');
    unlist_all_fills();
    return clear_keys(redis_, std::move(pattern), io_metrics, std::move(options), stop_.token());
}
}  // namespace ruvia::detail

namespace ruvia {
db_query_result_cache::db_query_result_cache(detail::db_query_cache_state& state_value, operation_scope& scope,
    detail::request_io_metrics* io_metrics, operation_options options) noexcept
    : state_(&state_value),
      io_metrics_(io_metrics),
      options_(std::move(options)),
      registration_(scope, this, &db_query_result_cache::expire_capability) {}

db_query_result_cache::db_query_result_cache(const db_query_result_cache& other) noexcept
    : state_(other.state_),
      io_metrics_(other.io_metrics_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void db_query_result_cache::expire_capability(void* target) noexcept {
    auto& cache = *static_cast<db_query_result_cache*>(target);
    cache.state_ = nullptr;
    cache.io_metrics_ = nullptr;
    cache.options_ = {};
}
task<void> db_query_result_cache::remove(std::span<const std::string_view> ids) const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->remove(ids, io_metrics_, options_));
}
task<void> db_query_result_cache::clear() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->clear(io_metrics_, options_));
}
}  // namespace ruvia
