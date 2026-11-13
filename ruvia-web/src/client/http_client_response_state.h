#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "body/http_body_buffer.h"
#include "client/http_client_request_storage.h"

namespace ruvia {
class response_stream_writer;
}

namespace ruvia::detail {

class http_client_pool;
class http_client_response_memory_domain;

inline std::uint64_t next_nonzero_id(std::uint64_t& value) noexcept {
    if (++value == 0) {
        ++value;
    }
    return value;
}

// Owned by the response memory domain, not by a connection/session arena.
// At most one application chunk is queued; wire drivers borrow it until their
// protocol cursor commits it. Terminal wakeups never free a pending SSL span.
class http_client_output_queue final {
public:
    http_client_output_queue(const worker_handle& worker_value, std::pmr::memory_resource* resource)
        : data_(worker_value),
          space_(worker_value),
          chunk_(resource) {}
    void notify_data() noexcept {
        data_.notify();
        if (wake_ != nullptr) {
            wake_(wake_target_);
        }
    }
    void acknowledge_chunk() noexcept {
        chunk_ready_ = false;
        http_clear_pmr_string_retaining_small(chunk_, http_body_buffer_bytes);
        space_.notify();
    }
    void finish() noexcept {
        ended_ = true;
        std::pmr::string(chunk_.get_allocator()).swap(chunk_);
        space_.notify();
    }
    void stop() noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        notify_data();
        space_.notify();
    }
    void* wake_target_{};
    void (*wake_)(void*) noexcept {};
    worker_signal data_;
    worker_signal space_;
    std::pmr::string chunk_;
    bool chunk_ready_{};
    bool end_requested_{};
    bool completion_pending_{};
    bool ended_{};
    bool stopped_{};
    bool operation_active_{};
    ::ruvia::operation_scope output_scope_;
};

// Transfers the response of an exchange or a push to its consumer once, after
// the final head arrived. Embedded in the address-stable response state; one
// started receive runs at a time, and the owner retiring closes the cold ones.
class http_client_response_handoff final {
public:
    [[nodiscard]] bool taken() const noexcept {
        return taken_;
    }
    [[nodiscard]] bool has_pending_receive() const noexcept {
        return scope_.has_pending_operations();
    }
    // A cold receive of `pin`'s response, bound to this handoff's scope;
    // `head_message` (static text) reports a failure before the head.
    [[nodiscard]] task<http_client_response> receive(http_client_response pin, std::string_view head_message);
    // The owner retires: its pending receives close, and once the response was
    // transferred the owner's handle no longer consumes it.
    void release(http_client_response& owner) noexcept;

private:
    [[nodiscard]] task<http_client_response> receive_owned(http_client_response pin, std::string_view head_message);

    ::ruvia::operation_scope scope_;
    bool taken_{false};
    bool active_{false};
};

class http_client_tunnel_state final {
public:
    http_client_tunnel_state(const worker_handle& worker_value, std::pmr::memory_resource* resource, http_client_tunnel_config config)
        : output_(worker_value, resource),
          config_(config) {}
    http_client_output_queue output_;
    http_client_tunnel_config config_;
    bool udp_{};
    bool accepted_{};
    bool receive_ended_{};
};

class http_client_upload_state final {
public:
    http_client_upload_state(const worker_handle& worker_value, std::pmr::memory_resource* resource, http_client_upload_config config)
        : output_(worker_value, resource),
          config_(config),
          trailers_(resource) {}
    http_client_upload_state(worker_handle&&, std::pmr::memory_resource*, http_client_upload_config) = delete;
    http_client_output_queue output_;
    http_client_upload_config config_;
    std::pmr::vector<http_header> trailers_;
    // Upload-only failures belong to the writer, not to an otherwise valid
    // response. HTTP/1 may record them while the response is still arriving;
    // late failures in any protocol must preserve an already validated EOF.
    std::exception_ptr failure_;
    bool content_released_{};
    http_client_response_handoff response_;
};

// Composite per-stream deadline timer shared by HTTP/2 and HTTP/3 client
// streams: combines the operation timeout with active read/write inactivity
// deadlines and reschedules lazily when a deadline only moves later.
struct http_client_stream_timer final {
    using time_point = std::chrono::steady_clock::time_point;

    std::optional<time_point> read_deadline_;
    std::optional<time_point> write_deadline_;
    std::optional<time_point> timer_at_;
    worker_timer_registration timer_;

