#include "db/mariadb_statements.h"

#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/db/db_result_access.h"

#include "db/db_sql.h"
#include "db/mariadb_text.h"

namespace ruvia::detail {
namespace {

// Column buffers above this many bytes are given back once their rows have
// been read, so a connection does not keep the size of its widest result.
constexpr std::size_t mariadb_column_bytes_kept = 64 * 1024;

// Storage a bound parameter points at for the length of an execution.
union mariadb_param_value {
    long long integer_;
    double real_;
    signed char flag_;
};

// Storage of one result column between mysql_stmt_fetch() and formatting.
struct mariadb_column final {
    std::pmr::vector<char> buffer_;
    unsigned long length_{0};
    my_bool null_{0};
    my_bool error_{0};
};

// One cached statement: its handle once prepared, the placeholders its text
// restricts (mariadb_statement_traits::integer_params_), and the hash of its
// key, for refusing it.
struct mariadb_statement final {
    MYSQL_STMT* handle_{nullptr};
    std::uint64_t integer_params_{0};
    std::size_t key_hash_{0};
};

[[nodiscard]] db_error statement_error(MYSQL_STMT& statement, std::string_view operation,
    db_error::code_type code = db_error::code_type::statement_failed) {
    return mariadb_error(operation, mysql_stmt_errno(&statement), mysql_stmt_sqlstate(&statement),
        mysql_stmt_error(&statement), code);
}

// A failure that leaves the connection out of step, as io_error: the
// connection's own report when it has one, else the statement's.
[[noreturn]] void throw_connection_failure(MYSQL& connection, MYSQL_STMT& statement, std::string_view operation) {
    if (mysql_errno(&connection) != 0) {
        throw mysql_error(connection, operation, db_error::code_type::io_error);
    }
    throw statement_error(statement, operation, db_error::code_type::io_error);
}

[[nodiscard]] bool connection_failure(MYSQL_STMT& statement) noexcept {
    return mariadb_connection_failure(mysql_stmt_errno(&statement));
}

// The type a parameter binds as, when the server reads it as it reads the
// parameter's literal (see mariadb_statement_cache).
[[nodiscard]] std::optional<unsigned int> bound_type(const db_value& param) noexcept {
    switch (db_value_access::type(param)) {
        case db_value_type::null:
            return MYSQL_TYPE_NULL;
        case db_value_type::string:
            if (db_value_access::text(param).find('\0') != std::string_view::npos) {
                return std::nullopt;
            }
            return MYSQL_TYPE_STRING;
        case db_value_type::signed_value:
            if (db_value_access::signed_value(param) < 0) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::unsigned_value:
            if (db_value_access::unsigned_value(param) >
                static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::double_value: {
            const auto value = db_value_access::get_double_value(param);
            if (!std::isfinite(value) || std::signbit(value)) {
                return std::nullopt;
            }
            return MYSQL_TYPE_DOUBLE;
        }
        case db_value_type::bool_value:
            return MYSQL_TYPE_TINY;
    }
    return std::nullopt;
}

// Whether every column's binary value tells the text the text protocol sends
// for it. A YEAR without ZEROFILL does not: the text protocol pads a table
// column's value to four digits and an expression's not, under the same
// metadata. Metadata from the prepare is not final either: a UNION of YEAR
// columns is described with ZEROFILL when prepared and without it when
// executed. So a statement whose prepared metadata holds any YEAR never runs
// prepared, and an executed one is readable unless a YEAR lacks ZEROFILL.
[[nodiscard]] bool readable_as_text(const MYSQL_FIELD* fields, std::size_t count, bool executed) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (fields[index].type == MYSQL_TYPE_YEAR && (!executed || (fields[index].flags & ZEROFILL_FLAG) == 0)) {
            return false;
        }
    }
    return true;
}

// The name and value of a session variable as libmariadb hands them over.
[[nodiscard]] std::string_view session_text(const MARIADB_CONST_STRING* text) noexcept {
    return text == nullptr || text->str == nullptr ? std::string_view{} : std::string_view(text->str, text->length);
}

// Takes the next item off a comma-separated list, without the spaces around
// it.
[[nodiscard]] std::string_view next_item(std::string_view& list) noexcept {
    const auto comma = list.find(',');
    auto item = list.substr(0, comma);
    list.remove_prefix(comma == std::string_view::npos ? list.size() : comma + 1);
    while (!item.empty() && item.front() == ' ') {
        item.remove_prefix(1);
    }
    while (!item.empty() && item.back() == ' ') {
        item.remove_suffix(1);
    }
    return item;
}

// Whether the comma-separated `list` names `name`, as the server compares
// variable names: regardless of case.
[[nodiscard]] bool lists(std::string_view list, std::string_view name) noexcept {
    const auto same = [](char left, char right) {
        const auto lower = [](char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        return lower(left) == lower(right);
    };
    while (!list.empty()) {
        if (std::ranges::equal(next_item(list), name, same)) {
            return true;
        }
    }
    return false;
}

// Whether a value of session_track_system_variables tracks every variable
// the cache relies on.
[[nodiscard]] bool tracks_all(std::string_view value) noexcept {
    if (lists(value, "*")) {
        return true;
    }
    for (auto wanted = mariadb_tracked_variables; !wanted.empty();) {
        if (!lists(value, next_item(wanted))) {
            return false;
        }
    }
    return true;
}

// Reads the tracked list back after a stored procedure may have changed it.
constexpr std::string_view tracking_query = "SELECT @@session_track_system_variables";

}  // namespace

struct mariadb_statement_cache::state final {
    explicit state(std::pmr::memory_resource* resource)
        : statements_(resource),
          retired_(resource),
          types_(resource),
          params_(resource),
          values_(resource),
          results_(resource),
          columns_(resource) {}

