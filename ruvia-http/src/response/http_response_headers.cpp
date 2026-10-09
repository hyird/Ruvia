#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"
#include "response/http_response_static_headers.h"

namespace ruvia {

http_response_header http_response_headers::make_owned_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto total = name.size() + value.size();
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes_value, name.data(), name.size());
        std::memcpy(bytes_value + name.size(), value.data(), value.size());
    }
    return detail::make_response_header(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value.size()), known_bit, true);
}

http_response_header http_response_headers::make_uninitialized_header(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = static_cast<char*>(resource_->allocate(total, 1));
        std::memcpy(bytes_value, name.data(), name.size());
    }
    return detail::make_response_header(bytes_value, static_cast<std::uint32_t>(name.size()),
        static_cast<std::uint32_t>(value_size), known_bit, true);
}

std::optional<http_response_header> http_response_headers::make_static_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (known_bit == 0) {
        return std::nullopt;
    }
    auto header_value = detail::builtin_static_response_header(known_bit, value);
    if (!header_value || header_value->name() != name) {
        return std::nullopt;
    }
    return header_value;
}

void http_response_headers::release_header(http_response_header& header_value) noexcept {
    if (header_value.owned_ && header_value.bytes_ != nullptr) {
        resource_->deallocate(const_cast<char*>(header_value.bytes_),
            static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_, 1);
    }
    header_value.bytes_ = nullptr;
    header_value.name_size_ = 0;
    header_value.value_size_ = 0;
    header_value.known_bit_ = 0;
    header_value.owned_ = false;
    header_value.append_ = false;
}

http_response_header& http_response_headers::append_header(http_response_header header_value) {
    // `header` may own a separately allocated name/value block.  The vector
    // stores the small descriptor by value, so an exception while spilling or
    // appending must release that block here; otherwise a failed PMR
    // allocation leaks the response header and leaves the next retry with a
    // different ownership picture.
    try {
        if (!spilled_ && size_ == inline_capacity) {
            spill(size_ + 1);
        }
        if (!spilled_) {
            auto* target = inline_data() + size_;
            *target = header_value;
            ++size_;
            return *target;
        }
        heap_.push_back(header_value);
        return heap_.back();
    } catch (...) {
        release_header(header_value);
        throw;
    }
}

http_response_header& http_response_headers::add(
    std::string_view name, std::string_view value, std::uint32_t known_bit) {
    return append_header(make_owned_header(name, value, known_bit));
}

http_response_header& http_response_headers::add_stable_view(
    std::string_view name, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto static_header = make_static_header(name, value, known_bit);
    return append_header(static_header ? *static_header : make_owned_header(name, value, known_bit));
}

http_response_header& http_response_headers::add_uninitialized_value(
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    return append_header(make_uninitialized_header(name, value_size, known_bit));
}

http_response_header& http_response_headers::assign_uninitialized_value(http_response_header& header_value,
    std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    if (header_value.owned_ && header_value.bytes_ != nullptr && !detail::response_header_storage_overlaps(header_value, name) &&
        total == static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        auto* const bytes_value = const_cast<char*>(header_value.bytes_);
        std::memcpy(bytes_value, name.data(), name.size());
        header_value.name_size_ = static_cast<std::uint32_t>(name.size());
        header_value.value_size_ = static_cast<std::uint32_t>(value_size);
        header_value.known_bit_ = known_bit;
        header_value.append_ = false;
        return header_value;
    }

    const auto replacement = make_uninitialized_header(name, value_size, known_bit);
    release_header(header_value);
    header_value = replacement;
    return header_value;
}

bool http_response_headers::try_assign_owned_in_place(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit) noexcept {
    if (!detail::response_header_storage_size_fits(name.size(), value.size())) {
        return false;
    }
    const auto total = name.size() + value.size();
    if (!header_value.owned_ || header_value.bytes_ == nullptr || detail::response_header_storage_overlaps(header_value, name) ||
        detail::response_header_storage_overlaps(header_value, value) ||
        total != static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        return false;
    }
    auto* const bytes_value = const_cast<char*>(header_value.bytes_);
    std::memcpy(bytes_value, name.data(), name.size());
    std::memcpy(bytes_value + name.size(), value.data(), value.size());
    header_value.name_size_ = static_cast<std::uint32_t>(name.size());
    header_value.value_size_ = static_cast<std::uint32_t>(value.size());
    header_value.known_bit_ = known_bit;
    header_value.append_ = false;
    return true;
}

void http_response_headers::assign(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    if (try_assign_owned_in_place(header_value, name, value, known_bit)) {
        return;
    }
    const auto replacement = make_owned_header(name, value, known_bit);
    release_header(header_value);
    header_value = replacement;
}

void http_response_headers::assign_stable_view(http_response_header& header_value, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto static_header = make_static_header(name, value, known_bit);
    if (static_header) {
        release_header(header_value);
        header_value = *static_header;
        return;
    }

    if (try_assign_owned_in_place(header_value, name, value, known_bit)) {
        return;
    }
    const auto replacement = make_owned_header(name, value, known_bit);
    release_header(header_value);
    header_value = replacement;
}

}  // namespace ruvia
