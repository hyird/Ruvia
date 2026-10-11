#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_informational_response.h"
#include "ruvia/web/http_client_response_bytes.h"

#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"

namespace ruvia {
class response_stream_writer;
}

namespace ruvia::detail {

class http_client_pool;
class http_client_request_storage;
class http_client_response_memory_domain;
class http_client_result_budget_domain;

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
    // facade tracks cold frames in body_operation_scope_; the algorithms claim
    // body_operation_active_ only when their lazy task starts.
    template <typename view>
    [[nodiscard]] task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(response_stream_writer* output = nullptr);
    [[nodiscard]] task<http_client_response_bytes> read_all(std::size_t max_bytes);
    void retain_reference() noexcept;
    void release_reference() noexcept;
    [[nodiscard]] http_client_response_memory_domain* memory_domain() const noexcept {
        return memory_domain_;
    }
    void notify_producer_space() noexcept;
    void discard_pending_body() noexcept;
    void discard_response_body() noexcept;
    void release_consumed_body_prefix();

    // The one interim-head retention path; every protocol passes its parsed fields.
    void retain_informational(http_status_code status, std::span<const http_header> fields);

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
    bool body_operation_active_{false};
    bool body_decode_required_{false};
    // Content-Encoding parsed with the final head while decoding is pending.
    std::optional<http_content_coding_field_result> content_coding_{};
    std::size_t connection_index_{0};
    std::uint64_t cancellation_id_{0};
    // Declared last so the operation scope closes while every field borrowed
    // by a body coroutine is alive.
    ::ruvia::operation_scope body_operation_scope_;

private:
    void promote_pending_data();
    void detach_transport_bindings() noexcept;

    http_client_response_memory_domain* memory_domain_{};
    http_client_response_state* previous_memory_state_{};
    http_client_response_state* next_memory_state_{};

    friend class http_client_response_memory_domain;
};

}  // namespace ruvia::detail