    void cancel() noexcept {
        timer_.cancel();
        timer_at_.reset();
    }

    [[nodiscard]] std::optional<time_point> earliest_deadline(const operation_timeout& timeout) const noexcept {
        auto deadline = timeout.deadline();
        for (const auto bound : {read_deadline_, write_deadline_}) {
            if (bound && (!deadline || *bound < *deadline)) {
                deadline = bound;
            }
        }
        return deadline;
    }

    [[nodiscard]] bool expired(const operation_timeout& timeout,
        time_point now = std::chrono::steady_clock::now()) const noexcept {
        return timeout.expired() || (read_deadline_ && *read_deadline_ <= now) ||
               (write_deadline_ && *write_deadline_ <= now);
    }

    template <typename callback_type>
    void schedule(const worker_handle& worker, const operation_timeout& timeout, callback_type&& on_expired) {
        const auto deadline = earliest_deadline(timeout);
        if (!deadline || (timer_at_ && *timer_at_ <= *deadline)) {
            return;
        }
        timer_.cancel();
        timer_at_.reset();
        if (worker.schedule_timer(timer_, *deadline,
                [on_expired = std::forward<callback_type>(on_expired)](worker_timer_outcome outcome) mutable noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        on_expired();
                    }
                }) == worker_timer_schedule_status::scheduled) {
            timer_at_ = deadline;
        }
    }

    template <typename timeout_source_type, typename expired_type, typename error_type>
    void schedule_with_retry(const worker_handle& worker, const timeout_source_type& timeout_source,
        expired_type on_expired, error_type on_error) {
        const auto current_timeout = [&timeout_source]() noexcept -> const operation_timeout& {
            if constexpr (std::is_pointer_v<timeout_source_type>) {
                return *timeout_source;
            } else {
                return timeout_source;
            }
        };
        schedule(worker, current_timeout(),
            [this, &worker, &timeout_source, current_timeout, on_expired, on_error]() noexcept {
                timer_at_.reset();
                if (expired(current_timeout())) {
                    on_expired();
                    return;
                }
                try {
                    schedule_with_retry(worker, timeout_source, on_expired, on_error);
                } catch (...) {
                    on_error(std::current_exception());
                }
            });
    }
};

template <typename wake_fn_type, typename stop_predicate_type, typename release_fn_type,
    typename send_chunk_fn_type, typename finish_fn_type>