    ~state() {
        free_all();
    }

    state(const state&) = delete;
    state& operator=(const state&) = delete;

    [[nodiscard]] mariadb_statement& entry(handle statement) {
        auto* found = statements_.payload(statement);
        if (found == nullptr) {
            throw db_error(
                db_error::code_type::protocol_error, db_driver::mariadb, "MariaDB statement left the cache");
        }
        return *found;
    }

    // Room to retire `count` more handles, made before anything changes, so
    // that retiring them cannot fail.
    void reserve_retired(std::size_t count = 1) {
        retired_.reserve(retired_.size() + count);
    }

    // Moves a handle to those waiting to be closed on the server.
    void retire(MYSQL_STMT* statement) noexcept {
        if (statement != nullptr) {
            retired_.push_back(statement);
        }
    }

    // Remembers a statement that runs as text on this connection, without
    // it taking a place among the prepared ones. A hash collision only runs
    // another statement as text.
    void refuse(std::size_t hash) noexcept {
        refused_[next_refused_] = hash;
        next_refused_ = (next_refused_ + 1) % refused_.size();
    }

    // Drops an entry for good, refusing its statement.
    void refuse(handle statement, std::size_t hash) noexcept {
        (void)statements_.forget(statement, false);
        refuse(hash);
    }

    [[nodiscard]] bool refused(std::size_t hash) const noexcept {
        return std::ranges::find(refused_, hash) != refused_.end();
    }

    // Every cached statement was parsed under a session state that is gone:
    // each is closed on the server and prepared anew when used again.
    void invalidate() {
        std::size_t handles = 0;
        statements_.for_each([&handles](mariadb_statement& statement) {
            handles += statement.handle_ != nullptr ? 1 : 0;
        });
        reserve_retired(handles);
        statements_.for_each([this](mariadb_statement& statement) { retire(std::exchange(statement.handle_, nullptr)); });
        statements_.clear();
        refused_.fill(0);
        changed_ = false;
    }

    [[nodiscard]] bool usable() const noexcept {
        return tracked_ && all_variables_ && schema_tracked_ && !untracked_ && client_utf8mb4_ && connection_utf8mb4_;
    }

    // The session may no longer report its changes: nothing runs prepared
    // until the connection is replaced.
    void lose_tracking() noexcept {
        untracked_ = true;
        changed_ = true;
    }

    // libmariadb's status callback (MARIADB_OPT_STATUS_CALLBACK): each change
    // of the session the server reports in an OK packet, while libmariadb
    // reads it. It only records; the cache acts on the next find(), which no
    // prepared execution is in progress for. Only the schema and the
    // variables a statement depends on count: the server also reports others
    // it tracks regardless of the list, such as last_gtid after every commit
    // when binary logging is on. libmariadb declares the second parameter as
    // its C enum; C makes that enum compatible with its underlying integer
    // type, which is what this declares, since va_start() after a parameter
    // of enum type is undefined in C++.
    using status_type = std::underlying_type_t<enum_mariadb_status_info>;
    static void hear(void* data, status_type type, ...) {
        if (type != static_cast<status_type>(SESSION_TRACK_TYPE)) {
            return;
        }
        auto& cache = *static_cast<state*>(data);
        std::va_list arguments;
        va_start(arguments, type);
        const auto track = static_cast<enum_session_state_type>(va_arg(arguments, int));
        if (track == SESSION_TRACK_SCHEMA) {
            cache.changed_ = true;
        } else if (track == SESSION_TRACK_SYSTEM_VARIABLES) {
            const auto name = session_text(va_arg(arguments, MARIADB_CONST_STRING*));
            const auto value = session_text(va_arg(arguments, MARIADB_CONST_STRING*));
            if (name == "session_track_system_variables") {
                cache.all_variables_ = tracks_all(value);
            } else if (name == "session_track_schema") {
                cache.schema_tracked_ = value == "ON";
            } else if (name == "character_set_client") {
                cache.client_utf8mb4_ = value == "utf8mb4";
            } else if (name == "character_set_connection") {
                cache.connection_utf8mb4_ = value == "utf8mb4";
            }
            if (name == "collation_connection" || lists(mariadb_tracked_variables, name)) {
                cache.changed_ = true;
            }
        }
        va_end(arguments);
    }

