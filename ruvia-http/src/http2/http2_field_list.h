#pragma once

#include <nghttp2/nghttp2.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_pmr_string.h"

#include "http2/http2_header_rules.h"
#include "util/geometric_reserve.h"
namespace ruvia::detail {

// The field section handed to one nghttp2 HEADERS/PUSH_PROMISE submission.
// Entries either borrow caller text that outlives the submission or own a copy
// (lowercased names, formatted values). nghttp2 copies every name and value
// while submitting, so nothing here is retained past that call; nghttp2's
// session deflater is the only HPACK encoder.
class http2_field_list final {
public:
    explicit http2_field_list(std::pmr::memory_resource* resource)
        : owned_(resource),
          entries_(resource),
          nv_(resource) {}

    void clear() noexcept {
        owned_.clear();
        entries_.clear();
        nv_.clear();
    }

    // Keep common sections warm; return oversized storage so one large field
    // section cannot pin descriptor buffers to an idle connection.
    void release() noexcept {
        http_clear_pmr_string_retaining_small(owned_);
        if (entries_.capacity() > http_retained_pmr_string_bytes / sizeof(entry_type)) {
            entries_ = std::pmr::vector<entry_type>(entries_.get_allocator());
        } else {
            entries_.clear();
        }
        if (nv_.capacity() > http_retained_pmr_string_bytes / sizeof(nghttp2_nv)) {
            nv_ = std::pmr::vector<nghttp2_nv>(nv_.get_allocator());
        } else {
            nv_.clear();
        }
    }

    void reserve(std::size_t fields) {
        entries_.reserve(entries_.size() + fields);
        nv_.reserve(entries_.size() + fields);
    }

    // `name` must already be lowercase; both views must outlive the submission.
    void add(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name),
            .value_ = text_type::borrowed(value),
            .sensitive_ = hpack_header_name_is_sensitive(name)});
    }

    // Copies `value`, which may be a temporary formatting buffer.
    void add_owned_value(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto owned_value = own(value);
        entries_.push_back(entry_type{.name_ = text_type::borrowed(name),
            .value_ = owned_value,
            .sensitive_ = hpack_header_name_is_sensitive(name)});
    }

    // HTTP/2 field names are lowercase on the wire (RFC 9113 §8.2.1).
    void add_lowercase(std::string_view name, std::string_view value) {
        reserve_geometric(entries_, entries_.size() + 1);
        const auto offset = owned_.size();
        const auto lower = http2_lower_header_name(name, [this, offset](std::size_t size) {
            reserve_geometric(owned_, offset + size);
            owned_.resize(offset + size);
            return owned_.data() + offset;
        });
        const auto entry_name = lower.data() == name.data()
                                    ? text_type::borrowed(name)
                                    : text_type{.data_ = nullptr, .offset_ = offset, .size_ = lower.size()};
        entries_.push_back(entry_type{.name_ = entry_name,
            .value_ = text_type::borrowed(value),
            .sensitive_ = hpack_header_name_is_sensitive(lower)});
    }

    template <typename field_range_type>
    void add_lowercase_fields(const field_range_type& fields) {
        for (const auto& field : fields) {
            add_lowercase(field.name(), field.value());
        }
    }

    // Resolves owned offsets once every entry is final.
    [[nodiscard]] std::span<const nghttp2_nv> nv() {
        nv_.clear();
        nv_.reserve(entries_.size());
        for (const auto& entry : entries_) {
            const auto name = resolve(entry.name_);
            const auto value = resolve(entry.value_);
            nv_.push_back(nghttp2_nv{
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = static_cast<std::uint8_t>(entry.sensitive_ ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
            });
        }
        return nv_;
    }

private:
    struct text_type final {
        const char* data_{nullptr};
        std::size_t offset_{0};
        std::size_t size_{0};

        [[nodiscard]] static text_type borrowed(std::string_view text) noexcept {
            return text_type{.data_ = text.data(), .offset_ = 0, .size_ = text.size()};
        }
    };
    struct entry_type final {
        text_type name_;
        text_type value_;
        bool sensitive_{false};
    };

    [[nodiscard]] text_type own(std::string_view text) {
        const auto offset = owned_.size();
        reserve_geometric(owned_, offset + text.size());
        owned_.append(text.data(), text.size());
        return text_type{.data_ = nullptr, .offset_ = offset, .size_ = text.size()};
    }

    [[nodiscard]] std::string_view resolve(const text_type& text) const noexcept {
        return text.data_ != nullptr ? std::string_view(text.data_, text.size_)
                                     : std::string_view(owned_).substr(text.offset_, text.size_);
    }

    std::pmr::string owned_;
    std::pmr::vector<entry_type> entries_;
    std::pmr::vector<nghttp2_nv> nv_;
};

}  // namespace ruvia::detail
