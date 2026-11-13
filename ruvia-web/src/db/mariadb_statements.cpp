#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/db/db_session.h"

#include "db/db_sql_format.h"
#include "db/mariadb_pipeline.h"
#include "db/mariadb_sql.h"

namespace ruvia::detail {
namespace {

// The text MariaDB's text protocol sends for values its binary protocol
// carries, so a statement returns the same db_field text whichever protocol
// ran it. The server formats a double with my_gcvt() into a buffer of
// FLOATING_POINT_BUFFER - 1 characters, a float the same way with at most
// FLT_DIG significant digits, and a value of fixed scale with my_fcvt(); both
// round with dtoa, which std::to_chars reproduces: the shortest digits that
// read back as the value, or the value correctly rounded when those are too
// many.

// The decimals a field reports when its value has no fixed scale.
constexpr unsigned int mariadb_not_fixed_decimals = 31;
// Room for any formatted value: a double in plain notation reaches about 330
// characters.
constexpr std::size_t mariadb_text_capacity = 400;

using mariadb_text_buffer = std::array<char, mariadb_text_capacity>;

namespace mariadb_text_detail {

// The significant digits of a nonzero finite magnitude and where its decimal
// point falls, as dtoa returns them: 1234 at point 2 is 12.34.
struct digits final {
    std::array<char, 32> text_{};
    int length_{0};
    int point_{0};
};

// Reads std::to_chars' scientific form "d.ddde+XX".
[[nodiscard]] digits parse_scientific(const char* first, const char* last) noexcept {
    digits result;
    const char* cursor = first;
    for (; cursor != last && *cursor != 'e'; ++cursor) {
        if (*cursor != '.') {
            result.text_[static_cast<std::size_t>(result.length_++)] = *cursor;
        }
    }
    int exponent = 0;
    if (cursor != last) {
        (void)std::from_chars(cursor + (cursor[1] == '+' ? 2 : 1), last, exponent);
    }
    while (result.length_ > 1 && result.text_[static_cast<std::size_t>(result.length_ - 1)] == '0') {
        --result.length_;
    }
    result.point_ = exponent + 1;
    return result;
}

// dtoa's mode 4: the shortest digits that read back as `magnitude`, or, when
// those are more than `limit`, `magnitude` correctly rounded to `limit`
// significant digits.
[[nodiscard]] digits significant(double magnitude, int limit) noexcept {
    std::array<char, 64> scratch{};
    auto converted =
        std::to_chars(scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific);
    auto result = parse_scientific(scratch.data(), converted.ptr);
    if (result.length_ > limit) {
        converted = std::to_chars(
            scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific, limit - 1);
        result = parse_scientific(scratch.data(), converted.ptr);
    }
    return result;
}

// Digits in plain notation: "0.00ddd", "dd.ddd" or "ddd00".
[[nodiscard]] char* put_plain(char* out, const digits& found) noexcept {
    if (found.point_ <= 0) {
        *out++ = '0';
        *out++ = '.';
        for (int index = found.point_; index < 0; ++index) {
            *out++ = '0';
        }
    }
    int index = 1;
    for (; index <= found.length_; ++index) {
        *out++ = found.text_[static_cast<std::size_t>(index - 1)];
        if (index == found.point_ && index < found.length_) {
            *out++ = '.';
        }
    }
    for (; index <= found.point_; ++index) {
        *out++ = '0';
    }
    return out;
}

}  // namespace mariadb_text_detail

// my_gcvt() with the server's width: plain notation while it shows every
// significant digit and the point stays within DBL_DIG places of the digits,
// else scientific ("1e20", "1.5e-20").
[[nodiscard]] std::size_t format_mariadb_real(mariadb_text_buffer& buffer, double value, bool single) noexcept {
    namespace text = mariadb_text_detail;
    constexpr int dbl_dig = 15;
    constexpr int flt_dig = 6;
    if (!std::isfinite(value)) {
        // dtoa reports an overflow for infinity and NaN, which my_gcvt()
        // writes as "0".
        buffer[0] = '0';
        return 1;
    }
    int width = 310;
    char* out = buffer.data();
    if (std::signbit(value)) {
        *out++ = '-';
        --width;
    }
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        *out++ = '0';
        return static_cast<std::size_t>(out - buffer.data());
    }
    const auto found = text::significant(magnitude, single ? flt_dig : width);
    const int length = found.length_;
    const int point = found.point_;
    // The plain notation's length: "0.00ddd", "dd.ddd" or "ddd00".
    int plain_length = point;
    if (point <= 0) {
        plain_length = length - point + 2;
    } else if (point < length) {
        plain_length = length + 1;
    }
    const bool have_space = plain_length <= width;
    if (have_space && point >= -dbl_dig + 1 && (point <= dbl_dig || length > point)) {
        out = text::put_plain(out, found);
        return static_cast<std::size_t>(out - buffer.data());
    }
    *out++ = found.text_[0];
    if (length > 1) {
        *out++ = '.';
        for (int index = 1; index < length; ++index) {
            *out++ = found.text_[static_cast<std::size_t>(index)];
        }
    }
    *out++ = 'e';
    int exponent = point - 1;
    if (exponent < 0) {
        *out++ = '-';
        exponent = -exponent;
    }
    out = std::to_chars(out, buffer.data() + buffer.size(), exponent).ptr;
    return static_cast<std::size_t>(out - buffer.data());
}

// my_fcvt(): exactly `decimals` digits after the point. dtoa keeps the
// shortest digits that read back as the value when they fit, and pads them
// with zeros; otherwise the value is correctly rounded.
[[nodiscard]] std::size_t format_mariadb_fixed(
    mariadb_text_buffer& buffer, double value, unsigned int decimals) noexcept {
    namespace text = mariadb_text_detail;
    if (!std::isfinite(value)) {
        // As my_gcvt(), my_fcvt() writes dtoa's overflow as "0".
        buffer[0] = '0';
        return 1;
    }
    const double magnitude = std::fabs(value);
    if (magnitude != 0) {
        const auto found = text::significant(magnitude, 17);
        const int fraction = found.length_ > found.point_ ? found.length_ - found.point_ : 0;
        if (fraction <= static_cast<int>(decimals)) {
            char* out = buffer.data();
            if (std::signbit(value)) {
                *out++ = '-';
            }
            out = text::put_plain(out, found);
            if (decimals > 0) {
                if (fraction == 0) {
                    *out++ = '.';
                }
                for (int pad = static_cast<int>(decimals) - fraction; pad > 0; --pad) {
                    *out++ = '0';
                }
            }
            return static_cast<std::size_t>(out - buffer.data());
        }
    }
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, static_cast<int>(decimals));
    return static_cast<std::size_t>(converted.ptr - buffer.data());
}