[[nodiscard]] task<void> run_multiplexed_client_upload(const worker_handle& worker,
    const http_client_request_storage& request, http_client_output_queue& upload, worker_signal& stream_signal,
    wake_fn_type&& wake_fn, stop_predicate_type&& stop_predicate, release_fn_type&& release_content,
    send_chunk_fn_type&& send_chunk, finish_fn_type&& finish_upload) {
    upload.wake_target_ = &wake_fn;
    upload.wake_ = [](void* target) noexcept { (*static_cast<std::remove_reference_t<wake_fn_type>*>(target))(); };
    struct upload_wake_guard final {
        http_client_output_queue& upload_;
        ~upload_wake_guard() {
            upload_.wake_ = nullptr;
            upload_.wake_target_ = nullptr;
        }
    } wake_guard{upload};
    worker_timer_registration continue_timer;
    if (request.upload() != nullptr && !request.upload()->content_released_) {
        (void)worker.schedule_timer(continue_timer,
            worker_timer_deadline_after(request.upload()->config_.continue_timeout_.value()),
            [&upload, policy = request.upload()](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired && !upload.stopped_) {
                    policy->content_released_ = true;
                    upload.notify_data();
                }
            });
    }
    while (!upload.ended_ && !upload.stopped_ && !stop_predicate()) {
        if (request.is_tunnel() ? !request.tunnel()->accepted_ : !request.upload()->content_released_) {
            co_await stream_signal.wait();
            continue;
        }
        continue_timer.cancel();
        if (!release_content()) {
            break;
        }
        if (upload.chunk_ready_) {
            if (co_await send_chunk(std::string_view(upload.chunk_))) {
                upload.acknowledge_chunk();
            }
        } else if (upload.end_requested_) {
            if (!(co_await finish_upload())) {
                break;
            }
            if (!stop_predicate() && !upload.stopped_ && !upload.ended_) {
                upload.finish();
            }
        } else {
            co_await stream_signal.wait();
        }
    }
}

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
    explicit http_client_response_state(http_client_response_memory_domain& memory_domain);

    // Body algorithms run on the address-stable storage owner. The public
    // facade tracks cold frames in body_operation_scope_; the algorithms claim
    // body_operation_active_ only when their lazy task starts.
    template <typename view>
    [[nodiscard]] task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(response_stream_writer* output = nullptr);
    [[nodiscard]] task<http_client_response_bytes> read_all(std::size_t max_bytes);
    void retain_reference() noexcept;
    void release_reference() noexcept;
    [[nodiscard]] http_client_response_memory_domain& memory_domain() const noexcept {
        return memory_domain_;
    }
    // Response storage is worker-local: throws std::logic_error(`message`)
    // when called off the worker that owns it.
    void require_owner_worker(const char* message) const;
    void notify_producer_space() noexcept;
    // The consumer's wait for the final head: rethrows the producer's failure
    // or reports `head_message` for its error code. An encoded body is decoded
    // completely before the head is returned, so its failure is observed here.
    [[nodiscard]] task<void> await_head(std::string_view head_message);
    void rethrow_response_failure() const;
    void discard_response_body() noexcept;
    void discard_failed_encoded_response() noexcept;
    [[nodiscard]] task<void> dispatch_informational(const operation_timeout& timeout);
    void request_informational_stop() noexcept {
        informational_stop_requested_ = informational_stop_requested_ || informational_active_;
        if (informational_abort_) {
            informational_abort_->request_stop();
        }
    }
    void release_consumed_body_prefix();
    [[nodiscard]] bool append_received_body(std::string_view bytes_value, std::size_t max_response_bytes) {
        const auto retained = buffered_.size() - offset_ + pending_.size();
        if (collect_all_ && bytes_value.size() > max_response_bytes - (std::min)(retained, max_response_bytes)) {
            discard_response_body();
            return false;
        }
        // The consumer's last borrowed view lives in buffered, not pending.
        // Appending producer output must never relocate that view.
        pending_.append(bytes_value);
        data_signal_.notify();
        return true;
    }

    // One retention path: consume response-owned fields or copy borrowed fields.
    // Owned fields must already use this response's resource. A 100 (Continue)
    // releases request content gated on it.
    template <typename fields_type>
        requires(std::is_same_v<std::remove_cvref_t<fields_type>, std::span<const http_header>> ||
                 std::is_same_v<fields_type, std::pmr::vector<http_header>>)
    void retain_informational(http_status_code status, fields_type&& fields_value,
        std::optional<http_client_request_content_signal> request_content_signal) {
        constexpr bool transferring = std::is_same_v<fields_type, std::pmr::vector<http_header>>;
        http_client_informational_response head(status, resource_);
        if constexpr (transferring) {
            if (fields_value.get_allocator().resource() != resource_) {
                throw std::logic_error("owned informational fields use a different response resource");
            }
        } else {
            head.headers_.reserve(fields_value.size());
        }
        // Fieldless heads still consume response-budgeted vector storage.
        // Each head's own field count is bounded by its decoder; across heads
        // only the retained bytes are bounded, so many small interim heads
        // (e.g. repeated 103 Early Hints) stay acceptable.
        auto budget = informational_budget_;
        for (const auto& field : fields_value) {
            if (!budget.add(field.name().size()) || !budget.add(field.value().size()) ||
                !budget.add(http_field_line_overhead)) {
                throw http_client_error(http_client_error::code_type::protocol_error, "informational response metadata exceeds byte limit");
            }
            if constexpr (!transferring) {
                head.headers_.emplace_back(field.name(), field.value(), resource_);
            }
        }
        if constexpr (transferring) {
            head.headers_ = std::move(fields_value);
        }
        informational_.push_back(std::move(head));
        informational_budget_ = budget;
        if (upload_ && request_content_signal == http_client_request_content_signal::continue_value) {
            upload_->content_released_ = true;
            upload_->output_.notify_data();
        }
    }

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
    std::pmr::memory_resource* resource_;
    http_received_status status_{http_status::ok};
    http_known_method request_method_{http_known_method::unknown};
    http_protocol_version protocol_version_{http_protocol_version::http11};
    std::optional<http_response_body_plan> response_body_plan_{};
    http_client_response_framing framing_{http_client_response_framing::none};
    std::optional<std::uint64_t> content_length_;
    http_client_connection_info connection_info_;
    std::optional<timeout_duration> effective_connect_timeout_;
    std::optional<timeout_duration> effective_read_timeout_;
    std::optional<timeout_duration> effective_write_timeout_;
    std::optional<timeout_duration> effective_acquire_timeout_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    stop_token informational_stop_;
    std::optional<stop_source> informational_abort_;
    bool informational_active_{false};
    bool informational_stop_requested_{false};
    bool decode_content_{true};
    std::pmr::vector<http_client_informational_response> informational_;
    // One aggregate byte budget across every retained interim head.
    http_field_section_budget informational_budget_;
    std::size_t informational_delivered_{};
    std::pmr::vector<http_header> headers_;
    std::pmr::vector<http_header> trailers_;
    std::pmr::string buffered_;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::pmr::string pending_;
    std::size_t offset_{0};
    std::size_t buffered_limit_{default_max_buffered_body_bytes};
    std::exception_ptr failure_;
    // The pool retired before the exchange completed; consumers report
    // http_client_error::closing with their own operation message.
    bool pool_closed_{false};
    std::size_t references_{1};
    bool head_ready_{false};
    bool complete_{false};
    bool abandoned_{false};
    bool collect_all_{false};
    bool body_operation_active_{false};
    bool body_decode_required_{false};
    // Content-Encoding parsed with the final head while decoding is pending.
    std::optional<http_content_coding_field_result> content_coding_{};
    http_client_response_transport transport_{http_client_response_transport::unassigned};
