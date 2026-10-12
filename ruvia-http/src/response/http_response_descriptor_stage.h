#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"

namespace ruvia {

// Owns only newly staged descriptors, never copies the existing header block.
// Staging and reservation may throw and leave the block unchanged; publication
// transfers every descriptor without allocation; unwinding releases the rest.
template <std::size_t capacity>
class http_response_headers::descriptor_stage final {
public:
    explicit descriptor_stage(http_response_headers& owner_value) noexcept
        : owner_(owner_value) {}
    descriptor_stage(const descriptor_stage&) = delete;
    descriptor_stage& operator=(const descriptor_stage&) = delete;

    ~descriptor_stage() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            owner_.release_header(prepared_[slot]);
        }
    }

    // Each staged field must be distinct.
    void stage(std::string_view name, std::string_view value, std::uint32_t known_bit) {
        const auto builtin = make_static_header(name, value, known_bit);
        prepared_[count_] = builtin ? *builtin : owner_.make_owned_header(name, value, known_bit);
        ++count_;
    }

    // Reserve a table slot for every staged field the block lacks, so publish
    // cannot allocate. Removing fields before publish keeps the reservation valid.
    void reserve() {
        std::size_t missing_headers = 0;
        for (std::size_t slot = 0; slot < count_; ++slot) {
            const auto& prepared = prepared_[slot];
            if (owner_.find_header_for_read(prepared.name(), detail::http_response_header_access::known_bit(prepared)) ==
                nullptr) {
                ++missing_headers;
            }
        }
        owner_.reserve(owner_.size() + missing_headers);
    }

    // Replace each field in place, collapsing earlier appended lines, or append it.
    void publish() noexcept {
        for (std::size_t slot = 0; slot < count_; ++slot) {
            const auto prepared = prepared_[slot];
            const auto known_bit = detail::http_response_header_access::known_bit(prepared);
            if (auto* const existing = owner_.find_header_for_update(prepared.name(), known_bit)) {
                const bool was_appended = detail::http_response_header_access::append(*existing);
                owner_.release_header(*existing);
                *existing = prepared;
                if (was_appended) {
                    (void)owner_.collapse_response_headers(*existing, known_bit);
                }
                continue;
            }
            const auto index = owner_.size();
            (void)owner_.append_prepared_header(prepared);
            owner_.record_known_header_index(known_bit, index);
        }
        count_ = 0;
    }

private:
    http_response_headers& owner_;
    std::array<http_response_header, capacity> prepared_{};
    std::size_t count_{0};
};

}  // namespace ruvia