    // Runs only once the connection is closed (connect() after close_slot(),
    // or the slot's end after the pool closed it). A handle whose close was
    // suspended has already left the connection's list, so mysql_close() did
    // not detach it: detaching each one here keeps mysql_stmt_close() from
    // reaching the freed connection.
    void free_all() noexcept {
        const auto release = [](MYSQL_STMT* statement) {
            statement->mysql = nullptr;
            (void)mysql_stmt_close(statement);
        };
        statements_.for_each([&release](mariadb_statement& statement) {
            if (statement.handle_ != nullptr) {
                release(std::exchange(statement.handle_, nullptr));
            }
        });
        for (auto* retired : retired_) {
            release(retired);
        }
        retired_.clear();
        statements_.clear();
        refused_.fill(0);
        // A new connection opens with the utf8mb4 it asks for, and is
        // untracked until it says otherwise.
        tracked_ = false;
        all_variables_ = false;
        schema_tracked_ = false;
        untracked_ = false;
        client_utf8mb4_ = true;
        connection_utf8mb4_ = true;
        changed_ = false;
        verify_ = false;
    }

    db_statement_cache<mariadb_statement> statements_;
    std::pmr::vector<MYSQL_STMT*> retired_;
    // Key hashes of statements that run as text on this connection; zero
    // marks a free position.
    std::array<std::size_t, db_statement_cache<mariadb_statement>::capacity> refused_{};
    std::size_t next_refused_{0};
    // What the session reported: whether it tracks every variable the cache
    // relies on and the schema, and whether its client and connection
    // character sets are still utf8mb4; and a change since the last find().
    bool tracked_{false};
    bool all_variables_{false};
    bool schema_tracked_{false};
    bool untracked_{false};
    bool client_utf8mb4_{true};
    bool connection_utf8mb4_{true};
    bool changed_{false};
    // A stored procedure ran since the tracked list was last read back.
    bool verify_{false};
    // Scratch of the one execution a connection runs at a time.
    std::pmr::vector<unsigned int> types_;
    std::pmr::vector<MYSQL_BIND> params_;
    std::pmr::vector<mariadb_param_value> values_;
    std::pmr::vector<MYSQL_BIND> results_;
    std::pmr::vector<mariadb_column> columns_;
    // The bytes the column buffers hold, and those the last result needed.
    std::size_t column_bytes_{0};
    std::size_t column_needed_{0};
};

mariadb_statement_cache::mariadb_statement_cache(std::pmr::memory_resource* resource) noexcept
    : resource_(pmr_resource_or_default(resource)) {}

mariadb_statement_cache::~mariadb_statement_cache() {
    if (state_ != nullptr) {
        std::pmr::polymorphic_allocator<state>(resource_).delete_object(state_);
    }
}

mariadb_statement_cache::mariadb_statement_cache(mariadb_statement_cache&& other) noexcept
    : resource_(other.resource_),
      state_(std::exchange(other.state_, nullptr)) {}

mariadb_statement_cache& mariadb_statement_cache::operator=(mariadb_statement_cache&& other) noexcept {
    std::swap(resource_, other.resource_);
    std::swap(state_, other.state_);
    return *this;
}

mariadb_statement_cache::state& mariadb_statement_cache::data() {
    if (state_ == nullptr) {
        state_ = std::pmr::polymorphic_allocator<state>(resource_).new_object<state>(resource_);
    }
    return *state_;
}

void mariadb_statement_cache::clear() noexcept {
    if (state_ != nullptr) {
        state_->free_all();
    }
}

bool mariadb_statement_cache::observe(MYSQL& connection) {
    // libmariadb reads both arguments as void*.
    return mysql_optionsv(&connection, MARIADB_OPT_STATUS_CALLBACK, reinterpret_cast<void*>(&state::hear),
               static_cast<void*>(&data())) == 0;
}

void mariadb_statement_cache::connected(bool tracked) noexcept {
    auto* cache = state_;
    if (cache == nullptr) {
        return;
    }
    // The statement that turned tracking of the variables the cache relies
    // on and of the schema on succeeded; the server's report of it says no
    // more.
    cache->tracked_ = tracked;
    cache->all_variables_ = tracked;
    cache->schema_tracked_ = tracked;
    cache->changed_ = true;
}

void mariadb_statement_cache::untrack() noexcept {
    if (state_ != nullptr) {
        state_->lose_tracking();
    }
}

void mariadb_statement_cache::routine_called() noexcept {
    if (state_ != nullptr) {
        state_->verify_ = true;
    }
}

bool mariadb_statement_cache::client_utf8mb4() const noexcept {
    return state_ != nullptr && state_->tracked_ && state_->all_variables_ && !state_->untracked_ &&
           !state_->verify_ && state_->client_utf8mb4_;
}

bool mariadb_names_session_tracking(std::string_view sql) noexcept {
    constexpr std::string_view name = "session_track";
    for (auto underscore = sql.find('_'); underscore != std::string_view::npos;
        underscore = sql.find('_', underscore + 1)) {
        if (underscore < 7 || sql.size() - (underscore - 7) < name.size()) {
            continue;
        }
        const auto candidate = sql.substr(underscore - 7, name.size());
        if (std::ranges::equal(candidate, name, [](char left, char right) {
                return (left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left) == right;
            })) {
            return true;
        }
    }
    return false;
}

std::optional<mariadb_statement_cache::handle> mariadb_statement_cache::find(
    std::string_view sql, std::span<const db_value> params, bool may_prepare) {
    auto& cache = data();
    if (cache.changed_) {
        cache.invalidate();
    }
    if (!cache.usable()) {
        return std::nullopt;
    }
    auto& types = cache.types_;
    types.clear();
    for (const auto& param : params) {
        const auto type = bound_type(param);
        if (!type.has_value()) {
            return std::nullopt;
        }
        types.push_back(*type);
    }
    // Looked up without being inserted first: a statement that may not run
    // prepared is refused before it could take the place of one that may.
    mariadb_statement_traits traits{};
    std::size_t hash = 0;
    const auto make = [&traits, &hash](
                          std::string_view, std::span<const unsigned int>, std::pmr::memory_resource*) noexcept {
        return mariadb_statement{nullptr, traits.integer_params_, hash};
    };
    auto found = cache.statements_.find(sql, types, false, make);
    if (!may_prepare) {
        return std::nullopt;
    }
    if (!found.statement_.has_value()) {
        if (!found.repeated_ || cache.refused(found.hash_)) {
            return std::nullopt;
        }
        hash = found.hash_;
        traits = mariadb_statement_traits_of(sql);
        if (!traits.preparable_) {
            cache.refuse(hash);
            return std::nullopt;
        }
        cache.reserve_retired();
        found = cache.statements_.find(sql, types, true, make);
        if (found.evicted_.has_value()) {
            cache.retire(found.evicted_->handle_);
        }
        if (!found.statement_.has_value()) {
            return std::nullopt;
        }
    }
    const auto& statement = *cache.statements_.payload(*found.statement_);
    for (std::size_t index = 0; index < types.size() && index < 64; ++index) {
        if (((statement.integer_params_ >> index) & 1U) != 0 &&
            types[index] != static_cast<unsigned int>(MYSQL_TYPE_LONGLONG)) {
            return std::nullopt;
        }
    }
    return found.statement_;
}

std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows) {
    if (failure != nullptr) {
        return failure;
    }
    if (extra_row_set) {
        return std::make_exception_ptr(std::invalid_argument("SQL must not return more than one result set"));
    }
    if (output_failure != nullptr) {
        return output_failure;
    }
    if (returns_rows && !returned_rows) {
        return std::make_exception_ptr(std::invalid_argument("query() requires row-producing SQL"));
    }
    if (!returns_rows && returned_rows) {
        return std::make_exception_ptr(std::invalid_argument("execute() does not accept row-producing SQL"));
    }
    return nullptr;
}