#ifdef RUVIA_ENABLE_HTTP2
    std::optional<::ruvia::http2_received_data_credit> http2_data_credit_{};
#endif
    std::size_t http3_data_credit_{0};
    std::optional<std::pmr::deque<std::pmr::string>> http3_datagrams_;
    std::size_t http3_datagram_bytes_{0};
    bool http3_native_datagrams_{false};
    // Pool position and pool-unique serial of the connection owner carrying
    // this response; positions are reused once their owner is reclaimed.
    std::size_t connection_index_{0};
    std::uint64_t connection_serial_{0};
    std::uint64_t connection_generation_{0};
    // The request used the alias route: its response may update Alt-Svc.
    bool alias_route_{true};
    std::uint64_t request_id_{0};
    std::uint64_t cancellation_id_{0};
    std::uint64_t stream_id_{0};
    std::optional<http_push_request> promised_request_{};
    http_client_response_handoff push_response_;
    // Declared after every field a body coroutine borrows, so the operation
    // scope closes while they are alive.
    ::ruvia::operation_scope body_operation_scope_;

private:
    void promote_pending_data();
    void detach_transport_bindings() noexcept;

    http_client_response_memory_domain& memory_domain_;
    http_client_response_state* previous_memory_state_{};
    http_client_response_state* next_memory_state_{};

    friend class http_client_response_memory_domain;
};

// Publish both identities before arm(), which may deliver cancellation inline.
// The owner must declare this after any generation/stream guard so callback
// retirement precedes unpublishing the operation.
template <typename target_type>
class http_client_response_cancellation final {
public:
    http_client_response_cancellation(const std::shared_ptr<target_type>& target,
        std::uint64_t& identity, http_client_response_state& response)
        : response_(response),
          registration_(target, identity) {
        response_.cancellation_id_ = registration_.id();
    }
    http_client_response_cancellation(std::shared_ptr<target_type>&&, std::uint64_t&, http_client_response_state&) = delete;
    http_client_response_cancellation(const std::shared_ptr<target_type>&&, std::uint64_t&, http_client_response_state&) = delete;
    http_client_response_cancellation(const http_client_response_cancellation&) = delete;
    http_client_response_cancellation& operator=(const http_client_response_cancellation&) = delete;
    http_client_response_cancellation(http_client_response_cancellation&&) = delete;
    http_client_response_cancellation& operator=(http_client_response_cancellation&&) = delete;
    ~http_client_response_cancellation() {
        registration_.reset();
        if (response_.cancellation_id_ == registration_.id()) {
            response_.cancellation_id_ = 0;
        }
    }
    void arm(const stop_token& stop) {
        registration_.arm(stop);
    }

private:
    http_client_response_state& response_;
    worker_cancellation_registration<target_type> registration_;
};

}  // namespace ruvia::detail