// An integer as the text protocol sends it: a ZEROFILL column pads it with
// zeros to the column's display width.
template <typename integer_type>
[[nodiscard]] std::size_t format_mariadb_integer(
    mariadb_text_buffer& buffer, integer_type value, std::size_t zerofill_width) noexcept {
    std::array<char, 24> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const auto length = static_cast<std::size_t>(converted.ptr - digits.data());
    char* out = buffer.data();
    for (std::size_t pad = length; pad < zerofill_width && pad < buffer.size() - digits.size(); ++pad) {
        *out++ = '0';
    }
    for (std::size_t index = 0; index < length; ++index) {
        *out++ = digits[index];
    }
    return static_cast<std::size_t>(out - buffer.data());
}

// The parts of a DATE, DATETIME, TIMESTAMP or TIME value.
struct mariadb_temporal final {
    unsigned int year_{0};
    unsigned int month_{0};
    unsigned int day_{0};
    // A TIME's hours include its days.
    unsigned int hour_{0};
    unsigned int minute_{0};
    unsigned int second_{0};
    unsigned long microsecond_{0};
    bool negative_{false};
};

enum class mariadb_temporal_kind : std::uint8_t { date,
    datetime,
    time };

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM:SS" or "[-]HH:MM:SS", with the column's
// fractional digits. A column without a fixed number of them
// (AUTO_SEC_PART_DIGITS) shows six when the value has a fraction, else none.
[[nodiscard]] std::size_t format_mariadb_temporal(mariadb_text_buffer& buffer, const mariadb_temporal& value,
    mariadb_temporal_kind kind, unsigned int decimals) noexcept {
    if (decimals > 6) {
        decimals = value.microsecond_ != 0 ? 6 : 0;
    }
    char* out = buffer.data();
    const auto put_number = [&out](unsigned long number, int width) {
        for (int index = width - 1; index >= 0; --index) {
            out[index] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
        out += width;
    };
    if (kind != mariadb_temporal_kind::time) {
        put_number(value.year_, 4);
        *out++ = '-';
        put_number(value.month_, 2);
        *out++ = '-';
        put_number(value.day_, 2);
        if (kind == mariadb_temporal_kind::date) {
            return static_cast<std::size_t>(out - buffer.data());
        }
        *out++ = ' ';
        put_number(value.hour_, 2);
    } else {
        if (value.negative_) {
            *out++ = '-';
        }
        put_number(value.hour_, value.hour_ >= 100 ? 3 : 2);
    }
    *out++ = ':';
    put_number(value.minute_, 2);
    *out++ = ':';
    put_number(value.second_, 2);
    if (decimals > 0) {
        *out++ = '.';
        unsigned long fraction = value.microsecond_;
        for (unsigned int drop = 6; drop > decimals; --drop) {
            fraction /= 10;
        }
        put_number(fraction, static_cast<int>(decimals));
    }
    return static_cast<std::size_t>(out - buffer.data());
}

// Column buffers above this many bytes are given back once their rows have
// been read, so a connection does not keep the size of its widest result.
constexpr std::size_t mariadb_column_bytes_kept = 64 * 1024;

static_assert(std::is_same_v<my_bool, char>);
static_assert(alignof(std::max_align_t) >= alignof(MYSQL_BIND));
static_assert(std::is_trivially_copyable_v<MYSQL_BIND>);

[[nodiscard]] MYSQL_BIND* reserve_binds(std::vector<std::max_align_t>& storage, std::size_t count) {
    const std::size_t units = (count * sizeof(MYSQL_BIND) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    storage.resize(units);
    return reinterpret_cast<MYSQL_BIND*>(storage.data());
}

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
    switch (param.type()) {
        case db_value_type::null:
            return MYSQL_TYPE_NULL;
        case db_value_type::string:
            if (param.text().find('\0') != std::string_view::npos) {
                return std::nullopt;
            }
            return MYSQL_TYPE_STRING;
        case db_value_type::signed_value:
            if (param.signed_value() < 0) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::unsigned_value:
            if (param.unsigned_value() >
                static_cast<std::uint64_t>((std::numeric_limits<long long>::max)())) {
                return std::nullopt;
            }
            return MYSQL_TYPE_LONGLONG;
        case db_value_type::double_value: {
            const auto value = param.double_value();
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
    while (!list.empty()) {
        if (http_ascii_equals_ignore_case(next_item(list), name)) {
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
void hear_session_status(void* data, status_type type, ...) {
    if (type != static_cast<status_type>(SESSION_TRACK_TYPE)) {
        return;
    }
    auto& cache = *static_cast<mariadb_statement_cache*>(data);
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

}  // namespace

mariadb_statement_cache::mariadb_statement_cache()
    : retired_(),
      types_(),
      binds_(),
      values_(),
      columns_() {}

mariadb_statement_cache::~mariadb_statement_cache() {
    clear();
}

mariadb_statement& mariadb_statement_cache::entry(entry_ref statement) {
    auto* found = statements_.payload(statement);
    if (found == nullptr) {
        throw db_error(
            db_error::code_type::protocol_error, db_driver::mariadb, "MariaDB statement left the cache");
    }
    return *found;
}

void mariadb_statement_cache::reserve_retired(std::size_t count) {
    retired_.reserve(retired_.size() + count);
}

void mariadb_statement_cache::retire(MYSQL_STMT* statement) noexcept {
    if (statement != nullptr) {
        retired_.push_back(statement);
    }
}

void mariadb_statement_cache::refuse(std::size_t hash) noexcept {
    refused_[next_refused_] = hash;
    next_refused_ = (next_refused_ + 1) % refused_.size();
}

void mariadb_statement_cache::refuse(entry_ref statement, std::size_t hash) noexcept {
    (void)statements_.forget(statement, false);
    refuse(hash);
}

bool mariadb_statement_cache::refused(std::size_t hash) const noexcept {
    return std::ranges::find(refused_, hash) != refused_.end();
}

void mariadb_statement_cache::invalidate() {
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

bool mariadb_statement_cache::usable() const noexcept {
    return tracked_ && all_variables_ && schema_tracked_ && !untracked_ && client_utf8mb4_ && connection_utf8mb4_;
}

void mariadb_statement_cache::lose_tracking() noexcept {
    untracked_ = true;
    changed_ = true;
}

// Runs only once the connection is closed (connect() after close_slot(),
// or the slot's end after the pool closed it). A handle whose close was
// suspended has already left the connection's list, so mysql_close() did
// not detach it: detaching each one here keeps mysql_stmt_close() from
// reaching the freed connection.
void mariadb_statement_cache::clear() noexcept {
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

bool mariadb_statement_cache::observe(MYSQL& connection) {
    // libmariadb reads both arguments as void*.
    return mysql_optionsv(&connection, MARIADB_OPT_STATUS_CALLBACK, reinterpret_cast<void*>(&hear_session_status),
               static_cast<void*>(this)) == 0;
}

void mariadb_statement_cache::connected(bool tracked) noexcept {
    tracked_ = tracked;
    all_variables_ = tracked;
    schema_tracked_ = tracked;
    changed_ = true;
}

void mariadb_statement_cache::untrack() noexcept {
    lose_tracking();
}

void mariadb_statement_cache::routine_called() noexcept {
    verify_ = true;
}

bool mariadb_statement_cache::client_utf8mb4() const noexcept {
    return tracked_ && all_variables_ && !untracked_ && !verify_ && client_utf8mb4_;
}

std::optional<mariadb_statement_cache::entry_ref> mariadb_statement_cache::find(
    std::string_view sql, std::span<const db_value> params, bool may_prepare) {
    auto& cache = *this;
    if (cache.changed_) {
        cache.invalidate();
    }
    if (!cache.usable()) {
        return std::nullopt;
    }
    auto& types = cache.types_;
    types.clear();
    types.reserve(params.size());
    for (const auto& param : params) {
        const auto type = bound_type(param);
        if (!type.has_value()) {
            return std::nullopt;
        }
        types.push_back(*type);
    }
    // A statement that may not run prepared is refused inside make() before it
    // could take the place of one that may.
    const auto make = [&cache](std::string_view candidate_sql, std::span<const unsigned int>,
                          std::size_t hash) -> std::optional<mariadb_statement> {
        if (cache.refused(hash)) {
            return std::nullopt;
        }
        const auto traits = mariadb_statement_traits_of(candidate_sql);
        if (!traits.preparable_) {
            cache.refuse(hash);
            return std::nullopt;
        }
        cache.reserve_retired();
        return mariadb_statement{nullptr, traits.integer_params_, hash};
    };
    auto found = cache.statements_.find(sql, types, may_prepare, make);
    if (found.evicted_.has_value()) {
        cache.retire(found.evicted_->handle_);
    }
    if (!may_prepare || !found.statement_.has_value()) {
        return std::nullopt;
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
    mariadb_statement_cache& cache, mariadb_statement_cache::entry_ref statement, std::span<const db_value> params) noexcept {
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
using cache_state = mariadb_statement_cache;

// Binds the parameters find() accepted with the types it keyed them by.
[[nodiscard]] std::exception_ptr bind_params(
    cache_state& cache, MYSQL_STMT& statement, std::span<const db_value> params) {
    if (mysql_stmt_param_count(&statement) != params.size()) {
        return std::make_exception_ptr(std::invalid_argument("SQL parameter count does not match placeholders"));
    }
    auto* binds = reserve_binds(cache.binds_, params.size());
    cache.values_.resize(params.size());
    for (std::size_t index = 0; index < params.size(); ++index) {
        auto& bind = binds[index];
        std::memset(&bind, 0, sizeof(bind));
        auto& value = cache.values_[index];
        const auto& param = params[index];
        switch (param.type()) {
            case db_value_type::null:
                bind.buffer_type = MYSQL_TYPE_NULL;
                break;
            case db_value_type::string: {
                const auto text = param.text();
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = const_cast<char*>(text.data());
                bind.buffer_length = static_cast<unsigned long>(text.size());
                break;
            }
            case db_value_type::signed_value:
                value.integer_ = param.signed_value();
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::unsigned_value:
                // At most INT64_MAX: signed, as its literal reads.
                value.integer_ = static_cast<long long>(param.unsigned_value());
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &value.integer_;
                break;
            case db_value_type::double_value:
                value.real_ = param.double_value();
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                bind.buffer = &value.real_;
                break;
            case db_value_type::bool_value:
                value.flag_ = param.bool_value() ? 1 : 0;
                bind.buffer_type = MYSQL_TYPE_TINY;
                bind.buffer = &value.flag_;
                break;
        }
    }
    if (!params.empty() && mysql_stmt_bind_param(&statement, binds) != 0) {
        return std::make_exception_ptr(statement_error(statement, "mysql_stmt_bind_param"));
    }
    return nullptr;
}

// Binds each column to the C type its binary value arrives in; text-like
// values go to a buffer as long as the longest one stored.
MYSQL_BIND* bind_results(cache_state& cache, const MYSQL_FIELD* fields, std::size_t count) {
    auto* binds = reserve_binds(cache.binds_, count);
    while (cache.columns_.size() < count) {
        cache.columns_.emplace_back(mariadb_column{std::vector<char>()});
    }
    cache.column_needed_ = 0;
    for (std::size_t index = 0; index < count; ++index) {
        auto& bind = binds[index];
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
    return binds;
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
void materialize_rows(cache_state& cache, MYSQL_STMT& statement, db_rows& output) {
    const auto field_count = static_cast<std::size_t>(statement.field_count);
    const auto* fields = statement.fields;
    auto* binds = bind_results(cache, fields, field_count);
    if (mysql_stmt_bind_result(&statement, binds) != 0) {
        throw statement_error(statement, "mysql_stmt_bind_result");
    }
    const auto row_count = static_cast<std::size_t>(mysql_stmt_num_rows(&statement));
    auto& values = db_result_access::fields(output);
    auto& names = db_result_access::column_names(output);
    db_result_access::reserve_result(output, row_count, field_count);
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
                values.push_back(db_result_access::null_field());
                continue;
            }
            values.push_back(
                db_result_access::owned_field(column_text(fields[index], binds[index], column, buffer)));
        }
        db_result_access::append_row(output, row_start, field_count);
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

void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output) {
    auto& cache = *run.cache_;
    auto& entry = cache.entry(run.handle_);
    auto& statement = *entry.handle_;
    if (run.returned_rows_) {
        run.extra_row_set_ = true;
    } else if (output != nullptr) {
        try {
            materialize_rows(cache, statement, *output);
        } catch (...) {
            db_result_access::clear(*output);
            run.output_failure_ = std::current_exception();
        }
    }
    if (cache.column_bytes_ > mariadb_column_bytes_kept) {
        cache.columns_.clear();
        cache.column_bytes_ = 0;
    }
    run.returned_rows_ = true;
    (void)mysql_stmt_free_result(&statement);
    run.step_ = mysql_stmt_more_results(&statement) != 0 ? run_step::next_result : run_step::done;
}

int resume_mariadb_prepared(MYSQL& connection, mariadb_prepared_run& run, int ready) {
    auto& cache = *run.cache_;
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

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t mariadb_string_literal_size_hint(std::size_t value_size) {
    // mysql_real_escape_string may expand every input byte to two output
    // bytes, and its input/output lengths are both unsigned long. Check both
    // bounds before doing the multiplication or passing the input to C.
    if (value_size > (std::numeric_limits<unsigned long>::max)() / 2 ||
        value_size > ((std::numeric_limits<std::size_t>::max)() - 2) / 2) {
        throw std::length_error("MariaDB string parameter is too large");
    }
    return value_size * 2 + 2;
}

// A backslash in `value` that a backslash put in front of it could join to
// the byte before: one that follows a byte above 0x7F, which a multibyte
// character set may read as a character's lead byte.
[[nodiscard]] bool ambiguous_backslash(std::string_view value) noexcept {
    for (auto backslash = value.find('\\'); backslash != std::string_view::npos;
        backslash = value.find('\\', backslash + 1)) {
        if (backslash != 0 && static_cast<unsigned char>(value[backslash - 1]) > 0x7f) {
            return true;
        }
    }
    return false;
}

// A string literal: each quote doubled, which reads the same in every
// sql_mode and character set, as no character set has a multibyte character
// ending in a quote byte; and, when the session reads backslash escapes, each
// backslash doubled. `charset` keeps a multibyte character whole, so that a
// backslash byte inside one is not escaped; without it, no backslash may
// follow a byte above 0x7F (ambiguous_backslash()), and an ASCII byte never
// joins the backslash put after it.
void append_string_literal(std::string& output, std::string_view value, bool backslash_escapes,
    const MARIADB_CHARSET_INFO* charset) {
    const bool multibyte = backslash_escapes && charset != nullptr && charset->char_maxlen > 1 &&
                           value.find('\\') != std::string_view::npos;
    output.push_back('\'');
    if (!multibyte) {
        const auto special = backslash_escapes ? std::string_view("'\\", 2) : std::string_view("'", 1);
        for (auto at = value.find_first_of(special); at != std::string_view::npos; at = value.find_first_of(special)) {
            output.append(value.substr(0, at + 1));
            output.push_back(value[at]);
            value.remove_prefix(at + 1);
        }
        output.append(value);
    } else {
        const char* const end = value.data() + value.size();
        for (const char* cursor = value.data(); cursor != end;) {
            if (const auto length = charset->mb_valid(cursor, end); length > 1) {
                output.append(cursor, length);
                cursor += length;
                continue;
            }
            const char byte = *cursor++;
            if (byte == '\'' || byte == '\\') {
                output.push_back(byte);
            }
            output.push_back(byte);
        }
    }
    output.push_back('\'');
}

[[nodiscard]] std::size_t value_literal_size_hint(const db_value& value) {
    switch (value.type()) {
        case db_value_type::null:
            return 4;
        case db_value_type::string:
            return mariadb_string_literal_size_hint(value.text().size());
        case db_value_type::signed_value:
        case db_value_type::unsigned_value:
            return 32;
        case db_value_type::double_value:
            return 64;
        case db_value_type::bool_value:
            return 1;
    }
    return 0;
}

void append_value_literal(
    std::string& output, const db_value& value, bool backslash_escapes, const MARIADB_CHARSET_INFO* charset) {
    switch (value.type()) {
        case db_value_type::null:
            output.append("NULL");
            break;
        case db_value_type::string:
            append_string_literal(output, value.text(), backslash_escapes, charset);
            break;
        case db_value_type::signed_value:
            append_db_number(output, value.signed_value());
            break;
        case db_value_type::unsigned_value:
            append_db_number(output, value.unsigned_value());
            break;
        case db_value_type::double_value:
            append_db_double_literal(output, value.double_value(), db_driver::mariadb);
            break;
        case db_value_type::bool_value:
            output.push_back(value.bool_value() ? '1' : '0');
            break;
    }
}

// A server error after which the server ends the session, or which shows
// that it read a packet it did not expect.
[[nodiscard]] bool session_ended(unsigned int native_code) noexcept {
    switch (native_code) {
        case ER_SERVER_SHUTDOWN:
        case ER_FORCING_CLOSE:
        case ER_ABORTING_CONNECTION:
        case ER_NET_PACKET_TOO_LARGE:
        case ER_NET_READ_ERROR_FROM_PIPE:
        case ER_NET_FCNTL_ERROR:
        case ER_NET_PACKETS_OUT_OF_ORDER:
        case ER_NET_UNCOMPRESS_ERROR:
        case ER_NET_READ_ERROR:
        case ER_NET_READ_INTERRUPTED:
        case ER_NET_ERROR_ON_WRITE:
        case ER_NET_WRITE_INTERRUPTED:
        case ER_NEW_ABORTING_CONNECTION:
        case ER_CONNECTION_KILLED:
            return true;
        default:
            return false;
    }
}

// One statement-level token of MariaDB SQL.
struct sql_token final {
    enum class kind : std::uint8_t {
        end,
        word,
        number,
        placeholder,
        comma,
        open,
        // An executable comment ("/*!" or "/*M!"): the server runs its
        // content as SQL.
        hidden,
        other,
    };

    kind kind_{kind::end};
    std::string_view text_{};
};

[[nodiscard]] constexpr bool sql_word_byte(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '$' || byte >= 0x80;
}

[[nodiscard]] bool sql_keyword(std::string_view word, std::string_view keyword) noexcept {
    return http_ascii_equals_ignore_case(word, keyword);
}

// Reads the token at `index` and moves past it. Whitespace and comments only
// separate tokens; a literal or quoted identifier is one `other` token.
[[nodiscard]] sql_token next_sql_token(std::string_view sql, std::size_t& index, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    while (index < sql.size()) {
        const auto start = index;
        const char character = sql[index];
        if (is_sql_whitespace(character)) {
            ++index;
            continue;
        }
        const bool block_comment = character == '/' && sql.substr(index, 2) == "/*";
        if (block_comment || character == '#' || is_mariadb_double_dash_comment(sql, index)) {
            index = skip_sql_atom(sql, index, backslash_escapes);
            if (block_comment && (sql.substr(start, 3) == "/*!" || sql.substr(start, 4) == "/*M!")) {
                return {kind::hidden, sql.substr(start, index - start)};
            }
            continue;
        }
        if (character == '\'' || character == '"' || character == '`') {
            index = skip_sql_atom(sql, index, backslash_escapes);
            return {kind::other, sql.substr(start, index - start)};
        }
        if (sql_word_byte(character)) {
            while (index < sql.size() && sql_word_byte(sql[index])) {
                ++index;
            }
            const bool number = character >= '0' && character <= '9';
            return {number ? kind::number : kind::word, sql.substr(start, index - start)};
        }
        ++index;
        switch (character) {
            case '?':
                return {kind::placeholder, sql.substr(start, 1)};
            case ',':
                return {kind::comma, sql.substr(start, 1)};
            case '(':
                return {kind::open, sql.substr(start, 1)};
            default:
                return {kind::other, sql.substr(start, 1)};
        }
    }
    return {};
}

// The first token past opening parentheses.
[[nodiscard]] sql_token leading_sql_token(std::string_view sql, bool backslash_escapes) noexcept {
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == sql_token::kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    return token;
}

// A placeholder where the text protocol takes only an integer literal, by the
// tokens before it, most recent first: "LIMIT ?", "OFFSET ?", "FETCH FIRST ?",
// "FETCH NEXT ?", "ROWS EXAMINED ?" and the second count of "LIMIT n, ?".
[[nodiscard]] bool integer_placeholder(const std::array<sql_token, 3>& before) noexcept {
    using kind = sql_token::kind;
    if (before[0].kind_ == kind::word) {
        const auto word = before[0].text_;
        return sql_keyword(word, "limit") || sql_keyword(word, "offset") || sql_keyword(word, "first") ||
               sql_keyword(word, "next") || sql_keyword(word, "examined");
    }
    return before[0].kind_ == kind::comma &&
           (before[1].kind_ == kind::placeholder || before[1].kind_ == kind::number) &&
           before[2].kind_ == kind::word && sql_keyword(before[2].text_, "limit");
}

// The traits of `sql` read in one backslash mode; nullopt-like `placeholders`
// lets the caller compare the two readings.
struct sql_reading final {
    mariadb_statement_traits traits_{true, 0};
    std::size_t placeholders_{0};
};

[[nodiscard]] sql_reading read_mariadb_statement(std::string_view sql, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    sql_reading reading;
    auto& traits = reading.traits_;
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    if (token.kind_ == kind::hidden ||
        (token.kind_ == kind::word && (sql_keyword(token.text_, "call") || sql_keyword(token.text_, "load") ||
                                          sql_keyword(token.text_, "set") || sql_keyword(token.text_, "use")))) {
        traits.preparable_ = false;
        return reading;
    }
    std::array<sql_token, 3> before{};
    for (; token.kind_ != kind::end; token = next_sql_token(sql, index, backslash_escapes)) {
        if (token.kind_ == kind::hidden || (token.kind_ == kind::word && sql_keyword(token.text_, "returning"))) {
            traits.preparable_ = false;
            return reading;
        }
        if (token.kind_ == kind::placeholder) {
            if (integer_placeholder(before)) {
                if (reading.placeholders_ >= 64) {
                    traits.preparable_ = false;
                    return reading;
                }
                traits.integer_params_ |= std::uint64_t{1} << reading.placeholders_;
            }
            ++reading.placeholders_;
        }
        before = {token, before[0], before[1]};
    }
    return reading;
}

}  // namespace

void validate_mariadb_sql_length(std::size_t length) {
    if (length > (std::numeric_limits<unsigned long>::max)()) {
        throw std::length_error("MariaDB SQL is too large for the client API");
    }
}

db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type error_code) {
    // A statement that failed because the connection did is an I/O failure,
    // not a statement the server rejected.
    if (error_code == db_error::code_type::statement_failed &&
        (native_code == CR_SERVER_GONE_ERROR || native_code == CR_SERVER_LOST || native_code == CR_ERR_NET_READ ||
            native_code == CR_ERR_NET_WRITE || session_ended(native_code))) {
        error_code = db_error::code_type::io_error;
    }
    const std::string_view state_view =
        (sql_state != nullptr && sql_state[0] != '\0') ? std::string_view(sql_state) : std::string_view{};
    const std::string_view message_view =
        (message != nullptr && message[0] != '\0') ? std::string_view(message) : std::string_view{};
    std::array<char, http_max_unsigned_decimal_size> digits;
    const std::string_view code_view =
        native_code != 0 ? http_format_unsigned(digits, static_cast<std::uint64_t>(native_code)) : std::string_view{};
    std::string error;
    error.reserve(operation.size() + 7U + (code_view.empty() ? 0U : 9U + code_view.size()) +
                  (state_view.empty() ? 0U : 12U + state_view.size()) +
                  (message_view.empty() ? 0U : 2U + message_view.size()));
    error.append(operation);
    error.append(" failed");
    if (!code_view.empty()) {
        error.append(" [errno=");
        error.append(code_view);
        error.push_back(']');
    }
    if (!state_view.empty()) {
        error.append(" [sqlstate=");
        error.append(state_view);
        error.push_back(']');
    }
    if (!message_view.empty()) {
        error.append(": ");
        error.append(message_view);
    }
    return db_error(error_code, db_driver::mariadb, error,
        native_code == 0 ? std::nullopt : std::optional<std::int64_t>(native_code),
        sql_state == nullptr ? std::string_view{} : std::string_view(sql_state));
}

db_error mysql_error(const st_mysql& connection, std::string_view operation, db_error::code_type error_code) {
    auto* mutable_connection = const_cast<st_mysql*>(&connection);
    return mariadb_error(operation, mysql_errno(mutable_connection), mysql_sqlstate(mutable_connection),
        mysql_error(mutable_connection), error_code);
}

bool mariadb_connection_failure(unsigned int native_code) noexcept {
    // libmariadb's own errors (the CR_ range and its extended CER_ range), and
    // a failure without a code, which has no known cause.
    return native_code == 0 || (native_code >= CR_MIN_ERROR && native_code <= CR_MAX_ERROR) ||
           (native_code >= CER_MIN_ERROR && native_code <= CER_MAX_ERROR) || session_ended(native_code);
}

void free_stored_result(void* result_value) noexcept {
    mysql_free_result(static_cast<st_mysql_res*>(result_value));
}

mariadb_parameter_traits inspect_mariadb_parameters(std::span<const db_value> params) noexcept {
    mariadb_parameter_traits result{};
    for (const auto& param : params) {
        if (param.type() != db_value_type::string) {
            continue;
        }
        const auto value = param.text();
        for (auto backslash = value.find('\\'); backslash != std::string_view::npos;
            backslash = value.find('\\', backslash + 1)) {
            result.has_backslash_ = true;
            if (backslash != 0 && static_cast<unsigned char>(value[backslash - 1]) > 0x7f) {
                result.has_ambiguous_backslash_ = true;
                return result;
            }
        }
    }
    return result;
}

bool mariadb_escape_needs_charset(const st_mysql& connection, bool has_ambiguous_backslash) noexcept {
    return has_ambiguous_backslash && (connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) == 0;
}

const MARIADB_CHARSET_INFO& mariadb_queried_charset(MYSQL_RES& result) {
    auto* row = mysql_fetch_row(&result);
    const auto* lengths = row == nullptr ? nullptr : mysql_fetch_lengths(&result);
    if (row == nullptr || row[0] == nullptr || lengths == nullptr || mysql_num_fields(&result) != 1) {
        throw std::invalid_argument("MariaDB did not name the session's character set");
    }
    const std::string name(row[0], lengths[0]);
    const auto* charset = mariadb_get_charset_by_name(name.c_str());
    if (charset == nullptr) {
        throw std::invalid_argument(
            (std::string("MariaDB session character set ").append(name).append(" is unknown to the client library, so a string parameter with a backslash "
                                                                               "cannot be escaped for it"))
                .c_str());
    }
    return *charset;
}

const MARIADB_CHARSET_INFO& mariadb_utf8mb4_charset() noexcept {
    // libmariadb's table of character sets is static and always has it.
    static const MARIADB_CHARSET_INFO& charset = *mariadb_get_charset_by_name("utf8mb4");
    return charset;
}

void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::string& output, const MARIADB_CHARSET_INFO* charset) {
    validate_mariadb_sql_length(sql.size());
    const bool backslash_escapes = (connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) == 0;
    std::size_t size_hint = sql.size();
    static constexpr std::size_t inline_placeholder_capacity = 32;
    std::array<std::size_t, inline_placeholder_capacity> inline_placeholders;
    std::size_t scan_offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto placeholder = find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        if (i < inline_placeholder_capacity) {
            inline_placeholders[i] = placeholder;
        }
        scan_offset = placeholder + 1;
        if (param.type() == db_value_type::string) {
            const auto value = param.text();
            if (value.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("MariaDB string parameter must not contain NUL bytes");
            }
            if (backslash_escapes && charset == nullptr && ambiguous_backslash(value)) {
                throw std::logic_error(
                    "MariaDB string parameter needs the session's character set to be escaped");
            }
        }
        const auto literal_size_hint = value_literal_size_hint(param);
        if (size_hint > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
            throw std::length_error("MariaDB SQL size calculation overflowed");
        }
        size_hint += literal_size_hint;
        validate_mariadb_sql_length(size_hint);
    }
    if (find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }
    output.clear();
    output.reserve(size_hint);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto placeholder = i < inline_placeholder_capacity
                                     ? inline_placeholders[i]
                                     : find_sql_syntax_byte(sql, '?', backslash_escapes, offset);
        output.append(sql.data() + offset, placeholder - offset);
        append_value_literal(output, params[i], backslash_escapes, charset);
        offset = placeholder + 1;
    }
    output.append(sql.data() + offset, sql.size() - offset);
    validate_mariadb_sql_length(output.size());
}

mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept {
    // A backslash ends a literal in one mode and escapes in the other; a
    // statement whose two readings disagree on its placeholders is not
    // prepared, and either reading's integer placeholders count.
    const auto escaped = read_mariadb_statement(sql, true);
    if (sql.find('\\') == std::string_view::npos) {
        return escaped.traits_;
    }
    const auto plain = read_mariadb_statement(sql, false);
    if (!escaped.traits_.preparable_ || !plain.traits_.preparable_ || escaped.placeholders_ != plain.placeholders_) {
        return {false, 0};
    }
    return {true, escaped.traits_.integer_params_ | plain.traits_.integer_params_};
}

mariadb_statement_classification classify_mariadb_statement(std::string_view sql) noexcept {
    mariadb_statement_classification result{};
    const auto leading = leading_sql_token(sql, true);
    if (leading.kind_ == sql_token::kind::hidden) {
        result.exclusive_ = true;
        result.calls_routine_ = true;
    } else if (leading.kind_ == sql_token::kind::word) {
        result.exclusive_ = sql_keyword(leading.text_, "load");
        result.calls_routine_ = sql_keyword(leading.text_, "call");
    }
    constexpr std::string_view tracking_name = "session_track";
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char ch = sql[i];
        if (ch == '\\') {
            result.has_backslash_ = true;
            if (result.names_session_tracking_) {
                break;
            }
        } else if (ch == '_' && !result.names_session_tracking_ && i >= 7 &&
                   sql.size() - (i - 7) >= tracking_name.size() &&
                   http_ascii_equals_ignore_case(sql.substr(i - 7, tracking_name.size()), tracking_name)) {
            result.names_session_tracking_ = true;
            if (result.has_backslash_) {
                break;
            }
        }
    }
    return result;
}

}  // namespace ruvia::detail