mariadb_prepared_run start_mariadb_prepared(
    mariadb_statement_cache& cache, mariadb_statement_cache::handle statement, std::span<const db_value> params) noexcept {
    mariadb_prepared_run run;
    run.cache_ = &cache;
    run.handle_ = statement;
    run.params_ = params;
    run.step_ = mariadb_prepared_run::step::close;
    return run;
}

void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept {
    run.step_ = mariadb_prepared_run::step::done;
}

namespace {

using run_step = mariadb_prepared_run::step;
using cache_state = mariadb_statement_cache::state;

// Binds the parameters find() accepted with the types it keyed them by.
[[nodiscard]] std::exception_ptr bind_params(
    cache_state& cache, MYSQL_STMT& statement, std::span<const db_value> params) {
    if (mysql_stmt_param_count(&statement) != params.size()) {
        return std::make_exception_ptr(std::invalid_argument("SQL parameter count does not match placeholders"));
    }
    cache.params_.resize(params.size());
    cache.values_.resize(params.size());
    for (std::size_t index = 0; index < params.size(); ++index) {
        auto& bind = cache.params_[index];
        auto& value = cache.values_[index];
        std::memset(&bind, 0, sizeof(bind));
        const auto& param = params[index];
        switch (db_value_access::type(param)) {
            case db_value_type::null:
                bind.buffer_type = MYSQL_TYPE_NULL;
                break;
            case db_value_type::string: {
                const auto text = db_value_access::text(param);
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = const_cast<char*>(text.data());
                bind.buffer_length = static_cast<unsigned long>(text.size());
                break;
            }
            case db_value_type::signed_value:
                value.integer_ = db_value_access::signed_value(param);
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::unsigned_value:
                // At most INT64_MAX: signed, as its literal reads.
                value.integer_ = static_cast<long long>(db_value_access::unsigned_value(param));
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::double_value:
                value.real_ = db_value_access::get_double_value(param);
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                bind.buffer = &value.real_;
                break;
            case db_value_type::bool_value:
                value.flag_ = db_value_access::get_bool_value(param) ? 1 : 0;
                bind.buffer_type = MYSQL_TYPE_TINY;
                bind.buffer = &value.flag_;
                break;
        }
    }
    if (!params.empty() && mysql_stmt_bind_param(&statement, cache.params_.data()) != 0) {
        return std::make_exception_ptr(statement_error(statement, "mysql_stmt_bind_param"));
    }
    return nullptr;
}

// Binds each column to the C type its binary value arrives in; text-like
// values go to a buffer as long as the longest one stored.
void bind_results(cache_state& cache, const MYSQL_FIELD* fields, std::size_t count) {
    cache.results_.resize(count);
    while (cache.columns_.size() < count) {
        cache.columns_.emplace_back(mariadb_column{std::pmr::vector<char>(cache.columns_.get_allocator().resource())});
    }
    cache.column_needed_ = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto& bind = cache.results_[index];
        auto& column = cache.columns_[index];
        const auto& field = fields[index];
        std::memset(&bind, 0, sizeof(bind));
        std::size_t size = 0;
        switch (field.type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.is_unsigned = (field.flags & UNSIGNED_FLAG) != 0 ? 1 : 0;
                size = sizeof(long long);
                break;
            case MYSQL_TYPE_FLOAT:
                bind.buffer_type = MYSQL_TYPE_FLOAT;
                size = sizeof(float);
                break;
            case MYSQL_TYPE_DOUBLE:
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                size = sizeof(double);
                break;
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_TIME:
                bind.buffer_type = field.type;
                size = sizeof(MYSQL_TIME);
                break;
            default:
                bind.buffer_type = MYSQL_TYPE_STRING;
                size = static_cast<std::size_t>(field.max_length) + 1;
                break;
        }
        // Aligned for every bound C type.
        column.buffer_.resize(std::max(size, sizeof(MYSQL_TIME)) + alignof(MYSQL_TIME));
        cache.column_needed_ += column.buffer_.size();
        auto* storage = column.buffer_.data();
        auto space = column.buffer_.size();
        void* aligned = storage;
        (void)std::align(alignof(MYSQL_TIME), size, aligned, space);
        bind.buffer = aligned;
        bind.buffer_length = static_cast<unsigned long>(size);
        bind.length = &column.length_;
        bind.is_null = &column.null_;
        bind.error = &column.error_;
    }
    cache.column_bytes_ = 0;
    for (const auto& column : cache.columns_) {
        cache.column_bytes_ += column.buffer_.capacity();
    }
}

