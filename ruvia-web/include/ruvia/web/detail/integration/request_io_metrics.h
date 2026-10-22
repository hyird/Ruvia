#pragma once

#include <chrono>
#include <cstdint>

#include "ruvia/core/scoped_operation.h"

namespace ruvia::detail {

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
    scoped_request_io_timer(operation_scope& scope, request_io_kind kind) noexcept
        : scoped_request_io_timer(&scope, kind) {}
    scoped_request_io_timer(operation_scope* scope, request_io_kind kind) noexcept
        : metrics_(scope != nullptr ? static_cast<request_io_metrics*>(scope->context()) : nullptr),
          kind_(kind),
          started_at_(metrics_ != nullptr ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{}) {}

    ~scoped_request_io_timer() {
        if (metrics_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started_at_)
                                 .count();
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
