#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>

#include "http2/http2_local_settings.h"

namespace ruvia::detail {

// Owner-lease releases and DATA credits that could not be applied yet. A pinned
// stream is released only after its delivered DATA credit and queued events are
// gone; a release or credit whose application failed is retried later. Both sets
// are bounded by the stream-concurrency limit, so they never allocate.
class http2_stream_lease_ledger final {
public:
    enum class release_kind_type : std::uint8_t {
        after_credits,
        after_tunnel_end,
        abandon,
    };
    struct release_type final {
        std::uint32_t stream_id_{0};
        release_kind_type kind_{release_kind_type::after_credits};
    };
    struct credit_type final {
        std::uint32_t stream_id_{0};
        std::uint32_t bytes_{0};
    };

    [[nodiscard]] const release_type* release(std::uint32_t stream_id) const noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                return &releases_[index];
            }
        }
        return nullptr;
    }

    // abandon and after_tunnel_end override a pending release; after_credits only
    // replaces abandon, whose reset has then been sent.
    void defer_release(std::uint32_t stream_id, release_kind_type kind) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            auto& existing = releases_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (kind != release_kind_type::after_credits || existing.kind_ == release_kind_type::abandon) {
                existing.kind_ = kind;
            }
            return;
        }
        if (release_count_ == releases_.size()) {
            std::terminate();
        }
        releases_[release_count_++] = release_type{stream_id, kind};
    }

    void erase_release(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < release_count_; ++index) {
            if (releases_[index].stream_id_ == stream_id) {
                releases_[index] = releases_[--release_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t release_count() const noexcept {
        return release_count_;
    }
    [[nodiscard]] std::uint32_t release_stream_at(std::size_t index) const noexcept {
        return releases_[index].stream_id_;
    }

    void defer_credit(std::uint32_t stream_id, std::uint32_t bytes) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            auto& existing = credits_[index];
            if (existing.stream_id_ != stream_id) {
                continue;
            }
            if (bytes > (std::numeric_limits<std::uint32_t>::max)() - existing.bytes_) {
                std::terminate();
            }
            existing.bytes_ += bytes;
            return;
        }
        if (credit_count_ == credits_.size()) {
            std::terminate();
        }
        credits_[credit_count_++] = credit_type{stream_id, bytes};
    }

    void erase_credit(std::uint32_t stream_id) noexcept {
        for (std::size_t index = 0; index < credit_count_; ++index) {
            if (credits_[index].stream_id_ == stream_id) {
                credits_[index] = credits_[--credit_count_];
                return;
            }
        }
    }

    [[nodiscard]] std::size_t credit_count() const noexcept {
        return credit_count_;
    }
    [[nodiscard]] credit_type credit_at(std::size_t index) const noexcept {
        return credits_[index];
    }

private:
    std::array<release_type, http2_local_settings::max_concurrent_streams> releases_{};
    std::size_t release_count_{0};
    std::array<credit_type, http2_local_settings::max_concurrent_streams> credits_{};
    std::size_t credit_count_{0};
};

}  // namespace ruvia::detail