// The text the text protocol sends for the column's fetched value.
[[nodiscard]] std::string_view column_text(
    const MYSQL_FIELD& field, const MYSQL_BIND& bind, const mariadb_column& column, mariadb_text_buffer& buffer) {
    const auto zerofill = (field.flags & ZEROFILL_FLAG) != 0 ? static_cast<std::size_t>(field.length) : 0;
    std::size_t length = 0;
    switch (bind.buffer_type) {
        case MYSQL_TYPE_LONGLONG: {
            long long value = 0;
            std::memcpy(&value, bind.buffer, sizeof(value));
            length = bind.is_unsigned != 0
                         ? format_mariadb_integer(buffer, static_cast<unsigned long long>(value), zerofill)
                         : format_mariadb_integer(buffer, value, zerofill);
            break;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
            double value = 0;
            if (bind.buffer_type == MYSQL_TYPE_FLOAT) {
                float single = 0;
                std::memcpy(&single, bind.buffer, sizeof(single));
                value = single;
            } else {
                std::memcpy(&value, bind.buffer, sizeof(value));
            }
            length = field.decimals >= mariadb_not_fixed_decimals
                         ? format_mariadb_real(buffer, value, bind.buffer_type == MYSQL_TYPE_FLOAT)
                         : format_mariadb_fixed(buffer, value, field.decimals);
            if (zerofill > length) {
                std::memmove(buffer.data() + (zerofill - length), buffer.data(), length);
                std::memset(buffer.data(), '0', zerofill - length);
                length = zerofill;
            }
            break;
        }
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIME: {
            MYSQL_TIME value{};
            std::memcpy(&value, bind.buffer, sizeof(value));
            const mariadb_temporal parts{value.year, value.month, value.day, value.hour, value.minute, value.second,
                value.second_part, value.neg != 0};
            const auto kind = bind.buffer_type == MYSQL_TYPE_DATE   ? mariadb_temporal_kind::date
                              : bind.buffer_type == MYSQL_TYPE_TIME ? mariadb_temporal_kind::time
                                                                    : mariadb_temporal_kind::datetime;
            length = format_mariadb_temporal(buffer, parts, kind, field.decimals);
            break;
        }
        default:
            return std::string_view(static_cast<const char*>(bind.buffer), column.length_);
    }
    return std::string_view(buffer.data(), length);
}

