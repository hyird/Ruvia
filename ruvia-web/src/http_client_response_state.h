#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "connection_lifecycle.h"
#include "http2_push.h"
#include "http_client.h"
#include "http_client_request_storage.h"
#include "http_client_response_head.h"
#include "http_client_types.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_io_buffer.h"
#include "http_message.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_response_body.h"
#include "http_status.h"
#include "io_operation.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "stop_token.h"
#include "task.h"
#include "timeout_duration.h"
#include "worker_cancellation.h"
#include "worker_ref.h"
#include "worker_signal.h"
#include "worker_timer.h"

#ifdef RUVIA_ENABLE_HTTP2
#include "http2_connection.h"
#endif
#ifdef RUVIA_ENABLE_HTTP3
#include "http3_connection.h"
#endif

namespace ruvia::detail {

class http_client_result_budget_domain final {
public:
    explicit http_client_result_budget_domain(http_client_result_budget_config config);

    http_client_result_budget_domain(const http_client_result_budget_domain&) = delete;
    http_client_result_budget_domain& operator=(const http_client_result_budget_domain&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;

    // Only the owning worker accesses receive accounting. Escaped results
    // independently use the thread-safe retained-result counter above.
    [[nodiscard]] bool reserve_in_flight(std::size_t bytes) noexcept;
    void release_in_flight(std::size_t bytes) noexcept;

private:
    const std::size_t max_retained_bytes_;
    std::atomic<std::size_t> retained_bytes_{0};
    const std::size_t max_in_flight_bytes_;
    std::size_t in_flight_bytes_{0};
};

// Move-only reservation carried by an escaped result. It owns the budget domain
// independently of the client and releases its exact charge at destruction.
class http_client_result_budget_lease final {
public:
    http_client_result_budget_lease(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease& operator=(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept;
    ~http_client_result_budget_lease();

    [[nodiscard]] static std::optional<http_client_result_budget_lease> try_acquire(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes) noexcept;
    [[nodiscard]] std::pair<std::shared_ptr<http_client_result_budget_domain>, std::size_t> release() && noexcept {
        return {std::move(domain_), std::exchange(bytes_, 0)};
    }

private:
    http_client_result_budget_lease(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept;

    // Keep the domain alive until after releasing the reservation.
    std::shared_ptr<http_client_result_budget_domain> domain_;
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia {
class operation_deadline;
}  // namespace ruvia

namespace ruvia::detail {

class client_request_policy;
class http_client_response_state;
struct http_client_websocket_handshake;

// One received final response head, after its protocol driver stored the
// response fields in the state's headers_.
struct http_client_final_head final {
    http_received_status status_;
    http_protocol_version protocol_version_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    // Target of a request addressed to the fixed origin, whose jar retains the
    // head's Set-Cookie fields; nullopt for a tunnel to another authority.
    std::optional<std::string_view> origin_target_;
    // The protocol's verdict that this head opens the request's tunnel: a 2xx
    // to CONNECT, or the HTTP/1 101 upgrade of CONNECT-UDP (RFC 9298 section 3).
    bool opens_tunnel_{false};
};

// Stores a received HTTP/2 or HTTP/3 final head in the state: its fields
// (moved when they already use the state's resource), Content-Length and the
// framing its status implies for the request method.
void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head);

// The final-head publication shared by HTTP/1, HTTP/2 and HTTP/3: stops an upload the
// response completed, records status, version and body plan, retains origin
// cookies, settles tunnel acceptance (rejecting an invalid CONNECT-UDP head),
// selects collection of an encoded body, then wakes the head consumer.
void publish_http_client_final_head(http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head);

// Validates an RFC 8441 Extended CONNECT WebSocket response head (HTTP/2 or
// HTTP/3) and records the negotiated subprotocol and compression on `websocket`.
void accept_extended_connect_websocket_response(http_client_websocket_handshake& websocket, http_status_code status, std::span<const http_header> headers, bool stream_ended, std::string_view error_message);

// Ends a received body: decodes a collected Content-Encoding within the
// response byte limit, failing with timeout when `timeout` expired before or
// during decoding.
void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout, bool content_semantics_present, std::size_t max_decoded_bytes);

class http_client_result_budget_domain;

// Response storage and in-flight budget accounting of one client pool, plus the
// registry of live response states so the pool can retire their transport
// borrows on join. Worker-affine, reference counted by the pool root and every
// live state.
class http_client_response_memory_domain final {
public:
    class deleter_type final {
    public:
        void operator()(http_client_response_memory_domain* domain) const noexcept;
    };

    using owner = std::unique_ptr<http_client_response_memory_domain, deleter_type>;

    [[nodiscard]] static owner create(const worker_ref& worker, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);

    http_client_response_memory_domain(const http_client_response_memory_domain&) = delete;
    http_client_response_memory_domain& operator=(const http_client_response_memory_domain&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }

    void charge_in_flight(std::size_t bytes_value);
    void release_in_flight(std::size_t bytes_value) noexcept;

    [[nodiscard]] http_client_response_state* create_state(http_client_pool& pool);
    void destroy_state(http_client_response_state* state) noexcept;
    void detach_transport_bindings(http_client_pool& pool) noexcept;

private:
    enum class phase_type : unsigned char { prepared,
        active,
        retired };

    http_client_response_memory_domain(const worker_ref& worker_value, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    ~http_client_response_memory_domain();

    void destroy() noexcept;
    void require_current() const noexcept;
    void attach_state(http_client_response_state& state) noexcept;
    void unlink_state(http_client_response_state& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void release_root() noexcept;

    worker_ref worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_state* state_head_{};
    std::size_t references_{1};
    phase_type phase_{phase_type::prepared};

    friend class deleter_type;
    friend class http_client_response_state;
};

class http_client_in_flight_reservation final {
public:
    constexpr http_client_in_flight_reservation() noexcept = default;
    explicit constexpr http_client_in_flight_reservation(http_client_response_memory_domain* domain) noexcept :
        domain_(domain) {}
    ~http_client_in_flight_reservation() {
        clear();
    }
    http_client_in_flight_reservation(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation& operator=(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation(http_client_in_flight_reservation&& other) noexcept :
        domain_(std::exchange(other.domain_, nullptr)),
        bytes_(std::exchange(other.bytes_, 0)) {}
    http_client_in_flight_reservation& operator=(http_client_in_flight_reservation&& other) noexcept {
        if (this != &other) {
            clear();
            domain_ = std::exchange(other.domain_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    void resize_to(std::size_t target_bytes) {
        if (domain_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            domain_->charge_in_flight(target_bytes - bytes_);
        } else {
            domain_->release_in_flight(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (domain_ != nullptr && target_bytes < bytes_) {
            domain_->release_in_flight(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (domain_ != nullptr && bytes_ != 0) {
            domain_->release_in_flight(bytes_);
            bytes_ = 0;
        }
    }

    void swap(http_client_in_flight_reservation& other) noexcept {
        std::swap(domain_, other.domain_);
        std::swap(bytes_, other.bytes_);
    }

private:
    http_client_response_memory_domain* domain_{nullptr};
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia {
class response_stream_writer;
}

namespace ruvia::detail {

struct http_client_route;

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
    http_client_output_queue(const worker_ref& worker_value) :
        data_(worker_value),
        space_(worker_value) {}
    void notify_data() noexcept {
        data_.notify();
        if (wake_ != nullptr) {
            wake_(wake_target_);
        }
    }
    void acknowledge_chunk() noexcept {
        chunk_ready_ = false;
        http_clear_string_retaining_small(chunk_, http_body_buffer_bytes);
        space_.notify();
    }
    void finish() noexcept {
        ended_ = true;
        std::string(chunk_.get_allocator()).swap(chunk_);
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
    std::string chunk_;
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
    // `head_message` (static text) reports a failure before the head. Its
    // own `operation` interrupts only this wait.
    [[nodiscard]] task<http_client_response> receive(http_client_response pin, std::string_view head_message, operation_options operation);
    // The owner retires: its pending receives close, and once the response was
    // transferred the owner's handle no longer consumes it.
    void release(http_client_response& owner) noexcept;

private:
    [[nodiscard]] task<http_client_response> receive_owned(http_client_response pin, std::string_view head_message, operation_options operation);

    ::ruvia::operation_scope scope_;
    bool taken_{false};
    bool active_{false};
};

class http_client_tunnel_state final {
public:
    http_client_tunnel_state(const worker_ref& worker_value, http_client_tunnel_config config) :
        output_(worker_value),
        config_(config) {}
    http_client_output_queue output_;
    http_client_tunnel_config config_;
    bool udp_{};
    bool accepted_{};
    bool receive_ended_{};
};

class http_client_upload_state final {
public:
    http_client_upload_state(const worker_ref& worker_value, http_client_upload_config config) :
        output_(worker_value),
        config_(config) {}
    http_client_upload_state(worker_ref&&, http_client_upload_config) = delete;
    http_client_output_queue output_;
    http_client_upload_config config_;
    std::vector<http_header> trailers_;
    // The caller stopped sending through abandon(); the response stays valid.
    bool abandoned_{};
    // Upload-only failures belong to the writer, not to an otherwise valid
    // response. HTTP/1 may record them while the response is still arriving;
    // late failures in any protocol must preserve an already validated EOF.
    std::exception_ptr failure_;
    bool content_released_{};
    http_client_response_handoff response_;
    std::unique_ptr<http_client_request> redirect_request_;
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

    [[nodiscard]] std::optional<time_point> earliest_deadline(const operation_deadline& timeout) const noexcept {
        auto deadline = timeout.deadline();
        for (const auto bound : {read_deadline_, write_deadline_}) {
            if (bound && (!deadline || *bound < *deadline)) {
                deadline = bound;
            }
        }
        return deadline;
    }

    [[nodiscard]] bool expired(const operation_deadline& timeout, time_point now = std::chrono::steady_clock::now()) const noexcept {
        return timeout.expired() || (read_deadline_ && *read_deadline_ <= now) || (write_deadline_ && *write_deadline_ <= now);
    }

    template <typename callback_type>
    void schedule(const worker_ref& worker, const operation_deadline& timeout, callback_type&& on_expired) {
        const auto deadline = earliest_deadline(timeout);
        if (!deadline || (timer_at_ && *timer_at_ <= *deadline)) {
            return;
        }
        timer_.cancel();
        timer_at_.reset();
        if (worker.schedule_timer(timer_, *deadline, [on_expired = std::forward<callback_type>(on_expired)](worker_timer_outcome outcome) mutable noexcept {
            if (outcome == worker_timer_outcome::expired) {
                on_expired();
            }
        }) == worker_timer_schedule_status::scheduled) {
            timer_at_ = deadline;
        }
    }

    template <typename timeout_source_type, typename expired_type, typename error_type>
    void schedule_with_retry(const worker_ref& worker, const timeout_source_type& timeout_source, expired_type on_expired, error_type on_error) {
        const auto current_timeout = [&timeout_source]() noexcept -> const operation_deadline& {
            if constexpr (std::is_pointer_v<timeout_source_type>) {
                return *timeout_source;
            } else {
                return timeout_source;
            }
        };
        schedule(worker, current_timeout(), [this, &worker, &timeout_source, current_timeout, on_expired, on_error]() noexcept {
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

template <typename wake_fn_type, typename stop_predicate_type, typename release_fn_type, typename send_chunk_fn_type, typename finish_fn_type>
[[nodiscard]] task<void> run_multiplexed_client_upload(const worker_ref& worker, const http_client_request_storage& request, http_client_output_queue& upload, worker_signal& stream_signal, wake_fn_type&& wake_fn, stop_predicate_type&& stop_predicate, release_fn_type&& release_content, send_chunk_fn_type&& send_chunk, finish_fn_type&& finish_upload) {
    upload.wake_target_ = &wake_fn;
    upload.wake_ = [](void* target) noexcept {
        (*static_cast<std::remove_reference_t<wake_fn_type>*>(target))();
    };
    struct upload_wake_guard final {
        http_client_output_queue& upload_;
        ~upload_wake_guard() {
            upload_.wake_ = nullptr;
            upload_.wake_target_ = nullptr;
        }
    } wake_guard{upload};
    worker_timer_registration continue_timer;
    if (request.upload() != nullptr && !request.upload()->content_released_) {
        (void)worker.schedule_timer(continue_timer, deadline_after(request.upload()->config_.continue_timeout_.value()), [&upload, policy = request.upload()](worker_timer_outcome outcome) noexcept {
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
    // body_operation_active_ only when their lazy task starts. `operation` is
    // validated by the facade and timed from here.
    template <typename view>
    [[nodiscard]] task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> consume_body(operation_options operation, response_stream_writer* output = nullptr);
    [[nodiscard]] task<http_client_response_bytes> read_all(std::size_t max_bytes, operation_options operation);
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
    // The running consumer (body read or head receive) or output operation's
    // own interruption, reported as http_client_error timeout / cancelled.
    [[nodiscard]] bool read_interrupted() const noexcept {
        return read_operation_ != nullptr && read_operation_->interrupted();
    }
    void throw_if_read_interrupted() const;
    void throw_if_output_interrupted() const;
    // A forward's downstream response_stream_writer step runs with the time
    // left in `steps`; http_client_error(timeout) once none is left.
    [[nodiscard]] operation_options downstream_step(const operation_steps& steps) const;
    // Rethrows the exception being handled, reporting a downstream step that
    // `steps` interrupted as this client's http_client_error.
    [[noreturn]] void rethrow_downstream_failure(const operation_steps& steps) const;
    void discard_response_body() noexcept;
    void discard_failed_encoded_response() noexcept;
    [[nodiscard]] task<void> dispatch_informational(const operation_deadline& timeout);
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
        requires(std::is_same_v<std::remove_cvref_t<fields_type>, std::span<const http_header>> || std::is_same_v<fields_type, std::vector<http_header>>)
    void retain_informational(http_status_code status, fields_type&& fields_value, std::optional<http_client_request_content_signal> request_content_signal) {
        constexpr bool transferring = std::is_same_v<fields_type, std::vector<http_header>>;
        http_client_informational_response head(status);
        if constexpr (!transferring) {
            head.headers_.reserve(fields_value.size());
        }
        // Fieldless heads still consume response-budgeted vector storage.
        // Each head's own field count is bounded by its decoder; across heads
        // only the retained bytes are bounded, so many small interim heads
        // (e.g. repeated 103 Early Hints) stay acceptable.
        auto budget = informational_budget_;
        for (const auto& field : fields_value) {
            if (!budget.add(field.name().size()) || !budget.add(field.value().size()) || !budget.add(http_field_line_overhead)) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "informational response metadata exceeds byte limit");
            }
            if constexpr (!transferring) {
                head.headers_.emplace_back(field.name(), field.value());
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
        return tunnel_ ? &tunnel_->output_ : (upload_ ? &upload_->output_ : nullptr);
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
    http_received_status status_{http_status::ok};
    http_known_method request_method_{http_known_method::unknown};
    http_protocol_version protocol_version_{http_protocol_version::http11};
    std::optional<http_response_body_plan> response_body_plan_{};
    http_client_response_framing framing_{http_client_response_framing::none};
    std::optional<std::uint64_t> content_length_;
    http_client_connection_info connection_info_;
    bool request_written_{false};
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
    std::vector<http_client_informational_response> informational_;
    // One aggregate byte budget across every retained interim head.
    http_field_section_budget informational_budget_;
    std::size_t informational_delivered_{};
    std::vector<http_header> headers_;
    std::vector<http_header> trailers_;
    std::string buffered_;
    // Producers only append here. The consumer swaps it with `buffered` before
    // returning a view, so later network progress cannot invalidate that view.
    std::string pending_;
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
    // The interrupts of the running consumer and output operations; null
    // while none runs. Their source routes stop requests on this worker.
    const operation_interrupt* read_operation_{};
    const operation_interrupt* output_operation_{};
    bool body_decode_required_{false};
    // Content-Encoding parsed with the final head while decoding is pending.
    std::optional<http_content_coding_field_result> content_coding_{};
    http_client_response_transport transport_{http_client_response_transport::unassigned};
#ifdef RUVIA_ENABLE_HTTP2
    std::optional<::ruvia::http2_received_data_credit> http2_data_credit_{};
#endif
#ifdef RUVIA_ENABLE_HTTP3
    // Receive credit of buffered response DATA, returned as the consumer
    // takes it (RFC 9000 §4.1).
    std::optional<::ruvia::http3_received_data_credit> http3_data_credit_{};
#endif
    std::optional<std::deque<std::string>> http3_datagrams_;
    std::size_t http3_datagram_bytes_{0};
    bool http3_native_datagrams_{false};
    // Pool position and pool-unique serial of the connection owner carrying
    // this response; positions are reused once their owner is reclaimed.
    std::size_t connection_index_{0};
    std::uint64_t connection_serial_{0};
    std::uint64_t connection_generation_{0};
    // The request's route while its exchange runs: a final head may teach it
    // Alt-Svc. Null for pushes and once the request finished.
    const http_client_route* route_{};
    std::uint64_t request_id_{0};
    std::uint64_t cancellation_id_{0};
    std::uint64_t stream_id_{0};
    std::optional<http_push_request> promised_request_{};
    http_client_response_handoff push_response_;
    operation_interrupt_source interrupt_source_;
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

enum class http_client_operation_lane : std::uint8_t {
    read,
    output,
};

// Binds one operation's own timeout and stop token to a response lane while it
// runs. A read interruption wakes the consumer and keeps buffered bytes; an
// output interruption stops the output and abandons the exchange or tunnel,
// since a partly sent chunk cannot be withdrawn. An already-stopped token
// throws http_client_error(cancelled) before anything is armed.
class http_client_operation_interruption final {
public:
    http_client_operation_interruption(http_client_response_state& state, http_client_operation_lane lane, const operation_options& operation);
    ~http_client_operation_interruption();
    http_client_operation_interruption(const http_client_operation_interruption&) = delete;
    http_client_operation_interruption& operator=(const http_client_operation_interruption&) = delete;

private:
    static void wake_reader(void* state) noexcept;
    static void abort_output(void* state) noexcept;

    http_client_response_state& state_;
    http_client_operation_lane lane_;
    operation_interrupt interrupt_;
};

// Publish both identities before arm(), which may deliver cancellation inline.
// The owner must declare this after any generation/stream guard so callback
// retirement precedes unpublishing the operation.
template <typename target_type>
class http_client_response_cancellation final {
public:
    http_client_response_cancellation(const std::shared_ptr<target_type>& target, std::uint64_t& identity, http_client_response_state& response) :
        response_(response),
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
