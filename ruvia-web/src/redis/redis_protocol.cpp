#include "redis/redis_protocol.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "redis/redis_types_access.h"
#include "redis/redis_utils.h"

namespace ruvia::detail {
namespace {

template <typename args_type>
[[nodiscard]] std::size_t resp_serialized_size(const args_type& args) {
    const auto decimal_size = [](std::size_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max()) {
            throw std::length_error("redis RESP length is too large");
        }
        return http_unsigned_decimal_size(static_cast<std::uint64_t>(value));
    };
    const auto add_size = [](std::size_t& current, std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - current) {
            throw std::length_error("redis RESP command is too large");
        }
        current += amount;
    };

    std::size_t size = 0;
    add_size(size, 1);
    add_size(size, decimal_size(args.size()));
    add_size(size, 2);
    for (const auto& arg : args) {
        const auto bytes_value = static_cast<std::size_t>(arg.size());
        add_size(size, 1);
        add_size(size, decimal_size(bytes_value));
        add_size(size, 2);
        add_size(size, bytes_value);
        add_size(size, 2);
    }
    return size;
}

// Writes the RESP multi-bulk encoding of `args` to `output`. No manual
// reserve: `output` is the per-connection write buffer reused across requests,
// so in steady state it already holds enough capacity, and std::pmr::string's
// geometric growth keeps pipelined appends amortized O(total) bytes (an exact
// per-command reserve would instead force O(n^2) copying for large pipelines).
template <typename args_type>
void serialize_resp_command(std::pmr::string& output, const args_type& args, std::size_t serialized_size) {
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.push_back('*');
    append_redis_number(output, static_cast<std::uint64_t>(args.size()));
    output.append("\r\n", 2);
    for (const auto& arg : args) {
        const auto size = static_cast<std::size_t>(arg.size());
        output.push_back('$');
        append_redis_number(output, static_cast<std::uint64_t>(size));
        output.append("\r\n", 2);
        if (size != 0) {
            output.append(arg.data(), size);
        }
        output.append("\r\n", 2);
    }
}

[[nodiscard]] redis_reply_builder& builder_of(const redisReadTask* task_value) noexcept {
    return *static_cast<redis_reply_builder*>(task_value->privdata);
}

}  // namespace

void append_resp_command(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serialize_resp_command(output, args, resp_serialized_size(args));
}

void append_resp_command(std::pmr::string& output, std::span<const std::pmr::string> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serialize_resp_command(output, args, resp_serialized_size(args));
}

