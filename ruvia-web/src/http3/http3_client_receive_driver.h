#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "ruvia/http/quic_connection.h"

#include "http3/http3_client_sans_io_session_engine.h"

namespace ruvia::detail {

// Single-worker, sans-runtime receive adapter. A connection driver owns one
// instance and visits each active peer/request stream fairly; this accepts at
// most one bounded QUIC read per call. The HTTP/3 engine synchronously copies
// every exposed event before this scratch buffer is reused. The caller still
// owns stream retirement, transport close, cancellation, and driver task join.
class http3_client_receive_driver final {
public:
    static constexpr std::size_t read_block_bytes = 16 * 1024;
    using stream_id_type = std::uint64_t;
    enum class status_type : std::uint8_t {
        blocked,
        progress,
        response_complete,
        stream_reset,
        peer_stream_ended,
        stream_error,
        connection_error,
        transport_error,
    };
    struct result_type final {
        status_type status_{status_type::blocked};
        std::size_t bytes_{};
        http3_client_sans_io_session_engine::result_type protocol_{};
        std::optional<std::uint64_t> peer_reset_error_code_{};
        bool peer_reports_unprocessed_{false};
    };

    explicit http3_client_receive_driver(http3_client_sans_io_session_engine& engine) noexcept
        : engine_(engine),
          pending_(engine.resource()) {}
    [[nodiscard]] result_type accept_reset(stream_id_type id, std::optional<std::uint64_t> peer_error_code) {
        const bool unprocessed = peer_error_code && engine_.peer_reports_unprocessed(id, peer_error_code);
        auto result_value = feed(id, {}, false, true, 0);
        if (result_value.status_ == status_type::stream_reset) {
            result_value.peer_reset_error_code_ = peer_error_code;
            result_value.peer_reports_unprocessed_ = unprocessed;
        }
        return result_value;
    }
    void retire(stream_id_type id) noexcept {
        pending_.erase(id);
    }
    http3_client_receive_driver(const http3_client_receive_driver&) = delete;
    http3_client_receive_driver& operator=(const http3_client_receive_driver&) = delete;

    // read(id, output) -> quic_stream_read_result. The caller must stop
    // visiting a stream after terminal status; a connection-scope failure must
    // close the QUIC transport before any response is delivered as successful.
    // A streaming owner can pass remaining producer capacity as read_budget.
    // Zero does not consume stream bytes; OpenSSL can still buffer received
    // datagrams internally, so this alone is not an end-to-end memory bound.
    template <typename read_type>
    [[nodiscard]] result_type drive(stream_id_type id, read_type&& read,
        std::size_t read_budget = read_block_bytes) {
        if (driving_) {
            // In particular, do not overwrite scratch_ while an outer feed's
            // callback still borrows its body span from those bytes.
            throw std::logic_error("HTTP/3 receive driver cannot be called recursively");
        }
        if (read_budget == 0) {
            return {};
        }
        driving_ = true;
        struct drive_guard final {
            bool& driving_;
            ~drive_guard() {
                driving_ = false;
            }
        } guard_value{driving_};
        if (const auto pending = pending_.find(id); pending != pending_.end()) {
            if (pending->second.bytes_.size() > read_budget) {
                return {};
            }
            return feed(id, pending->second.bytes_, pending->second.fin_, false, 0);
        }
        const auto input = read(id,
            std::span<char>(scratch_).first(std::min(read_budget, scratch_.size())));
        using read_status_type = ruvia::quic_stream_read_status;
        switch (input.status_) {
            case read_status_type::would_block:
                return {};
            case read_status_type::data:
                if (input.size_ == 0 || input.size_ > std::min(read_budget, scratch_.size())) {
                    return transport_error();
                }
                return feed(id, {scratch_.data(), input.size_}, false, false, input.size_);
            case read_status_type::fin:
                return feed(id, {}, true, false, 0);
            case read_status_type::reset: {
                const bool unprocessed = input.peer_reset_error_code_ &&
                                         engine_.peer_reports_unprocessed(id, input.peer_reset_error_code_);
                auto result_value = feed(id, {}, false, true, 0);
                if (result_value.status_ == status_type::stream_reset) {
                    result_value.peer_reset_error_code_ = input.peer_reset_error_code_;
                    result_value.peer_reports_unprocessed_ = unprocessed;
                }
                return result_value;
            }
            case read_status_type::closed:
                return transport_error();
        }
        return transport_error();
    }

private:
    [[nodiscard]] result_type feed(stream_id_type id, std::span<const char> bytes_value,
        bool fin, bool reset, std::size_t accepted_bytes) {
        const auto parsed_value = engine_.feed(id, bytes_value, fin, reset);
        if (parsed_value.scope_ == http3_connection_error_scope::connection) {
            return {.status_ = parsed_value.status_ == http3_client_sans_io_session_status::transport_error
                                   ? status_type::transport_error
                                   : status_type::connection_error,
                .bytes_ = accepted_bytes,
                .protocol_ = parsed_value};
        }
        if (parsed_value.scope_ == http3_connection_error_scope::stream) {
            return {.status_ = status_type::stream_error, .bytes_ = accepted_bytes, .protocol_ = parsed_value};
        }
        if (parsed_value.status_ == http3_client_sans_io_session_status::invalid_state ||
            parsed_value.status_ == http3_client_sans_io_session_status::stream_limit_exceeded) {
            return transport_error();
        }
        if (parsed_value.status_ == http3_client_sans_io_session_status::qpack_blocked || parsed_value.status_ == http3_client_sans_io_session_status::push_promise_pending) {
            if (parsed_value.consumed_bytes_ > bytes_value.size()) {
                return transport_error();
            }
            if (!pending_.contains(id) && pending_.size() >= engine_.max_live_streams()) {
                return transport_error();
            }
            auto [pending, inserted] = pending_.try_emplace(id, engine_.resource());
            pending->second.bytes_.assign(std::string_view(bytes_value.data(), bytes_value.size()).substr(parsed_value.consumed_bytes_));
            pending->second.fin_ = fin;
            return {.status_ = accepted_bytes != 0 || parsed_value.consumed_bytes_ != 0 ? status_type::progress : status_type::blocked,
                .bytes_ = accepted_bytes,
                .protocol_ = parsed_value};
        }
        pending_.erase(id);
        if (reset) {
            return {.status_ = status_type::stream_reset, .protocol_ = parsed_value};
        }
        if (fin) {
            return {.status_ = parsed_value.status_ == http3_client_sans_io_session_status::message_end
                                   ? status_type::response_complete
                                   : status_type::peer_stream_ended,
                .protocol_ = parsed_value};
        }
        return {.status_ = status_type::progress, .bytes_ = accepted_bytes, .protocol_ = parsed_value};
    }

    [[nodiscard]] result_type transport_error() noexcept {
        const auto failure = engine_.stop();
        return {.status_ = failure.status_ == http3_client_sans_io_session_status::transport_error
                               ? status_type::transport_error
                               : status_type::connection_error,
            .protocol_ = failure};
    }

    struct pending_input_type final {
        explicit pending_input_type(std::pmr::memory_resource* resource)
            : bytes_(resource) {}
        std::pmr::string bytes_;
        bool fin_{};
    };
    http3_client_sans_io_session_engine& engine_;
    std::pmr::unordered_map<stream_id_type, pending_input_type> pending_;
    std::array<char, read_block_bytes> scratch_{};
    bool driving_{};
};

}  // namespace ruvia::detail
