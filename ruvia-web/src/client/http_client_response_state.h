#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_informational_response.h"
#include "ruvia/web/http_client_response_bytes.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"
#include "http3/http3_client_body_budget.h"

namespace ruvia {
class response_stream_writer;
}

namespace ruvia::detail {

class http_client_pool;
class http_client_response_memory_domain;
class http_client_result_budget_domain;
class http3_client_connection;

enum class http_client_response_transport : std::uint8_t {
    unassigned,
    http1,
    http2,
    http3,
};

// Address-stable response storage. Network stream registration and connection
// leases are added here rather than to the movable public response wrapper.
class http_client_response_state final {
public:
    http_client_response_state(const worker_handle& worker_value, std::pmr::memory_resource* resource)
        : head_signal_(worker_value),
          data_signal_(worker_value),
          space_signal_(worker_value),
          resource_(resource),
          informational_(resource),
          headers_(resource),
          trailers_(resource),
          buffered_(resource),
          pending_(resource) {}
    explicit http_client_response_state(http_client_response_memory_domain& memory_domain);
    http_client_response_state(worker_handle&&, std::pmr::memory_resource*) = delete;

    // Body algorithms run on the address-stable storage owner. The public
    // facade wraps them in body_operation_scope_ to enforce the linear lane.
    template <typename view>
    [[nodiscard]] task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(response_stream_writer* output = nullptr);
    [[nodiscard]] task<http_client_response_bytes> read_all(std::size_t max_bytes);
    void retain_reference() noexcept;
    void release_reference() noexcept;
    [[nodiscard]] http_client_response_memory_domain* memory_domain() const noexcept {
        return memory_domain_;
    }
    void notify_producer_space() noexcept;
    [[nodiscard]] bool bind_http3_body_budget(http3_client_body_budget& budget) noexcept;
    void release_http3_body_budget() noexcept;
    [[nodiscard]] bool has_http3_body_budget() const noexcept;
    [[nodiscard]] std::size_t producer_body_bytes() const noexcept;
    [[nodiscard]] std::size_t producer_body_budget_available() const noexcept;
    [[nodiscard]] bool retain_producer_body_bytes(std::size_t bytes) noexcept;
    void release_producer_body_bytes(std::size_t bytes) noexcept;
    void reconcile_producer_body_bytes() noexcept;
    [[nodiscard]] bool replace_producer_body_bytes(std::size_t bytes) noexcept;
    void discard_pending_body() noexcept;
    void discard_response_body() noexcept;
    void release_consumed_body_prefix();

    void retain_informational(http_status_code status, std::span<const http_header_view> fields);

    [[nodiscard]] http_client_output_queue* output() noexcept {
        return tunnel_ ? &tunnel_->output_ : upload_ ? &upload_->output_
                                                     : nullptr;
    }
    [[nodiscard]] bool receive_complete() const noexcept {
        return complete_ || (tunnel_ && tunnel_->accepted_ && tunnel_->receive_ended_);
    }
    std::optional<http_client_tunnel_state> tunnel_;
    std::optional<http_client_upload_state> upload_;
    worker_signal head_signal_;
    worker_signal data_signal_;
    worker_signal space_signal_;
    http_client_pool* pool_{nullptr};
    // Borrows the stable shared_ptr member owned by the response memory domain.
    // Only read_all() copies it after its single-result byte limit has passed.
    const std::shared_ptr<http_client_result_budget_domain>* result_budget_domain_{nullptr};
    std::pmr::memory_resource* resource_;
    http_status_code status_{http_status::ok};
    http_protocol_version protocol_version_{http_protocol_version::http11};
    http_known_method request_method_{http_known_method::unknown};
    std::optional<http_response_body_plan> response_body_plan_{};
    std::pmr::vector<http_client_informational_response> informational_;
    std::size_t informational_field_bytes_{};
    std::pmr::vector<http_header> headers_;
    std::pmr::vector<http_header> trailers_;
    // Declared before body strings so their storage is destroyed before the
    // receive reservation is returned to its worker-stable owner.
    http3_client_body_budget::lease_type http3_body_budget_;
    std::pmr::string buffered_;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::pmr::string pending_;
    std::size_t offset_{0};
    std::size_t buffered_limit_{default_max_buffered_body_bytes};
    std::exception_ptr failure_;
    std::optional<std::uint8_t> error_code_;
    std::size_t references_{1};
    bool head_ready_{false};
    bool complete_{false};
    bool abandoned_{false};
    bool incremental_read_{false};
    bool collect_all_{false};
    bool body_decode_required_{false};
    // Any informational/final response event proves the peer processed some
    // part of the request; GOAWAY/REQUEST_REJECTED must not replay it.
    bool http3_response_started_{false};
    http_client_response_transport transport_{http_client_response_transport::unassigned};
    http3_client_connection* http3_connection_{nullptr};
    std::uint64_t http3_request_id_{0};
    std::optional<::ruvia::http2_received_data_credit> http2_data_credit_{};
    std::size_t connection_index_{0};
    std::uint64_t request_id_{0};
    std::uint64_t cancellation_id_{0};
    std::uint64_t stream_id_{0};
    // Declared last so the operation scope closes while every field borrowed
    // by a body coroutine is alive.
    ::ruvia::operation_scope body_operation_scope_;
    std::optional<http_push_request> promised_request_{};
    bool push_response_taken_{false};
    ::ruvia::operation_scope push_response_scope_;

private:
    void promote_pending_data();
    void detach_transport_bindings() noexcept;

    http_client_response_memory_domain* memory_domain_{};
    http_client_response_state* previous_memory_state_{};
    http_client_response_state* next_memory_state_{};

    friend class http_client_response_memory_domain;
};

}  // namespace ruvia::detail
