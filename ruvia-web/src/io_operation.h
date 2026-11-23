#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "operation_interrupt.h"

namespace ruvia::detail {

using ::ruvia::claim_operation_lane;
using ::ruvia::operation_interrupt;
using ::ruvia::operation_interrupt_cause;
using ::ruvia::operation_interrupt_source;
using ::ruvia::operation_lane_lease;
using ::ruvia::operation_steps;

template <typename start_type>
[[nodiscard]] decltype(auto) start_request_linked(const operation_options& options, const stop_token& request_stop, start_type&& start) {
    return ::ruvia::start_linked_operation(options, request_stop, std::forward<start_type>(start));
}

[[nodiscard]] inline operation_options request_bound_options(const operation_options& options, const stop_token& request_stop) {
    return ::ruvia::bound_operation_options(options, request_stop);
}

[[nodiscard]] inline std::unique_ptr<const linked_operation_options> make_request_link(const operation_options& options, const stop_token& request_stop) {
    return ::ruvia::make_operation_link(options, request_stop);
}

struct request_io_metrics final {
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
};

enum class request_io_kind : std::uint8_t {
    db = 0,
    redis = 1,
};

class scoped_request_io_timer final {
public:
    // `metrics` is the request's counter block, or null outside a request
    // (worker callbacks, standalone clients); null disables timing.
    scoped_request_io_timer(request_io_metrics* metrics, request_io_kind kind) noexcept :
        metrics_(metrics),
        kind_(kind),
        started_at_(metrics_ != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

    ~scoped_request_io_timer() {
        if (metrics_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_at_).count();
        const auto micros = elapsed < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(elapsed);
        if (kind_ == request_io_kind::db) {
            ++metrics_->db_count_;
            metrics_->db_duration_micros_ += micros;
        } else {
            ++metrics_->redis_count_;
            metrics_->redis_duration_micros_ += micros;
        }
    }

    scoped_request_io_timer(const scoped_request_io_timer&) = delete;
    scoped_request_io_timer& operator=(const scoped_request_io_timer&) = delete;

private:
    request_io_metrics* metrics_{nullptr};
    request_io_kind kind_{request_io_kind::db};
    std::chrono::steady_clock::time_point started_at_{};
};

}  // namespace ruvia::detail