// Fetches every stored row and keeps it, as text, in `output`. The fields
// are those of the statement's current result, which libmariadb updates on
// each execution.
void materialize_rows(cache_state& cache, MYSQL_STMT& statement, db_rows& output, std::pmr::memory_resource* resource) {
    const auto field_count = static_cast<std::size_t>(statement.field_count);
    const auto* fields = statement.fields;
    bind_results(cache, fields, field_count);
    if (mysql_stmt_bind_result(&statement, cache.results_.data()) != 0) {
        throw statement_error(statement, "mysql_stmt_bind_result");
    }
    const auto row_count = static_cast<std::size_t>(mysql_stmt_num_rows(&statement));
    auto& rows = db_result_access::rows(output);
    auto& values = db_result_access::fields(output);
    auto& names = db_result_access::column_names(output);
    rows.reserve(row_count);
    values.reserve(row_count * field_count);
    names.reserve(field_count);
    for (std::size_t index = 0; index < field_count; ++index) {
        names.emplace_back(fields[index].name, static_cast<std::size_t>(fields[index].name_length));
    }
    mariadb_text_buffer buffer;
    for (;;) {
        const int fetched = mysql_stmt_fetch(&statement);
        if (fetched == MYSQL_NO_DATA) {
            break;
        }
        if (fetched != 0) {
            throw statement_error(statement, "mysql_stmt_fetch");
        }
        const auto row_start = values.size();
        for (std::size_t index = 0; index < field_count; ++index) {
            const auto& column = cache.columns_[index];
            if (column.null_ != 0) {
                values.push_back(db_result_access::null_field(resource));
                continue;
            }
            values.push_back(db_result_access::owned_field(
                column_text(fields[index], cache.results_[index], column, buffer), resource));
        }
        rows.push_back(
            db_result_access::borrowed_row(values.data() + row_start, field_count, names.data(), names.size()));
    }
}

// Drops a statement whose result the run cannot use, and leaves that
// result's rows to the draining step: forgotten, so that its next use
// prepares it anew, or refused, so that it runs as text. The statement has
// run, so it is not run again.
void start_draining(cache_state& cache, mariadb_prepared_run& run, mariadb_statement& entry, bool refuse) {
    cache.reserve_retired();
    auto* statement = std::exchange(entry.handle_, nullptr);
    if (refuse) {
        cache.refuse(run.handle_, entry.key_hash_);
    } else {
        (void)cache.statements_.forget(run.handle_, true);
    }
    cache.retire(statement);
    run.closing_ = statement;
    run.step_ = run_step::draining;
}

}  // namespace

void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output, std::pmr::memory_resource* resource) {
    auto& cache = run.cache_->data();
    auto& entry = cache.entry(run.handle_);
    auto& statement = *entry.handle_;
    if (run.returned_rows_) {
        run.extra_row_set_ = true;
    } else if (output != nullptr) {
        try {
            materialize_rows(cache, statement, *output, resource);
        } catch (...) {
            db_result_access::rows(*output).clear();
            db_result_access::fields(*output).clear();
            db_result_access::column_names(*output).clear();
            run.output_failure_ = std::current_exception();
        }
        // Buffers a much smaller result left over from a wide one are given
        // back; a run of wide results keeps them.
        if (cache.column_bytes_ > mariadb_column_bytes_kept && cache.column_needed_ * 4 < cache.column_bytes_) {
            cache.columns_.clear();
            cache.column_bytes_ = 0;
        }
    }
    run.returned_rows_ = true;
    (void)mysql_stmt_free_result(&statement);
    run.step_ = mysql_stmt_more_results(&statement) != 0 ? run_step::next_result : run_step::done;
}

