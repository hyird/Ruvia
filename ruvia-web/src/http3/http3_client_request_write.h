#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http_known_method.h"

#include "client/http_client_request_storage.h"

namespace ruvia::detail {
class http3_client_sans_io_session_engine;

enum class http3_client_request_write_error : std::uint8_t {
    invalid_request,
    unsupported_tunnel,
    request_encoding,
    out_of_memory,
    invalid_state,
    excessive_acknowledgement,
    data_plan,
};

// Owns the completed request in worker-local storage. Offered spans remain
// stable until acknowledged; acknowledge(0) leaves the same span available.
class http3_client_request_write final {
public:
    class prepared_tag_type final {
    private:
        friend class http3_client_request_write;
        prepared_tag_type() = default;
    };

    using error_type = http3_client_request_write_error;
    using segment_type = std::span<const char>;

    // Failure leaves request untouched. Ownership is committed only after all
    // encoding and allocations have succeeded.
    [[nodiscard]] static std::variant<http3_client_request_write, error_type> create(
        http_client_request_storage&& request, std::string_view scheme, std::string_view authority,
        std::pmr::memory_resource* worker_pool, http3_field_section_limits limits = {}) noexcept;

    http3_client_request_write(const http3_client_request_write&) = delete;
    http3_client_request_write& operator=(const http3_client_request_write&) = delete;
    http3_client_request_write(http3_client_request_write&& other);
    http3_client_request_write& operator=(http3_client_request_write&&) = delete;

    [[nodiscard]] bool prepare_connection_head(std::uint64_t stream_id, http3_client_sans_io_session_engine& engine);
    [[nodiscard]] std::variant<segment_type, error_type> next() noexcept;
    [[nodiscard]] std::variant<std::monostate, error_type> acknowledge(std::size_t count) noexcept;
    [[nodiscard]] std::variant<std::monostate, error_type> acknowledge_fin(bool successful) noexcept;
    [[nodiscard]] bool requires_connect_settings() const noexcept {
        return request_.is_tunnel() && !request_.tunnel_protocol().empty();
    }
    [[nodiscard]] bool waiting_for_content() const noexcept;
    void stop_sending() noexcept;
    [[nodiscard]] bool fin_ready() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] http_known_method known_method() const noexcept;
    // Owner must first stop all transport access, including a pending WANT
    // write. Invalidates offered spans and disables this cursor. Transfers the
    // original request once, retaining its allocator (which must outlive it).
    [[nodiscard]] std::optional<http_client_request_storage> take_request_after_retirement();

    // Only create() can produce prepared_tag_type; this lets variant construct the
    // completed cursor in place without a potentially throwing cursor move.
    explicit http3_client_request_write(prepared_tag_type, std::pmr::memory_resource* resource,
        http_client_request_storage&& request, std::pmr::string&& scheme,
        std::pmr::string&& authority, std::pmr::vector<char>&& headers,
        http3_data_write_plan data_plan, http3_field_section_limits limits = {}) noexcept;

private:
    enum class state_type : std::uint8_t { headers,
        data_header,
        data_body,
        trailers,
        fin,
        finished,
        failed };
    [[nodiscard]] static http3_client_request_write& require_no_outstanding_segment(
        http3_client_request_write& other);
    [[nodiscard]] segment_type active_segment() const noexcept;
    [[nodiscard]] std::variant<std::monostate, error_type> prepare_data() noexcept;
    [[nodiscard]] std::variant<std::monostate, error_type> prepare_trailers() noexcept;
    [[nodiscard]] std::variant<std::monostate, error_type> fail_plan() noexcept;

    std::pmr::memory_resource* worker_pool_;
    http3_field_section_limits field_limits_{};
    http_client_request_storage request_;
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::vector<char> headers_;
    std::optional<http3_data_write_plan> data_plan_;
    http3_data_write_plan::chunk_type chunk_{};
    std::size_t segment_offset_{};
    std::size_t body_offset_{};
    state_type state_{state_type::headers};
    bool chunk_pending_{};
    bool offered_{};
    bool request_taken_{};
};

}  // namespace ruvia::detail
