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
#include "http2/http2_stream_state.h"

namespace ruvia::detail {

class http2_stream_table final {
public:
    explicit http2_stream_table(std::pmr::memory_resource* resource)
        : resource_(http_pmr_resource_or_default(resource)),
          overflow_(std::size_t{0}, resource_) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http2_stream_state* find(std::uint32_t stream_id) & noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        for (auto& stream : overflow_) {
            if (stream != nullptr && stream->id() == stream_id) {
                return stream.get();
            }
        }
        return nullptr;
    }
    [[nodiscard]] http2_stream_state* find(std::uint32_t) && = delete;

    [[nodiscard]] const http2_stream_state* find(std::uint32_t stream_id) const& noexcept {
        for (const auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                return &*slot;
            }
        }
        for (const auto& stream : overflow_) {
            if (stream != nullptr && stream->id() == stream_id) {
                return stream.get();
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
    [[nodiscard]] http2_stream_state* create(
        std::uint32_t stream_id, std::int32_t peer_initial_window_size) & {
        if (auto* existing = find(stream_id); existing != nullptr) {
            return existing;
        }
        if (size_ >= http2_local_settings::max_concurrent_streams) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(stream_id, resource_);
                slot->set_send_window(peer_initial_window_size);
                ++size_;
                return &*slot;
            }
        }
        auto stream = make_http_pmr_object<http2_stream_state>(resource_, stream_id, resource_);
        stream->set_send_window(peer_initial_window_size);
        auto* result_value = stream.get();
        overflow_.push_back(std::move(stream));
        ++size_;
        return result_value;
    }
    [[nodiscard]] http2_stream_state* create(std::uint32_t, std::int32_t) && = delete;

    bool remove(std::uint32_t stream_id) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->id() == stream_id) {
                slot.reset();
                --size_;
                return true;
            }
        }
        for (std::size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] != nullptr && overflow_[i]->id() == stream_id) {
                erase_overflow_at(i);
                --size_;
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

    template <typename callback>
    void remove_aborted(callback&& callback_value) {
        for (auto& slot : inline_) {
            if (!slot || !slot->is_aborted()) {
                continue;
            }
            callback_value(*slot);
            slot.reset();
            --size_;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            auto& stream = overflow_[i];
            if (stream == nullptr || !stream->is_aborted()) {
                ++i;
                continue;
            }
            callback_value(*stream);
            erase_overflow_at(i);
            --size_;
        }
    }

    [[nodiscard]] bool apply_send_window_delta(std::int64_t delta) noexcept {
        // SETTINGS_INITIAL_WINDOW_SIZE applies to every active stream as one
        // protocol transaction. Preflight the complete table first; applying
        // the delta while discovering a later overflow would leave earlier
        // streams with a different window even though the SETTINGS is rejected.
        bool fits = true;
        for_each([&fits, delta](http2_stream_state& stream) noexcept {
            if (!fits) {
                return;
            }
            const auto current = static_cast<std::int64_t>(stream.send_window());
            if (delta > static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) -
                            current ||
                delta < static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)()) -
                            current) {
                fits = false;
            }
        });
        if (!fits) {
            return false;
        }
        for_each([delta](http2_stream_state& stream) noexcept { (void)stream.add_send_window(delta); });
        return true;
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
            overflow_[index].reset();
            overflow_needs_compact_ = true;
            return;
        }
        if (index + 1 != overflow_.size()) {
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_.pop_back();
    }

    void compact_overflow_if_idle() noexcept {
        if (snapshot_iteration_depth_ != 0 || !overflow_needs_compact_) {
            return;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            if (overflow_[i] == nullptr) {
                if (i + 1 != overflow_.size()) {
                    overflow_[i] = std::move(overflow_.back());
                }
                overflow_.pop_back();
                continue;
            }
            ++i;
        }
        overflow_needs_compact_ = false;
    }

    std::pmr::memory_resource* resource_;
    std::array<std::optional<http2_stream_state>, inline_capacity> inline_{};
    std::pmr::vector<overflow_stream_type> overflow_;
    std::size_t size_{0};
    std::size_t snapshot_iteration_depth_{0};
    bool overflow_needs_compact_{false};
};

[[nodiscard]] inline bool http2_is_idle_stream(
    std::uint32_t stream_id, std::uint32_t last_stream_id) noexcept {
    return stream_id > last_stream_id || (stream_id & 1U) == 0;
}

inline bool http2_apply_stream_send_window_delta(
    http2_stream_table& streams, std::int64_t delta) noexcept {
    return streams.apply_send_window_delta(delta);
}

}  // namespace ruvia::detail
