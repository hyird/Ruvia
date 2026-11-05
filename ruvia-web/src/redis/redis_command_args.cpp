#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/redis_owned_command.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_protocol.h"
#include "redis/redis_types_access.h"
#include "redis/redis_utils.h"

namespace ruvia::detail {

redis_owned_command own_redis_args(
    std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    return make_owned_redis_command(resource, args);
}

redis_owned_command own_redis_args(std::string_view first,
    std::span<const std::string_view> rest, std::pmr::memory_resource* resource) {
    return make_owned_redis_command(resource, first, rest);
}

redis_owned_command make_owned_redis_command(
    std::pmr::memory_resource* resource, std::span<const std::string_view> args) {
    redis_owned_command command{std::pmr::string(pmr_resource_or_default(resource))};
    append_resp_command(command.wire_, args);
    return command;
}

redis_owned_command make_owned_redis_command(std::pmr::memory_resource* resource,
    std::string_view first, std::span<const std::string_view> rest) {
    redis_owned_command command{std::pmr::string(pmr_resource_or_default(resource))};
    append_resp_command(command.wire_, first, rest);
    return command;
}

redis_owned_command make_redis_integer_command(std::string_view command,
    std::string_view key, std::int64_t value, std::pmr::memory_resource* resource) {
    std::array<char, redis_int_scratch_size> value_scratch{};
    const std::string_view args[]{command, key, format_redis_int(value_scratch, value)};
    return make_owned_redis_command(resource, args);
}

redis_owned_command make_redis_range_command(std::string_view command,
    std::string_view key, std::int64_t start, std::int64_t stop, std::pmr::memory_resource* resource,
    bool with_scores) {
    std::array<char, redis_int_scratch_size> start_scratch{};
    std::array<char, redis_int_scratch_size> stop_scratch{};
    const std::string_view args[]{
        command,
        key,
        format_redis_int(start_scratch, start),
        format_redis_int(stop_scratch, stop),
        "WITHSCORES",
    };
    return make_owned_redis_command(
        resource, std::span<const std::string_view>(args, with_scores ? 5U : 4U));
}

redis_owned_command make_redis_zadd_command(std::string_view key, double score,
    std::string_view member, std::pmr::memory_resource* resource) {
    std::array<char, redis_score_scratch_size> score_scratch{};
    const std::string_view args[]{"ZADD", key, format_redis_score(score_scratch, score), member};
    return make_owned_redis_command(resource, args);
}

redis_owned_command own_redis_args(
    std::initializer_list<std::string_view> args, std::pmr::memory_resource* resource) {
    return make_owned_redis_command(
        resource, std::span<const std::string_view>(args.begin(), args.size()));
}

std::pmr::string redis_milliseconds_string(
    std::chrono::milliseconds ttl, std::pmr::memory_resource* resource) {
    return redis_int_string(static_cast<std::int64_t>(ttl.count()), resource);
}

redis_owned_command redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys, std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis command requires at least one key");
    }
    return make_owned_redis_command(resource, command, keys);
}

redis_owned_command redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource) {
    if (items.empty()) {
        throw std::invalid_argument("redis mset requires at least one item");
    }
    if (items.size() > (std::numeric_limits<std::size_t>::max() - 1U) / 2U) {
        throw std::length_error("redis RESP command is too large");
    }
    const auto arg_count = items.size() * 2U + 1U;
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(4U));
    for (const auto& [key, value] : items) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(key.size()));
        add_resp_serialized_size(size, resp_bulk_serialized_size(value.size()));
    }
    redis_owned_command command{std::pmr::string(pmr_resource_or_default(resource))};
    command.wire_.reserve(size);
    append_resp_header(command.wire_, arg_count);
    append_resp_bulk(command.wire_, "MSET");
    for (const auto& [key, value] : items) {
        append_resp_bulk(command.wire_, key);
        append_resp_bulk(command.wire_, value);
    }
    return command;
}

redis_owned_command redis_set_args(std::string_view key, std::string_view value,
    const redis_set_options& options, std::pmr::memory_resource* resource) {
    std::string_view condition_token;
    if (options.condition_) {
        switch (*options.condition_) {
            case redis_set_condition::if_absent:
                condition_token = "NX";
                break;
            case redis_set_condition::if_present:
                condition_token = "XX";
                break;
            default:
                throw std::invalid_argument("redis set condition is invalid");
        }
    }
    const bool returns_previous = redis_set_returns_previous(options.previous_value_);
    const auto* duration = options.expiration_ ? options.expiration_->duration() : nullptr;
    const bool keeps_existing = options.expiration_ && options.expiration_->keeps_existing();
    std::array<char, redis_int_scratch_size> duration_scratch{};
    std::array<std::string_view, 8> args{};
    std::size_t count = 0;
    args[count++] = "SET";
    args[count++] = key;
    args[count++] = value;
    if (duration != nullptr) {
        args[count++] = "PX";
        args[count++] = format_redis_int(duration_scratch, static_cast<std::int64_t>(duration->count()));
    }
    if (!condition_token.empty()) {
        args[count++] = condition_token;
    }
    if (returns_previous) {
        args[count++] = "GET";
    }
    if (keeps_existing) {
        args[count++] = "KEEPTTL";
    }
    return make_owned_redis_command(resource, std::span<const std::string_view>(args.data(), count));
}

redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    if (fields_value.size() > (std::numeric_limits<std::size_t>::max() - 2U) / 2U) {
        throw std::length_error("redis RESP command is too large");
    }
    const auto arg_count = fields_value.size() * 2U + 2U;
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(4U));
    add_resp_serialized_size(size, resp_bulk_serialized_size(key.size()));
    for (const auto& [field, value] : fields_value) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(field.size()));
        add_resp_serialized_size(size, resp_bulk_serialized_size(value.size()));
    }
    redis_owned_command command{std::pmr::string(pmr_resource_or_default(resource))};
    command.wire_.reserve(size);
    append_resp_header(command.wire_, arg_count);
    append_resp_bulk(command.wire_, "HSET");
    append_resp_bulk(command.wire_, key);
    for (const auto& [field, value] : fields_value) {
        append_resp_bulk(command.wire_, field);
        append_resp_bulk(command.wire_, value);
    }
    return command;
}

redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    if (fields_value.size() > std::numeric_limits<std::size_t>::max() - 2U) {
        throw std::length_error("redis RESP command is too large");
    }
    const auto arg_count = fields_value.size() + 2U;
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(command.size()));
    add_resp_serialized_size(size, resp_bulk_serialized_size(key.size()));
    for (const auto field : fields_value) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(field.size()));
    }
    redis_owned_command owned{std::pmr::string(pmr_resource_or_default(resource))};
    owned.wire_.reserve(size);
    append_resp_header(owned.wire_, arg_count);
    append_resp_bulk(owned.wire_, command);
    append_resp_bulk(owned.wire_, key);
    for (const auto field : fields_value) {
        append_resp_bulk(owned.wire_, field);
    }
    return owned;
}

redis_owned_command redis_eval_args(std::string_view command, std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> argv,
    std::pmr::memory_resource* resource) {
    if (keys.size() > std::numeric_limits<std::size_t>::max() - 3U ||
        argv.size() > std::numeric_limits<std::size_t>::max() - 3U - keys.size()) {
        throw std::length_error("redis RESP command is too large");
    }
    const auto arg_count = 3U + keys.size() + argv.size();
    std::array<char, redis_int_scratch_size> key_count_scratch{};
    const auto key_count = format_redis_int(key_count_scratch, static_cast<std::int64_t>(keys.size()));
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(command.size()));
    add_resp_serialized_size(size, resp_bulk_serialized_size(script.size()));
    add_resp_serialized_size(size, resp_bulk_serialized_size(key_count.size()));
    for (const auto key : keys) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(key.size()));
    }
    for (const auto arg : argv) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(arg.size()));
    }
    redis_owned_command owned{std::pmr::string(pmr_resource_or_default(resource))};
    owned.wire_.reserve(size);
    append_resp_header(owned.wire_, arg_count);
    append_resp_bulk(owned.wire_, command);
    append_resp_bulk(owned.wire_, script);
    append_resp_bulk(owned.wire_, key_count);
    for (const auto key : keys) {
        append_resp_bulk(owned.wire_, key);
    }
    for (const auto arg : argv) {
        append_resp_bulk(owned.wire_, arg);
    }
    return owned;
}

redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait,
    std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    if (keys.size() > std::numeric_limits<std::size_t>::max() - 2U) {
        throw std::length_error("redis RESP command is too large");
    }
    std::array<char, 32> timeout_scratch{};
    std::string_view timeout_text = "0";
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto milliseconds = duration->count();
        const auto seconds_text = format_redis_int(
            std::span<char, redis_int_scratch_size>(timeout_scratch.data(), redis_int_scratch_size),
            milliseconds / 1000);
        auto len = seconds_text.size();
        const auto remainder = milliseconds % 1000;
        if (remainder != 0) {
            timeout_scratch[len++] = '.';
            timeout_scratch[len++] = static_cast<char>('0' + remainder / 100);
            timeout_scratch[len++] = static_cast<char>('0' + (remainder / 10) % 10);
            timeout_scratch[len++] = static_cast<char>('0' + remainder % 10);
        }
        timeout_text = std::string_view(timeout_scratch.data(), len);
    }
    const auto arg_count = keys.size() + 2U;
    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(command.size()));
    for (const auto key : keys) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(key.size()));
    }
    add_resp_serialized_size(size, resp_bulk_serialized_size(timeout_text.size()));
    redis_owned_command owned{std::pmr::string(pmr_resource_or_default(resource))};
    owned.wire_.reserve(size);
    append_resp_header(owned.wire_, arg_count);
    append_resp_bulk(owned.wire_, command);
    for (const auto key : keys) {
        append_resp_bulk(owned.wire_, key);
    }
    append_resp_bulk(owned.wire_, timeout_text);
    return owned;
}

