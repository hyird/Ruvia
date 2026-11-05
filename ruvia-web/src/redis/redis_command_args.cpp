#include "redis/redis_command_args.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_number_format.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool redis_x_read_group_uses_no_ack(redis_x_read_group_acknowledgement_policy policy) {
    switch (policy) {
        case redis_x_read_group_acknowledgement_policy::track_pending:
            return false;
        case redis_x_read_group_acknowledgement_policy::no_ack:
            return true;
        default:
            throw std::invalid_argument("redis xreadgroup acknowledgement policy is invalid");
    }
}

}  // namespace

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

redis_owned_command make_owned_redis_command(
    std::pmr::memory_resource* resource, std::initializer_list<std::string_view> args) {
    return make_owned_redis_command(resource, std::span<const std::string_view>(args.begin(), args.size()));
}

redis_owned_command make_redis_integer_command(std::string_view command,
    std::string_view key, std::int64_t value, std::pmr::memory_resource* resource) {
    std::array<char, redis_int_scratch_size> value_scratch{};
    return make_owned_redis_command(resource, {command, key, format_redis_int(value_scratch, value)});
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
    return make_owned_redis_command(resource, {"ZADD", key, format_redis_score(score_scratch, score), member});
}

bool redis_set_returns_previous(redis_set_previous_value_policy policy) {
    switch (policy) {
        case redis_set_previous_value_policy::discard:
            return false;
        case redis_set_previous_value_policy::return_value:
            return true;
        default:
            throw std::invalid_argument("redis set previous value policy is invalid");
    }
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
    return encode_redis_command(resource, [&](const auto& emit) {
        emit("MSET");
        for (const auto& [key, value] : items) {
            emit(key);
            emit(value);
        }
    });
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
    const std::string_view duration_text =
        duration != nullptr ? format_redis_int(duration_scratch, static_cast<std::int64_t>(duration->count()))
                            : std::string_view();
    return encode_redis_command(resource, [&](const auto& emit) {
        emit("SET");
        emit(key);
        emit(value);
        if (duration != nullptr) {
            emit("PX");
            emit(duration_text);
        }
        if (!condition_token.empty()) {
            emit(condition_token);
        }
        if (returns_previous) {
            emit("GET");
        }
        if (keeps_existing) {
            emit("KEEPTTL");
        }
    });
}

redis_owned_command redis_hset_fields_args(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    return encode_redis_command(resource, [&](const auto& emit) {
        emit("HSET");
        emit(key);
        for (const auto& [field, value] : fields_value) {
            emit(field);
            emit(value);
        }
    });
}

redis_owned_command redis_command_with_key_fields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields_value,
    std::pmr::memory_resource* resource) {
    if (fields_value.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    return encode_redis_command(resource, [&](const auto& emit) {
        emit(command);
        emit(key);
        for (const auto field : fields_value) {
            emit(field);
        }
    });
}

redis_owned_command redis_eval_args(std::string_view command, std::string_view script,
    std::span<const std::string_view> keys, std::span<const std::string_view> argv,
    std::pmr::memory_resource* resource) {
    std::array<char, http_max_unsigned_decimal_size> key_count_scratch{};
    const auto key_count = http_format_unsigned(key_count_scratch, keys.size());
    return encode_redis_command(resource, [&](const auto& emit) {
        emit(command);
        emit(script);
        emit(key_count);
        for (const auto key : keys) {
            emit(key);
        }
        for (const auto arg : argv) {
            emit(arg);
        }
    });
}

redis_owned_command redis_blocking_pop_args(std::string_view command,
    std::span<const std::string_view> keys, redis_block_wait wait,
    std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    // Seconds with millisecond precision: digits, '.', three fraction digits.
    std::array<char, redis_int_scratch_size + 4> timeout_scratch{};
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
    return encode_redis_command(resource, [&](const auto& emit) {
        emit(command);
        for (const auto key : keys) {
            emit(key);
        }
        emit(timeout_text);
    });
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
    return encode_redis_command(resource, [&](const auto& emit) {
        emit("XREADGROUP");
        emit("GROUP");
        emit(group);
        emit(consumer);
        if (options.count_.has_value()) {
            emit("COUNT");
            emit(count_text);
        }
        if (options.block_.has_value()) {
            emit("BLOCK");
            emit(block_text);
        }
        if (use_no_ack) {
            emit("NOACK");
        }
        emit("STREAMS");
        for (const auto& stream : streams) {
            emit(stream.stream_.view());
        }
        for (const auto& stream : streams) {
            emit(stream.id_.view());
        }
    });
}

}  // namespace ruvia::detail
