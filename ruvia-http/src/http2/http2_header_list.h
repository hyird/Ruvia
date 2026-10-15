#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

struct http2_stored_header_view final {
    std::string_view name_;
    std::string_view value_;
    request_header_kind kind_{request_header_kind::other};
};

class http2_header_list final {
public:
    struct checkpoint_type final {
        std::size_t storage_size_;
        std::size_t overflow_storage_size_;
        std::size_t overflow_field_count_;
        std::uint8_t count_;
        bool using_overflow_storage_;
    };

    explicit http2_header_list(std::pmr::memory_resource* resource = nullptr)
        : http2_header_list(http_resolved_pmr_resource_tag{}, http_pmr_resource_or_default(resource)) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    [[nodiscard]] bool full() const noexcept {
        return count_ == max_http_header_fields;
    }

    void swap(http2_header_list& other) noexcept {
        if (overflow_storage_.get_allocator().resource() !=
            other.overflow_storage_.get_allocator().resource()) {
            std::terminate();
        }
        using std::swap;
        swap(inline_storage_, other.inline_storage_);
        overflow_storage_.swap(other.overflow_storage_);
        swap(inline_fields_, other.inline_fields_);
        overflow_fields_.swap(other.overflow_fields_);
        swap(storage_size_, other.storage_size_);
        swap(count_, other.count_);
        swap(using_overflow_storage_, other.using_overflow_storage_);
    }

    [[nodiscard]] checkpoint_type checkpoint() const noexcept {
        return checkpoint_type{storage_size_, overflow_storage_.size(), overflow_fields_.size(), count_,
            using_overflow_storage_};
    }

    void rollback(checkpoint_type checkpoint) noexcept {
        if (checkpoint.storage_size_ > storage_size_ ||
            checkpoint.overflow_storage_size_ > overflow_storage_.size() ||
            checkpoint.overflow_field_count_ > overflow_fields_.size() || checkpoint.count_ > count_) {
            std::terminate();
        }
        overflow_storage_.resize(checkpoint.overflow_storage_size_);
        overflow_fields_.resize(checkpoint.overflow_field_count_);
        storage_size_ = checkpoint.storage_size_;
        count_ = checkpoint.count_;
        using_overflow_storage_ = checkpoint.using_overflow_storage_;
    }

    [[nodiscard]] http2_stored_header_view at(std::size_t index) const& noexcept {
        const auto& field = index < inline_header_fields
                                ? inline_fields_[index]
                                : overflow_fields_[index - inline_header_fields];
        return http2_stored_header_view{.name_ = view(field.name_offset_, field.name_size_),
            .value_ = view(field.value_offset_, field.value_size_),
            .kind_ = field.kind_};
    }
    [[nodiscard]] http2_stored_header_view at(std::size_t) const&& = delete;

    [[nodiscard]] bool append(
        std::string_view name, std::string_view value, request_header_kind kind) {
        if (full() || name.size() > max_stored_header_view_size ||
            value.size() > max_stored_header_view_size ||
            storage_size_ > max_stored_header_view_size - name.size() ||
            storage_size_ + name.size() > max_stored_header_view_size - value.size()) {
            return false;
        }

        const auto field_bytes = name.size() + value.size();
        if (!using_overflow_storage_ && field_bytes > inline_header_storage_bytes - storage_size_) {
            ensure_overflow_storage(field_bytes);
        }

        const auto name_offset = static_cast<std::uint32_t>(storage_size_);
        append_bytes(name);
        const auto value_offset = static_cast<std::uint32_t>(storage_size_);
        append_bytes(value);

        const auto field = header_field_type{.name_offset_ = name_offset,
            .name_size_ = static_cast<std::uint32_t>(name.size()),
            .value_offset_ = value_offset,
            .value_size_ = static_cast<std::uint32_t>(value.size()),
            .kind_ = kind};

        if (count_ < inline_header_fields) {
            inline_fields_[count_] = field;
        } else {
            if (overflow_fields_.empty()) {
                overflow_fields_.reserve(max_http_header_fields - inline_header_fields);
            }
            overflow_fields_.push_back(field);
        }
        ++count_;
        return true;
    }

private:
    http2_header_list(http_resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : overflow_storage_(resource),
          overflow_fields_(resource) {}

    static constexpr std::size_t inline_header_fields = 16;
    static constexpr std::size_t inline_header_storage_bytes = 512;
    static constexpr std::size_t max_stored_header_view_size =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

    struct header_field_type final {
        std::uint32_t name_offset_{0};
        std::uint32_t name_size_{0};
        std::uint32_t value_offset_{0};
        std::uint32_t value_size_{0};
        request_header_kind kind_{request_header_kind::other};
    };

    [[nodiscard]] std::string_view view(std::uint32_t offset, std::uint32_t size) const noexcept {
        return std::string_view(storage_data() + offset, size);
    }

    [[nodiscard]] const char* storage_data() const noexcept {
        return using_overflow_storage_ ? overflow_storage_.data() : inline_storage_.data();
    }

    void append_bytes(std::string_view value) {
        if (value.empty()) {
            return;
        }
        if (!using_overflow_storage_) {
            std::memcpy(inline_storage_.data() + storage_size_, value.data(), value.size());
            storage_size_ += value.size();
            return;
        }
        overflow_storage_.append(value.data(), value.size());
        storage_size_ += value.size();
    }

    void ensure_overflow_storage(std::size_t additional_bytes) {
        if (using_overflow_storage_) {
            return;
        }
        overflow_storage_.reserve(storage_size_ + additional_bytes);
        overflow_storage_.append(inline_storage_.data(), storage_size_);
        using_overflow_storage_ = true;
    }

    std::array<char, inline_header_storage_bytes> inline_storage_{};
    std::pmr::string overflow_storage_;
    std::array<header_field_type, inline_header_fields> inline_fields_{};
    std::pmr::vector<header_field_type> overflow_fields_;
    std::size_t storage_size_{0};
    std::uint8_t count_{0};
    bool using_overflow_storage_{false};
};

}  // namespace ruvia::detail