redis_owned_command redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options, std::pmr::memory_resource* resource) {
    if (group.empty() || consumer.empty()) {
        throw std::invalid_argument("redis xreadgroup requires a group and consumer");
    }
    if (streams.empty()) {
        throw std::invalid_argument("redis xreadgroup requires at least one stream");
    }
    for (const auto& stream : streams) {
        if (stream.stream_.empty() || stream.id_.empty()) {
            throw std::invalid_argument("redis xreadgroup stream and id must not be empty");
        }
    }
    const auto use_no_ack = redis_x_read_group_uses_no_ack(options.acknowledgement_);
    const std::size_t extra_args = 5U + (options.count_.has_value() ? 2U : 0U) +
                                   (options.block_.has_value() ? 2U : 0U) + (use_no_ack ? 1U : 0U);
    if (streams.size() > (std::numeric_limits<std::size_t>::max() - extra_args) / 2U) {
        throw std::length_error("redis RESP command is too large");
    }
    const auto arg_count = extra_args + streams.size() * 2U;
    std::array<char, http_max_unsigned_decimal_size> count_scratch{};
    std::string_view count_text;
    if (options.count_.has_value()) {
        count_text = http_format_unsigned(count_scratch, options.count_->value());
    }
    std::array<char, redis_int_scratch_size> block_scratch{};
    std::string_view block_text = "0";
    if (options.block_.has_value()) {
        if (const auto duration = options.block_->duration(); duration.has_value()) {
            block_text = format_redis_int(block_scratch, static_cast<std::int64_t>(duration->count()));
        }
    }

    std::size_t size = resp_header_serialized_size(arg_count);
    add_resp_serialized_size(size, resp_bulk_serialized_size(10U));  // XREADGROUP
    add_resp_serialized_size(size, resp_bulk_serialized_size(5U));   // GROUP
    add_resp_serialized_size(size, resp_bulk_serialized_size(group.size()));
    add_resp_serialized_size(size, resp_bulk_serialized_size(consumer.size()));
    if (options.count_.has_value()) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(5U));  // COUNT
        add_resp_serialized_size(size, resp_bulk_serialized_size(count_text.size()));
    }
    if (options.block_.has_value()) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(5U));  // BLOCK
        add_resp_serialized_size(size, resp_bulk_serialized_size(block_text.size()));
    }
    if (use_no_ack) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(5U));  // NOACK
    }
    add_resp_serialized_size(size, resp_bulk_serialized_size(7U));  // STREAMS
    for (const auto& stream : streams) {
        add_resp_serialized_size(size, resp_bulk_serialized_size(stream.stream_.size()));
        add_resp_serialized_size(size, resp_bulk_serialized_size(stream.id_.size()));
    }

    redis_owned_command owned{std::pmr::string(pmr_resource_or_default(resource))};
    owned.wire_.reserve(size);
    append_resp_header(owned.wire_, arg_count);
    append_resp_bulk(owned.wire_, "XREADGROUP");
    append_resp_bulk(owned.wire_, "GROUP");
    append_resp_bulk(owned.wire_, group);
    append_resp_bulk(owned.wire_, consumer);
    if (options.count_.has_value()) {
        append_resp_bulk(owned.wire_, "COUNT");
        append_resp_bulk(owned.wire_, count_text);
    }
    if (options.block_.has_value()) {
        append_resp_bulk(owned.wire_, "BLOCK");
        append_resp_bulk(owned.wire_, block_text);
    }
    if (use_no_ack) {
        append_resp_bulk(owned.wire_, "NOACK");
    }
    append_resp_bulk(owned.wire_, "STREAMS");
    for (const auto& stream : streams) {
        append_resp_bulk(owned.wire_, stream.stream_.view());
    }
    for (const auto& stream : streams) {
        append_resp_bulk(owned.wire_, stream.id_.view());
    }
    return owned;
}
}  // namespace ruvia::detail
