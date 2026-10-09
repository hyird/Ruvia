#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/quic_connection.h"

#include "http3/http3_client_request_write.h"

namespace ruvia::detail {

// One worker-affine outbound request stream. The caller must first confirm that
// the local control and QPACK stream prefixes have been accepted. This driver
// only writes the request; a separate response owner handles reads and joins.
// Keep its address stable while a write is pending, and close the QUIC stream
// before destroying this driver on cancellation, fatal error or shutdown.
class http3_client_request_driver final {
public:
    using stream_id_type = std::uint64_t;
    enum class result_type : std::uint8_t { blocked,
        connection_draining,
        progress,
        finished,
        fatal };

    explicit http3_client_request_driver(http3_client_request_write&& request)
        : request_(std::in_place, std::move(request)) {}
    http3_client_request_driver(const http3_client_request_driver&) = delete;
    http3_client_request_driver& operator=(const http3_client_request_driver&) = delete;
    http3_client_request_driver(http3_client_request_driver&&) = delete;
    http3_client_request_driver& operator=(http3_client_request_driver&&) = delete;

    // open() -> quic_stream_open_result; register_response(id, known_method) -> bool;
    // write(id, bytes) -> quic_stream_write_result; finish(id) -> quic_operation_status. Register the
    // request and its HEAD semantics before emitting its HEADERS. At most one
    // nonblocking write and one FIN attempt per tick; no task is started here.
    // After progress/finished, pump QUIC before sleeping even if the previous
    // socket pump had no traffic: SSL may only queue output during this tick.
    // blocked is stream credit/WANT, not necessarily a UDP-writable signal.
    template <typename open_type, typename register_value_type, typename write_type, typename finish>
    [[nodiscard]] result_type drive(open_type&& open, register_value_type&& register_response,
        write_type&& write, finish&& finish_value) {
        if (failed_) {
            return result_type::fatal;
        }
        if (request_->finished()) {
            return result_type::finished;
        }
        bool progress_value = false;
        if (!stream_id_) {
            const auto opened = open();
            if (opened.status_ == ruvia::quic_operation_status::would_block ||
                opened.status_ == ruvia::quic_operation_status::need_input) {
                return result_type::blocked;
            }
            if (opened.status_ == ruvia::quic_operation_status::draining ||
                opened.status_ == ruvia::quic_operation_status::closing ||
                opened.status_ == ruvia::quic_operation_status::retired) {
                return result_type::connection_draining;
            }
            if (opened.status_ != ruvia::quic_operation_status::accepted) {
                return fail();
            }
            stream_id_ = opened.stream_id_;
            progress_value = true;
        }
        if (!registered_) {
            try {
                if (!register_response(*stream_id_, request_->known_method())) {
                    return fail();
                }
            } catch (...) {
                return fail();
            }
            registered_ = true;
        }
        const auto segment = request_->next();
        if ((segment.index() != 0)) {
            return fail();
        }
        if (!std::get<0>(segment).empty()) {
            const auto result_value = write(*stream_id_, std::get<0>(segment));
            switch (result_value.status_) {
                case ruvia::quic_operation_status::accepted:
                    if (result_value.accepted_ > std::get<0>(segment).size() || (request_->acknowledge(result_value.accepted_).index() != 0)) {
                        return fail();
                    }
                    return progress_value || result_value.accepted_ != 0 ? result_type::progress : result_type::blocked;
                case ruvia::quic_operation_status::would_block:
                case ruvia::quic_operation_status::need_input:
                    if ((request_->acknowledge(0).index() != 0)) {
                        return fail();
                    }
                    return progress_value ? result_type::progress : result_type::blocked;
                default:
                    return fail();
            }
        }
        if (!request_->fin_ready()) {
            return progress_value ? result_type::progress : result_type::blocked;
        }
        const auto error = finish_value(*stream_id_);
        if (error == ruvia::quic_operation_status::accepted) {
            if ((request_->acknowledge_fin(true).index() != 0)) {
                return fail();
            }
            return result_type::finished;
        }
        if (error == ruvia::quic_operation_status::would_block ||
            error == ruvia::quic_operation_status::need_input) {
            return progress_value ? result_type::progress : result_type::blocked;
        }
        (void)request_->acknowledge_fin(false);
        return fail();
    }

    [[nodiscard]] bool prepare_connection_head(std::uint64_t id, http3_client_sans_io_session_engine& engine) {
        return request_->prepare_connection_head(id, engine);
    }
    void stop_sending() noexcept {
        request_->stop_sending();
    }
    [[nodiscard]] bool requires_connect_settings() const noexcept {
        return request_->requires_connect_settings();
    }
    [[nodiscard]] bool waiting_for_content() const noexcept {
        return request_->waiting_for_content();
    }
    [[nodiscard]] std::optional<stream_id_type> stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] bool finished() const noexcept {
        return request_->finished();
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] std::optional<http_client_request_storage> take_request_after_retirement() {
        failed_ = true;
        return request_->take_request_after_retirement();
    }
    [[nodiscard]] bool replay_after_rejected_early_stream(
        std::string_view scheme, std::string_view authority,
        std::pmr::memory_resource* resource) {
        auto original = request_->take_request_after_retirement();
        if (!original) {
            return false;
        }
        auto replay = http3_client_request_write::create(
            std::move(*original), scheme, authority, resource);
        if ((replay.index() != 0)) {
            return false;
        }
        request_.reset();
        request_.emplace(std::move(std::get<0>(replay)));
        stream_id_.reset();
        registered_ = false;
        failed_ = false;
        return true;
    }

private:
    [[nodiscard]] result_type fail() noexcept {
        failed_ = true;
        return result_type::fatal;
    }

    std::optional<http3_client_request_write> request_;
    std::optional<stream_id_type> stream_id_;
    bool registered_{};
    bool failed_{};
};

}  // namespace ruvia::detail