std::size_t resp_command_serialized_size(std::span<const std::string_view> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(std::span<const std::pmr::string> args) {
    return resp_serialized_size(args);
}

const char* hiredis_reader_error(const redisReader& reader_value) noexcept {
    return reader_value.errstr[0] == '\0' ? "redis protocol error" : reader_value.errstr;
}

redisReplyObjectFunctions redis_reply_builder::functions_{
    &redis_reply_builder::create_string,
    &redis_reply_builder::create_array,
    &redis_reply_builder::create_integer,
    &redis_reply_builder::create_double,
    &redis_reply_builder::create_nil,
    &redis_reply_builder::create_bool,
    &redis_reply_builder::free_object,
};

redis_reply_builder::redis_reply_builder(std::optional<std::size_t> max_reply_bytes, std::size_t max_depth) noexcept
    : max_reply_bytes_(max_reply_bytes),
      maximum_elements_(max_reply_bytes.has_value() ? *max_reply_bytes / 3 : std::numeric_limits<std::size_t>::max()),
      remaining_elements_(maximum_elements_),
      max_depth_(max_depth) {}

redisReader* redis_reply_builder::create_reader() {
    auto* reader_value = redisReaderCreateWithFunctions(&functions_);
    if (reader_value == nullptr) {
        throw redis_error(redis_error::code_type::protocol_error, "failed to create redis reader");
    }
    reader_value->privdata = this;
    if (max_reply_bytes_.has_value()) {
        // Rejects an oversized array header before any element arrives.
        // Every element requires at least three RESP bytes (e.g. "+\r\n").
        const auto elements = std::max<std::size_t>(1, *max_reply_bytes_ / 3);
        reader_value->maxelements = static_cast<long long>(
            std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
    }
    reader_ = reader_value;
    return reader_value;
}

void redis_reply_builder::begin(std::pmr::memory_resource* resource) noexcept {
    resource_ = resource;
    remaining_elements_ = maximum_elements_;
    rejection_ = rejection_type::none;
    failure_ = nullptr;
}

void redis_reply_builder::throw_if_rejected() const {
    switch (rejection_) {
        case rejection_type::none:
            return;
        case rejection_type::elements:
            throw redis_error(redis_error::code_type::protocol_error, "redis reply exceeds configured element limit");
        case rejection_type::depth:
            throw redis_error(redis_error::code_type::protocol_error, "redis array nesting is too deep");
        case rejection_type::failed:
            std::rethrow_exception(failure_);
    }
}

redis_value redis_reply_builder::take(void* reply) noexcept {
    auto* root = static_cast<redis_value*>(reply);
    redis_value value(std::move(*root));
    free_object(root);
    return value;
}

// hiredis creates the elements of an array in index order, so each child is
// appended to its parent. Only the innermost open array grows and hiredis
// keeps no pointer to its earlier, completed elements, so a reallocation never
// invalidates a node the reader still refers to. Roots are allocated alone;
// the reader hands them to free_object() or to the caller.
template <typename factory_type>
void* redis_reply_builder::place(const redisReadTask* task_value, factory_type&& factory) noexcept {
    try {
        if (task_value->parent != nullptr) {
            auto& siblings = redis_types_access::elements(*static_cast<redis_value*>(task_value->parent->obj));
            siblings.push_back(factory());
            return &siblings.back();
        }
        auto value = factory();
        std::pmr::polymorphic_allocator<redis_value> allocator(resource_);
        return std::construct_at(allocator.allocate(1), std::move(value));
    } catch (...) {
        rejection_ = rejection_type::failed;
        failure_ = std::current_exception();
        return nullptr;
    }
}

void* redis_reply_builder::create_string(const redisReadTask* task_value, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    std::string_view text(data, size);
    if (task_value->type == REDIS_REPLY_VERB) {
        // The reader has verified the "fmt:" prefix; the value excludes it.
        text.remove_prefix(4);
    }
    return builder.place(task_value, [&] {
        return task_value->type == REDIS_REPLY_ERROR ? redis_types_access::error_value(text, builder.resource_)
                                                     : redis_types_access::string_value(text, builder.resource_);
    });
}

void* redis_reply_builder::create_array(const redisReadTask* task_value, std::size_t elements) noexcept {
    auto& builder = builder_of(task_value);
    std::size_t depth = 0;
    for (auto* parent_value = task_value; parent_value != nullptr; parent_value = parent_value->parent) {
        if (++depth > builder.max_depth_) {
            builder.rejection_ = rejection_type::depth;
            return nullptr;
        }
    }
    if (elements > builder.remaining_elements_) {
        builder.rejection_ = rejection_type::elements;
        return nullptr;
    }
    // Reserve only what the buffered bytes can already describe; a header
    // announcing more elements grows the array as they actually arrive.
    const auto buffered = builder.reader_->len - builder.reader_->pos;
    const auto capacity = std::min(elements, buffered / 3);
    auto* node = builder.place(task_value, [&] { return redis_types_access::array_value(capacity, builder.resource_); });
    if (node != nullptr) {
        builder.remaining_elements_ -= elements;
    }
    return node;
}

void* redis_reply_builder::create_integer(const redisReadTask* task_value, long long value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::integer_value(static_cast<std::int64_t>(value), builder.resource_);
    });
}

void* redis_reply_builder::create_double(const redisReadTask* task_value, double, char* data, std::size_t size) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] {
        return redis_types_access::string_value(std::string_view(data, size), builder.resource_);
    });
}

void* redis_reply_builder::create_nil(const redisReadTask* task_value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] { return redis_types_access::null_value(builder.resource_); });
}

void* redis_reply_builder::create_bool(const redisReadTask* task_value, int value) noexcept {
    auto& builder = builder_of(task_value);
    return builder.place(task_value, [&] { return redis_types_access::integer_value(value == 0 ? 0 : 1, builder.resource_); });
}

void redis_reply_builder::free_object(void* reply) noexcept {
    auto* root = static_cast<redis_value*>(reply);
    std::pmr::polymorphic_allocator<redis_value> allocator(redis_types_access::resource(*root));
    std::destroy_at(root);
    allocator.deallocate(root, 1);
}

}  // namespace ruvia::detail
