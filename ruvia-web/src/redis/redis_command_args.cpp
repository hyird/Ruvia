#include <optional>
#include <stdexcept>

#include "ruvia/web/detail/redis/redis_owned_command.h"
#include "ruvia/web/detail/redis/redis_utils.h"

#include "redis/redis_handle_helpers.h"
#include "redis/redis_types_access.h"

namespace ruvia::detail {

namespace {

std::pmr::vector<std::pmr::string> own_redis_args_impl(std::optional<std::string_view> first,
    std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> owned(resource);
    owned.reserve(args.size() + static_cast<std::size_t>(first.has_value()));
    if (first.has_value()) {
        emplace_redis_string(owned, *first);
    }
    for (const auto arg : args) {
        emplace_redis_string(owned, arg);
    }
    return owned;
}

}  // namespace

std::pmr::vector<std::pmr::string> own_redis_args(
    std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    return own_redis_args_impl(std::nullopt, args, resource);
}

std::pmr::vector<std::pmr::string> own_redis_args(std::string_view first,
    std::span<const std::string_view> rest, std::pmr::memory_resource* resource) {
    return own_redis_args_impl(first, rest, resource);
}

redis_owned_command make_owned_redis_command(
    std::pmr::memory_resource* resource, std::span<const std::string_view> args) {
    return {own_redis_args(args, resource)};
}

redis_owned_command make_owned_redis_command(std::pmr::memory_resource* resource,
    std::string_view first, std::span<const std::string_view> rest) {
    return {own_redis_args(first, rest, resource)};
}

std::pmr::vector<std::pmr::string> own_redis_args(
    std::initializer_list<std::string_view> args, std::pmr::memory_resource* resource) {
    return own_redis_args(std::span<const std::string_view>(args.begin(), args.size()), resource);
}

std::pmr::string redis_seconds_string(std::chrono::seconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    append_redis_number(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

std::pmr::string redis_milliseconds_string(
    std::chrono::milliseconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    append_redis_number(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

std::pmr::string redis_cursor_string(
    std::optional<redis_scan_cursor> cursor_value, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    append_redis_number(output, cursor_value.has_value() ? redis_types_access::cursor_value(*cursor_value) : 0);
    return output;
}

std::pmr::vector<std::pmr::string> redis_command_with_keys(std::string_view command,
    std::span<const std::string_view> keys, std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis command requires at least one key");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 1);
    emplace_redis_string(args, command);
    for (const auto key : keys) {
        emplace_redis_string(args, key);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redis_mset_args(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource) {
    if (items.empty()) {
        throw std::invalid_argument("redis mset requires at least one item");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(items.size() * 2 + 1);
    emplace_redis_string(args, "MSET");
    for (const auto& [key, value] : items) {
        emplace_redis_string(args, key);
        emplace_redis_string(args, value);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redis_set_args(std::string_view key, std::string_view value,
    const redis_set_options& options, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(8);
    emplace_redis_string(args, "SET");
    emplace_redis_string(args, key);
    emplace_redis_string(args, value);
    if (options.expiration_) {
        if (const auto* duration = options.expiration_->duration()) {
            emplace_redis_string(args, "PX");
            args.emplace_back(redis_milliseconds_string(*duration, resource));
        }
    }
    if (options.condition_) {
        switch (*options.condition_) {
            case redis_set_condition::if_absent:
                emplace_redis_string(args, "NX");
                break;
            case redis_set_condition::if_present:
                emplace_redis_string(args, "XX");
                break;
            default:
                throw std::invalid_argument("redis set condition is invalid");
        }
    }
    if (redis_set_returns_previous(options.previous_value_)) {
        emplace_redis_string(args, "GET");
    }
    if (options.expiration_ && options.expiration_->keeps_existing()) {
        emplace_redis_string(args, "KEEPTTL");
    }
    return args;
}

std::pmr::vector<std::pmr::string> redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields_value.size() * 2 + 2);
    emplace_redis_string(args, "HSET");
    emplace_redis_string(args, key);
    for (const auto& [field, value] : fields_value) {
        emplace_redis_string(args, field);
        emplace_redis_string(args, value);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields_value.size() + 2);
    emplace_redis_string(args, command);
    emplace_redis_string(args, key);
    for (const auto field : fields_value) {
        emplace_redis_string(args, field);
    }
    return args;
}

void append_redis_scan_options(std::pmr::vector<std::pmr::string>& args,
    const redis_scan_options& options, std::pmr::memory_resource* resource) {
    if (options.count_.has_value() && *options.count_ == 0) {
        throw std::invalid_argument("configured redis scan count must be greater than zero");
    }
    if (!options.match_.empty()) {
        emplace_redis_string(args, "MATCH");
        emplace_redis_string(args, options.match_);
    }
    if (options.count_.has_value()) {
        emplace_redis_string(args, "COUNT");
        std::pmr::string count(resource);
        append_redis_number(count, *options.count_);
        args.emplace_back(std::move(count));
    }
}

std::pmr::vector<std::pmr::string> redis_eval_args(std::string_view command, std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> argv,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(3 + keys.size() + argv.size());
    emplace_redis_string(args, command);
    emplace_redis_string(args, script);
    args.emplace_back(redis_int_string(static_cast<std::int64_t>(keys.size()), resource));
    for (const auto key : keys) {
        emplace_redis_string(args, key);
    }
    for (const auto arg : argv) {
        emplace_redis_string(args, arg);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait,
    std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 2);
    emplace_redis_string(args, command);
    for (const auto key : keys) {
        emplace_redis_string(args, key);
    }
    std::pmr::string timeout(resource);
    if (const auto duration = wait.duration(); duration.has_value()) {
        const auto milliseconds = duration->count();
        append_redis_number(timeout, milliseconds / 1000);
        const auto remainder = milliseconds % 1000;
        if (remainder != 0) {
            timeout.push_back('.');
            timeout.push_back(static_cast<char>('0' + remainder / 100));
            timeout.push_back(static_cast<char>('0' + (remainder / 10) % 10));
            timeout.push_back(static_cast<char>('0' + remainder % 10));
        }
    } else {
        timeout.push_back('0');
    }
    args.emplace_back(std::move(timeout));
    return args;
}

std::pmr::vector<std::pmr::string> redis_x_read_group_args(std::string_view group,
    std::string_view consumer, std::span<const redis_stream_read_view> streams,
    const redis_x_read_group_options& options, std::pmr::memory_resource* resource) {
    if (group.empty() || consumer.empty()) {
        throw std::invalid_argument("redis xreadgroup requires a group and consumer");
    }
    if (streams.empty()) {
        throw std::invalid_argument("redis xreadgroup requires at least one stream");
    }
    if (options.count_.has_value() && *options.count_ == 0) {
        throw std::invalid_argument("redis xreadgroup count must be greater than zero");
    }
    for (const auto& stream : streams) {
        if (stream.stream_.empty() || stream.id_.empty()) {
            throw std::invalid_argument("redis xreadgroup stream and id must not be empty");
        }
    }

    std::pmr::vector<std::pmr::string> args(resource);
    const auto use_no_ack = redis_x_read_group_uses_no_ack(options.acknowledgement_);
    args.reserve(6 + streams.size() * 2 + (options.count_.has_value() ? 2 : 0) +
                 (options.block_.has_value() ? 2 : 0) + (use_no_ack ? 1 : 0));
    emplace_redis_string(args, "XREADGROUP");
    emplace_redis_string(args, "GROUP");
    emplace_redis_string(args, group);
    emplace_redis_string(args, consumer);
    if (options.count_.has_value()) {
        emplace_redis_string(args, "COUNT");
        std::pmr::string count(resource);
        append_redis_number(count, *options.count_);
        args.emplace_back(std::move(count));
    }
    if (options.block_.has_value()) {
        emplace_redis_string(args, "BLOCK");
        if (const auto duration = options.block_->duration(); duration.has_value()) {
            args.emplace_back(redis_milliseconds_string(*duration, resource));
        } else {
            emplace_redis_string(args, "0");
        }
    }
    if (use_no_ack) {
        emplace_redis_string(args, "NOACK");
    }
    emplace_redis_string(args, "STREAMS");
    for (const auto& stream : streams) {
        emplace_redis_string(args, stream.stream_.view());
    }
    for (const auto& stream : streams) {
        emplace_redis_string(args, stream.id_.view());
    }
    return args;
}

}  // namespace ruvia::detail