int resume_mariadb_prepared(MYSQL& connection, mariadb_prepared_run& run, int ready) {
    auto& cache = run.cache_->data();
    for (;;) {
        switch (run.step_) {
            case run_step::verifying: {
                int failed = 0;
                const int status = run.started_ ? mysql_real_query_cont(&failed, &connection, ready)
                                                : mysql_real_query_start(&failed, &connection, tracking_query.data(),
                                                      static_cast<unsigned long>(tracking_query.size()));
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    if (mariadb_connection_failure(mysql_errno(&connection))) {
                        throw mysql_error(connection, "reading MariaDB session tracking", db_error::code_type::io_error);
                    }
                    cache.lose_tracking();
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                run.step_ = run_step::verified;
                continue;
            }
            case run_step::verified: {
                MYSQL_RES* result = nullptr;
                const int status = run.started_ ? mysql_store_result_cont(&result, &connection, ready)
                                                : mysql_store_result_start(&result, &connection);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (result == nullptr && mariadb_connection_failure(mysql_errno(&connection))) {
                    throw mysql_error(connection, "reading MariaDB session tracking", db_error::code_type::io_error);
                }
                const std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> owned(result, &mysql_free_result);
                auto* row = result == nullptr || mysql_num_fields(result) != 1 ? nullptr : mysql_fetch_row(result);
                const auto* lengths = row == nullptr ? nullptr : mysql_fetch_lengths(result);
                if (lengths == nullptr || row[0] == nullptr || !tracks_all(std::string_view(row[0], lengths[0]))) {
                    // A procedure changed the list without a report: the
                    // session's later changes may go unheard.
                    cache.lose_tracking();
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                cache.verify_ = false;
                run.step_ = run_step::close;
                continue;
            }
            case run_step::close: {
                if (!run.started_) {
                    if (cache.verify_) {
                        run.step_ = run_step::verifying;
                        continue;
                    }
                    if (cache.retired_.empty()) {
                        run.step_ = cache.entry(run.handle_).handle_ == nullptr ? run_step::prepare : run_step::execute;
                        if (run.step_ == run_step::execute) {
                            return 0;
                        }
                        continue;
                    }
                    run.closing_ = cache.retired_.back();
                }
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_close_cont(&failed, run.closing_, ready)
                                                : mysql_stmt_close_start(&failed, run.closing_);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                // Freed whether or not the server heard of it.
                cache.retired_.pop_back();
                run.closing_ = nullptr;
                if (failed != 0) {
                    throw mysql_error(connection, "mysql_stmt_close", db_error::code_type::io_error);
                }
                continue;
            }
            case run_step::prepare: {
                auto& entry = cache.entry(run.handle_);
                if (!run.started_) {
                    cache.reserve_retired();
                    auto* created = mysql_stmt_init(&connection);
                    if (created == nullptr) {
                        throw std::bad_alloc();
                    }
                    entry.handle_ = created;
                    const my_bool update_max_length = 1;
                    (void)mysql_stmt_attr_set(created, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);
                }
                // The cache owns the text libmariadb sends, also while the
                // prepare is suspended and the statement's caller has left.
                const auto sql = cache.statements_.sql(run.handle_);
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_prepare_cont(&failed, entry.handle_, ready)
                                                : mysql_stmt_prepare_start(&failed, entry.handle_, sql.data(),
                                                      static_cast<unsigned long>(sql.size()));
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                auto& statement = *entry.handle_;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_prepare");
                    }
                    // Never prepared, so closing it sends nothing.
                    (void)mysql_stmt_close(std::exchange(entry.handle_, nullptr));
                    cache.refuse(run.handle_, entry.key_hash_);
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                if (!readable_as_text(statement.fields, statement.field_count, false)) {
                    // Prepared on the server: closed by the next run.
                    cache.retire(std::exchange(entry.handle_, nullptr));
                    cache.refuse(run.handle_, entry.key_hash_);
                    run.text_ = true;
                    run.step_ = run_step::done;
                    return 0;
                }
                run.step_ = run_step::execute;
                return 0;
            }
            case run_step::execute:
            case run_step::executing: {
                auto& entry = cache.entry(run.handle_);
                auto& statement = *entry.handle_;
                if (run.step_ == run_step::execute) {
                    if (auto failure = bind_params(cache, statement, run.params_); failure != nullptr) {
                        run.failure_ = std::move(failure);
                        run.step_ = run_step::done;
                        return 0;
                    }
                    run.step_ = run_step::executing;
                }
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_execute_cont(&failed, &statement, ready)
                                                : mysql_stmt_execute_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    const auto code = mysql_stmt_errno(&statement);
                    if (code == CR_NEW_STMT_METADATA) {
                        // The server prepared it anew, a table having
                        // changed, and ran it: its rows, of a shape the
                        // handle was not prepared for, are on their way.
                        run.failure_ = std::make_exception_ptr(mariadb_error("mysql_stmt_execute", code,
                            mysql_stmt_sqlstate(&statement),
                            "the statement's result changed shape since it was prepared; it is prepared anew on its "
                            "next use",
                            db_error::code_type::statement_failed));
                        start_draining(cache, run, entry, false);
                        continue;
                    }
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_execute");
                    }
                    run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_execute"));
                    if (code == ER_NEED_REPREPARE || code == ER_UNKNOWN_STMT_HANDLER) {
                        // The server no longer runs it as prepared.
                        cache.reserve_retired();
                        cache.retire(std::exchange(entry.handle_, nullptr));
                        (void)cache.statements_.forget(run.handle_, true);
                        run.stale_ = true;
                    }
                    run.step_ = run_step::done;
                    return 0;
                }
                run.affected_rows_ = mysql_stmt_affected_rows(&statement);
                run.insert_id_ = mysql_stmt_insert_id(&statement);
                if (mysql_stmt_field_count(&statement) != 0) {
                    if (!readable_as_text(statement.fields, statement.field_count, true)) {
                        run.failure_ = std::make_exception_ptr(db_error(db_error::code_type::statement_failed,
                            db_driver::mariadb,
                            "MariaDB prepared statement result has a YEAR column whose text the binary protocol does "
                            "not tell; the statement runs as text from its next use"));
                        start_draining(cache, run, entry, true);
                        continue;
                    }
                    run.step_ = run_step::storing;
                    continue;
                }
                run.step_ = mysql_stmt_more_results(&statement) != 0 ? run_step::next_result : run_step::done;
                if (run.step_ == run_step::done) {
                    return 0;
                }
                continue;
            }
            case run_step::storing: {
                auto& statement = *cache.entry(run.handle_).handle_;
                int failed = 0;
                const int status = run.started_ ? mysql_stmt_store_result_cont(&failed, &statement, ready)
                                                : mysql_stmt_store_result_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_store_result");
                    }
                    if (run.failure_ == nullptr) {
                        run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_store_result"));
                    }
                    // The server's error ended the rows, but libmariadb still
                    // holds the statement as waiting for its result: it would
                    // refuse to close or reset any other statement of the
                    // connection, and freeing the result would leave it
                    // expecting more rows on its next execution.
                    run.step_ = run_step::resetting;
                    continue;
                }
                run.step_ = run_step::rows;
                return 0;
            }
            case run_step::next_result: {
                auto& statement = *cache.entry(run.handle_).handle_;
                int result = 0;
                const int status = run.started_ ? mysql_stmt_next_result_cont(&result, &statement, ready)
                                                : mysql_stmt_next_result_start(&result, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                ready = 0;
                if (result > 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_next_result");
                    }
                    if (run.failure_ == nullptr) {
                        run.failure_ = std::make_exception_ptr(statement_error(statement, "mysql_stmt_next_result"));
                    }
                    run.step_ = run_step::done;
                    return 0;
                }
                if (result < 0) {
                    run.step_ = run_step::done;
                    return 0;
                }
                if (mysql_stmt_field_count(&statement) != 0) {
                    run.step_ = run_step::storing;
                    continue;
                }
                if (mysql_stmt_more_results(&statement) == 0) {
                    run.step_ = run_step::done;
                    return 0;
                }
                continue;
            }
            case run_step::resetting: {
                auto& entry = cache.entry(run.handle_);
                auto& statement = *entry.handle_;
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_reset_cont(&failed, &statement, ready)
                                                : mysql_stmt_reset_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                if (failed != 0) {
                    if (connection_failure(statement)) {
                        throw_connection_failure(connection, statement, "mysql_stmt_reset");
                    }
                    // The server no longer knows it: it is prepared anew.
                    cache.reserve_retired();
                    cache.retire(std::exchange(entry.handle_, nullptr));
                    (void)cache.statements_.forget(run.handle_, true);
                }
                run.step_ = run_step::done;
                return 0;
            }
            case run_step::draining: {
                auto& statement = *run.closing_;
                my_bool failed = 0;
                const int status = run.started_ ? mysql_stmt_free_result_cont(&failed, &statement, ready)
                                                : mysql_stmt_free_result_start(&failed, &statement);
                run.started_ = true;
                if (status != 0) {
                    return status;
                }
                run.started_ = false;
                run.closing_ = nullptr;
                if (failed != 0) {
                    throw_connection_failure(connection, statement, "mysql_stmt_free_result");
                }
                run.step_ = run_step::done;
                return 0;
            }
            case run_step::rows:
            case run_step::done:
                return 0;
        }
    }
}

}  // namespace ruvia::detail
