#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

#include "ruvia/http/detail/util/http_pmr_object.h"

#include "http2/http2_local_settings.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_stream_state.h"
#include "util/geometric_reserve.h"

namespace ruvia::detail {

class http2_stream_table final {
public:
    // Hard upper bound on live stream records. Reserved push promises do not hold a
    // concurrency slot, so this can exceed SETTINGS_MAX_CONCURRENT_STREAMS.
    static constexpr std::size_t max_streams =
        static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 4;
    static constexpr std::size_t max_push_streams =
        max_streams - static_cast<std::size_t>(http2_local_settings::max_concurrent_streams);

    explicit http2_stream_table(std::pmr::memory_resource* resource)
        : resource_(http_pmr_resource_or_default(resource)),
          overflow_ids_(std::size_t{0}, resource_),
          overflow_(std::size_t{0}, resource_) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http2_stream_state* find(std::uint32_t stream_id) & noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] http2_stream_state* find(std::uint32_t) && = delete;

    [[nodiscard]] const http2_stream_state* find(std::uint32_t stream_id) const& noexcept {
        if (stream_id == 0) {
            return nullptr;
        }
        for (const auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                return overflow_[i].get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] const http2_stream_state* find(std::uint32_t) const&& = delete;

    // NOTE on slot reuse: a closed stream's http2_stream_state is destroyed here (inline
    // slot .reset() / overflow erase), freeing its per-stream pmr strings. We do NOT
    // pool the storage and reset-in-place to retain capacity, deliberately. A correct
    // reset_for_reuse would have to reinitialise ~85 fields across 8 sub-objects, and a
    // single missed field would leak one request's decoded headers / routing / body
    // into the next reused slot -- a cross-request data-disclosure risk not worth taking
    // for a micro-optimisation off the measured hot path.
    [[nodiscard]] http2_stream_state* create(std::uint32_t stream_id) & {
        if (auto* existing = find(stream_id); existing != nullptr) {
            return existing;
        }
        const bool is_push = (stream_id & 1U) == 0;
        if (size_ >= max_streams || (is_push && push_size_ >= max_push_streams)) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id, resource_);
                ++size_;
                if (is_push) {
                    ++push_size_;
                }
                return &*slot;
            }
        }
        auto stream = make_http_pmr_object<http2_stream_state>(resource_, stream_id, resource_);
        reserve_geometric(overflow_ids_, overflow_ids_.size() + 1);
        reserve_geometric(overflow_, overflow_.size() + 1);
        auto* result_value = stream.get();
        overflow_ids_.push_back(stream_id);
        overflow_.push_back(std::move(stream));
        ++size_;
        if (is_push) {
            ++push_size_;
        }
        return result_value;
    }
    [[nodiscard]] http2_stream_state* create(std::uint32_t) && = delete;

    bool remove(std::uint32_t stream_id) noexcept {
        if (stream_id == 0) {
            return false;
        }
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                slot.reset();
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        const auto* const ids = overflow_ids_.data();
        const auto count = overflow_ids_.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == stream_id) {
                erase_overflow_at(i);
                --size_;
                if ((stream_id & 1U) == 0) {
                    --push_size_;
                }
                return true;
            }
        }
        return false;
    }

    template <typename callback>
    void for_each(callback&& callback_value) {
        snapshot_iteration_guard_type guard_value(*this);
        for (auto& slot : inline_) {
            if (slot) {
                callback_value(*slot);
            }
        }
        const auto overflow_end = overflow_.size();
        for (std::size_t i = 0; i < overflow_end; ++i) {
            auto& stream = overflow_[i];
            if (stream != nullptr) {
                callback_value(*stream);
            }
        }
    }

private:
    // Two inline slots cover the typical request/response cadence without
    // paying the full concurrent-stream budget up front: each slot embeds a
    // ~1.3 KB http2_stream_state, and the table lives in the connection object
    // for the connection's whole lifetime, so the two slots put ~2.6 KB into
    // every connection. Deeper multiplexing spills to the pmr overflow path.
    static constexpr std::size_t inline_capacity = 2;
    using overflow_stream_type =
        std::unique_ptr<http2_stream_state, http_pmr_object_deleter<http2_stream_state>>;

    class snapshot_iteration_guard_type final {
    public:
        explicit snapshot_iteration_guard_type(http2_stream_table& table_value) noexcept
            : table_(table_value) {
            ++table_.snapshot_iteration_depth_;
        }

        snapshot_iteration_guard_type(const snapshot_iteration_guard_type&) = delete;
        snapshot_iteration_guard_type& operator=(const snapshot_iteration_guard_type&) = delete;

        ~snapshot_iteration_guard_type() {
            --table_.snapshot_iteration_depth_;
            table_.compact_overflow_if_idle();
        }

    private:
        http2_stream_table& table_;
    };

    void erase_overflow_at(std::size_t index) noexcept {
        if (snapshot_iteration_depth_ != 0) {
            overflow_ids_[index] = 0;
            overflow_[index].reset();
            overflow_needs_compact_ = true;
            return;
        }
        if (index + 1 != overflow_.size()) {
            overflow_ids_[index] = overflow_ids_.back();
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_ids_.pop_back();
        overflow_.pop_back();
    }

    void compact_overflow_if_idle() noexcept {
        if (snapshot_iteration_depth_ != 0 || !overflow_needs_compact_) {
            return;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            if (overflow_[i] == nullptr) {
                if (i + 1 != overflow_.size()) {
                    overflow_ids_[i] = overflow_ids_.back();
                    overflow_[i] = std::move(overflow_.back());
                }
                overflow_ids_.pop_back();
                overflow_.pop_back();
                continue;
            }
            ++i;
        }
        overflow_needs_compact_ = false;
    }

    std::pmr::memory_resource* resource_;
    std::array<std::optional<http2_stream_state>, inline_capacity> inline_{};
    std::pmr::vector<std::uint32_t> overflow_ids_;
    std::pmr::vector<overflow_stream_type> overflow_;
    std::size_t size_{0};
    std::size_t push_size_{0};
    std::size_t snapshot_iteration_depth_{0};
    bool overflow_needs_compact_{false};
};

}  // namespace ruvia::detail
