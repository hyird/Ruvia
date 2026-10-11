#include "http3/http3_server_connection.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http_interim_response.h"

#include "http3/http3_connection_driver.h"
#include "http3/http3_server_request_entry.h"
#include "router/route_table.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] http_status_code rejection_status(http3_sans_io_session_engine::rejection_type rejection) {
    using rejection_type = http3_sans_io_session_engine::rejection_type;
    switch (rejection) {
        case rejection_type::expectation_unsupported:
            return http_status::expectation_failed;
        case rejection_type::body_too_large:
            return http_status::content_too_large;
        case rejection_type::connect_unsupported:
        case rejection_type::streaming_unsupported:
        case rejection_type::websocket_unsupported:
        case rejection_type::response_stream_unsupported:
            return http_status::not_implemented;
        case rejection_type::in_flight_body_capacity:
        case rejection_type::worker_body_budget_exhausted:
            return http_status::service_unavailable;
        case rejection_type::none:
            break;
    }
    throw std::invalid_argument("HTTP/3 rejection requires a status mapping");
}

[[nodiscard]] bool connection_failure(const http3_server_stream_input::result_type& result) noexcept {
    return result.protocol_.scope_ == http3_connection_error_scope::connection ||
           result.protocol_.status_ == http3_connection_status::connection_error;
}

}  // namespace

struct http3_server_connection::entry_retirement_type final {
    http3_server_connection& owner_;
    request_entry_type& entry_;

    ~entry_retirement_type() {
        if (!entry_.output_terminal_) {
            owner_.output_.remove(entry_.slot_);
            entry_.output_enabled_ = false;
            entry_.cancel_requested_ = true;
            entry_.retirement_granted_ = true;
            entry_.output_terminal_ = true;
            entry_.slot_.status_ = request_status_type::failed;
            entry_.dispatch_.cancel();
            entry_.publication_finished_.notify();
            owner_.close_connection(http3_connection_error_code::internal_error);
        }
        owner_.finish_entry(entry_);
    }
};

struct http3_server_connection::request_retirement_type final {
    http3_server_connection& owner_;
    std::uint64_t stream_id_;

    ~request_retirement_type() {
        if (owner_.session_.request(stream_id_) == nullptr) {
            return;
        }
        const auto* slot = owner_.requests_.find(stream_id_);
        if (slot != nullptr && slot->status_ == request_status_type::published) {
            // The response is terminal, not the peer's receive direction. Its
            // remaining storage belongs to the input owner until FIN or reset.
            return;
        }
        (void)owner_.session_.release(stream_id_);
        owner_.close_connection(http3_connection_error_code::internal_error);
    }
};

http3_server_connection::http3_server_connection(const route_table& routes_value, worker_memory& worker_value,
    context_services services, const http_server_options& options, http3_connection_driver& transport,
    http3_body_budget& body_budget, http3_server_connection_config config)
    : services_(services),
      worker_(worker_value),
      routes_(routes_value),
      options_(options),
      admission_policy_(http_request_admission_policy::server(options)),
      transport_(transport),
      max_tracked_streams_(config.max_tracked_streams_),
      connection_scanner_(config.connection_scanner_),
      executor_(config.executor_),
      session_(routes_, worker_, body_budget, config.session_),
      input_(session_, worker_, max_tracked_streams_),
      tasks_(services_.worker(), {.resource_ = worker_.resource()}),
      requests_(worker_.resource(), max_tracked_streams_) {
    session_.bind_control_output_wake(this, [](void* raw) noexcept {
        static_cast<http3_server_connection*>(raw)->notify_activation();
    });
    session_.observe_push_cancellation(this, [](void* raw, std::uint64_t push_id) noexcept {
        static_cast<http3_server_connection*>(raw)->observe_push_cancellation(push_id);
    });
}

http3_server_connection::~http3_server_connection() {
    if (output_.ready() != 0 || output_.blocked() != 0 || requests_.active() != 0 ||
        requests_.rejections() != 0 || tasks_.size() != 0 || (ever_spawned_ && !join_completed_)) {
        std::terminate();
    }
}

void http3_server_connection::accept_data(
    std::uint64_t stream_id, bool received_early_data, std::span<const std::byte> bytes_value) noexcept {
    if (admission_closed_) {
        return;
    }
    try {
        handle_input_result(stream_id, input_.accept_data(stream_id, received_early_data, bytes_value));
    } catch (...) {
        close_connection(http3_connection_error_code::internal_error);
    }
    notify_activation();
}

void http3_server_connection::accept_peer_stream_data(
    std::uint64_t stream_id, std::span<const std::byte> bytes_value) noexcept {
    if (admission_closed_) {
        return;
    }
    try {
        handle_input_result(stream_id, input_.accept_peer_stream_data(stream_id, bytes_value));
    } catch (...) {
        close_connection(http3_connection_error_code::internal_error);
    }
    notify_activation();
}

void http3_server_connection::accept_fin(std::uint64_t stream_id) noexcept {
    if (admission_closed_) {
        return;
    }
    try {
        handle_input_result(stream_id, input_.accept_fin(stream_id));
    } catch (...) {
        close_connection(http3_connection_error_code::internal_error);
    }
    notify_activation();
}

void http3_server_connection::accept_reset(std::uint64_t stream_id, http3_connection_error_code) noexcept {
    // The code names why the transport ended the stream; the handler outcome
    // is the same cancellation for every cause.
    if (auto* slot = requests_.find(stream_id); slot != nullptr && slot->push_id_) {
        cancel_push(*slot);
        notify_activation();
        return;
    }
    if (admission_closed_) {
        return;
    }
    const auto result_value = input_.accept_reset(stream_id);
    try {
        if (result_value.status_ != input_type::status_type::reset &&
            result_value.status_ != input_type::status_type::closed_stream) {
            handle_input_result(stream_id, result_value);
        }
    } catch (...) {
        close_connection(http3_connection_error_code::internal_error);
    }
    // Whatever the receive state was, the transport stream is terminal.
    if (auto* slot = requests_.find(stream_id); slot != nullptr) {
        cancel_stream(*slot);
    }
    notify_activation();
}

std::size_t http3_server_connection::input_capacity(std::uint64_t stream_id) const noexcept {
    if (admission_closed_) {
        // Late input is discarded without buffering.
        return std::numeric_limits<std::size_t>::max();
    }
    if (input_.qpack_blocked(stream_id)) {
        return 0;
    }
    return session_.input_capacity(stream_id);
}

bool http3_server_connection::resume_qpack_input() noexcept {
    if (admission_closed_) {
        return false;
    }
    bool progress_value = false;
    try {
        for (unsigned i = 0; i < 32 && !stop_requested_; ++i) {
            const auto resumed = input_.resume_qpack();
            if (!resumed) {
                break;
            }
            progress_value = true;
            handle_input_result(resumed->stream_id_, resumed->result_);
        }
    } catch (...) {
        close_connection(http3_connection_error_code::internal_error);
    }
    if (progress_value) {
        notify_activation();
    }
    return progress_value;
}

bool http3_server_connection::request_stop() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (stop_requested_) {
        return false;
    }
    stop_requested_ = true;
    if (!admission_closed_) {
        stop_entries();
    }
    return true;
}

void http3_server_connection::transport_retired() noexcept {
    transport_retired_ = true;
    stop_requested_ = true;
    if (!admission_closed_) {
        stop_entries();
    }
}

task<void> http3_server_connection::join() {
    if (!on_worker()) {
        throw std::logic_error("HTTP/3 connection join must run on its worker");
    }
    if (!admission_closed_) {
        throw std::logic_error("HTTP/3 connection must stop before joining");
    }
    if (join_started_) {
        throw std::logic_error("HTTP/3 connection can only be joined once");
    }
    join_started_ = true;
    try {
        co_await tasks_.join();
    } catch (...) {
        join_completed_ = true;
        throw;
    }
    join_completed_ = true;
    if (requests_.active() != 0 || requests_.rejections() != 0 || output_.ready() != 0 || output_.blocked() != 0 ||
        session_.active_stream_count() != 0) {
        std::terminate();
    }
}

bool http3_server_connection::drain_ready(std::size_t expected_admitted_requests) const noexcept {
    if (!on_worker() || admission_closed_ || stop_requested_ || input_.stopped() ||
        input_.observed_request_stream_count() != expected_admitted_requests ||
        input_.active_request_stream_count() != 0 || requests_.rejections() != 0 || output_.ready() != 0 ||
        output_.blocked() != 0 || session_.active_stream_count() != 0) {
        return false;
    }
    for (const auto& slot : requests_.slots()) {
        if ((slot.entry_ != nullptr && (!slot.entry_->output_terminal_ || slot.entry_->dispatch_.handler_active())) ||
            slot.rejection_entry_ != nullptr) {
            return false;
        }
    }
    return true;
}

task<void> http3_server_connection::run_request(std::uint64_t stream_id) {
    auto* slot = requests_.find(stream_id);
    if (slot != nullptr && slot->push_id_ && (slot->push_cancelled_ || admission_closed_)) {
        slot->push_prefix_.reset();
        (void)session_.cancel_request(stream_id);
        slot->status_ = request_status_type::cancelled;
        reset_stream(*slot);
        co_return;
    }
    if (slot == nullptr || slot->status_ != request_status_type::admitting || slot->entry_ != nullptr) {
        if (slot != nullptr) {
            slot->status_ = request_status_type::failed;
        }
        close_connection(http3_connection_error_code::internal_error);
        co_return;
    }

    request_retirement_type request_retirement{*this, stream_id};
    std::optional<request_entry_type> request;
    try {
        request.emplace(*this, *slot, stream_id);
    } catch (...) {
        slot->status_ = request_status_type::failed;
        close_connection(http3_connection_error_code::internal_error);
        co_return;
    }
    requests_.attach(*slot, *request);
    slot->status_ = request_status_type::running;
    entry_retirement_type retirement{*this, *request};
    auto& entry_value = *request;

    using run_status_type = dispatch_type::run_status_type;
    using cancellation_reason_type = dispatch_type::cancellation_reason_type;
    run_status_type run_status = run_status_type::failed;
    try {
        if (slot->push_prefix_) {
            const auto prefix = *slot->push_prefix_;
            slot->push_prefix_.reset();
            co_await entry_value.dispatch_.publish_response_bytes(prefix.bytes());
        }
        run_status = co_await entry_value.dispatch_.run_handler();
    } catch (...) {
        if (entry_value.cancel_requested_ || admission_closed_) {
            run_status = run_status_type::cancelled;
        } else {
            output_.remove(entry_value.slot_);
            entry_value.dispatch_.cancel();
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            close_connection(http3_connection_error_code::internal_error);
            co_return;
        }
    }

    if (run_status == run_status_type::cancelled && !entry_value.cancel_requested_ && !admission_closed_) {
        switch (entry_value.dispatch_.cancellation_reason()) {
            case cancellation_reason_type::worker_stop:
                (void)request_stop();
                break;
            case cancellation_reason_type::deadline:
                cancel_deadline_entry(entry_value);
                break;
            case cancellation_reason_type::explicit_value:
                cancel_entry(entry_value, request_status_type::cancelled);
                break;
            case cancellation_reason_type::none:
                entry_value.output_terminal_ = true;
                entry_value.slot_.status_ = request_status_type::failed;
                close_connection(http3_connection_error_code::internal_error);
                break;
        }
    }

    if (run_status == run_status_type::peer_field_section_limit) {
        output_.remove(entry_value.slot_);
        reset_stream(entry_value.slot_);
        entry_value.output_terminal_ = true;
        entry_value.slot_.status_ = request_status_type::failed;
        notify_activation();
        co_return;
    }

    if (run_status == run_status_type::tunnel_complete || run_status == run_status_type::output_complete) {
        // Every response byte and the FIN were accepted while the handler ran.
        if (!entry_value.output_terminal_ || entry_value.slot_.status_ != request_status_type::published) {
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            close_connection(http3_connection_error_code::internal_error);
        }
        co_return;
    }

    if (run_status == run_status_type::response_ready && entry_value.output_enabled_ &&
        !entry_value.cancel_requested_ && !admission_closed_) {
        entry_value.slot_.status_ = request_status_type::ready_to_publish;
        try {
            (void)entry_value.dispatch_.register_publication_deadline_callback(
                [this, entry_pointer = &entry_value]() noexcept {
                    publication_stopped(*entry_pointer);
                });
            enqueue_for_demand(entry_value.slot_, true);
        } catch (...) {
            output_.remove(entry_value.slot_);
            entry_value.dispatch_.cancel();
            entry_value.output_enabled_ = false;
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            entry_value.publication_finished_.notify();
            close_connection(http3_connection_error_code::internal_error);
            co_return;
        }
        while (!entry_value.output_terminal_ && !entry_value.retirement_granted_) {
            co_await entry_value.publication_finished_.wait();
        }
        co_return;
    }

    if (run_status == run_status_type::cancelled || entry_value.cancel_requested_ || admission_closed_ ||
        !entry_value.output_enabled_) {
        while (!entry_value.retirement_granted_) {
            co_await entry_value.publication_finished_.wait();
        }
        co_return;
    }

    entry_value.output_terminal_ = true;
    entry_value.slot_.status_ = request_status_type::failed;
    close_connection(http3_connection_error_code::internal_error);
}

void http3_server_connection::retire_request_input(std::uint64_t stream_id) noexcept {
    const auto* slot = requests_.find(stream_id);
    if (slot != nullptr && slot->push_id_) {
        (void)session_.cancel_request(stream_id);
    } else {
        (void)input_.cancel_request(stream_id);
    }
}

void http3_server_connection::handle_input_result(std::uint64_t stream_id, input_type::result_type result_value) {
    using status_type = input_type::status_type;
    process_push_cancellations();

    if (auto* slot = requests_.find(stream_id); slot != nullptr && slot->entry_ != nullptr) {
        slot->entry_->dispatch_.notify_tunnel_input();
    }

    if (result_value.status_ == status_type::protocol_error) {
        if (connection_failure(result_value) || input_.stopped()) {
            close_connection(result_value.protocol_.code_);
            return;
        }
        bool created = false;
        auto* slot = requests_.find_or_create(stream_id, created);
        if (slot == nullptr) {
            close_connection(http3_connection_error_code::excessive_load);
            return;
        }
        if (slot->interim_response_) {
            output_.remove(*slot);
            slot->interim_response_.reset();
        }
        if (slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::protocol_error);
        } else if (slot->rejection_entry_ != nullptr) {
            retire_rejected_receive(*slot->rejection_entry_, request_status_type::protocol_error);
            slot->status_ = request_status_type::protocol_error;
        } else {
            slot->status_ = request_status_type::protocol_error;
        }
        reset_stream(*slot, result_value.protocol_.code_);
        return;
    }

    if (session_.tunnel_input_overflowed(stream_id)) {
        bool created = false;
        auto* slot = requests_.find_or_create(stream_id, created);
        if (slot == nullptr) {
            close_connection(http3_connection_error_code::excessive_load);
            return;
        }
        if (slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::cancelled);
        } else {
            slot->status_ = request_status_type::cancelled;
        }
        reset_stream(*slot);
        retire_request_input(stream_id);
        return;
    }

    switch (result_value.status_) {
        case status_type::capacity_exhausted:
        case status_type::final_size_error: {
            const bool has_protocol_error =
                result_value.protocol_.code_ != http3_connection_error_code::no_error ||
                result_value.protocol_.scope_ != http3_connection_error_scope::none ||
                result_value.protocol_.status_ == http3_connection_status::stream_error ||
                result_value.protocol_.status_ == http3_connection_status::connection_error;
            close_connection(has_protocol_error ? result_value.protocol_.code_
                             : result_value.status_ == status_type::capacity_exhausted
                                 ? http3_connection_error_code::excessive_load
                                 : http3_connection_error_code::internal_error);
            return;
        }
        case status_type::stopped:
            close_connection(http3_connection_error_code::internal_error);
            return;
        case status_type::fed:
        case status_type::finished:
            break;
        case status_type::deferred_qpack:
        case status_type::reset:
        case status_type::local_cancelled:
        case status_type::closed_stream:
        case status_type::duplicate_fin:
        case status_type::invalid_input:
            return;
        case status_type::protocol_error:
            return;
    }

    const bool finished = result_value.status_ == status_type::finished;
    if (session_.request(stream_id) != nullptr) {
        const auto rejection = session_.rejection(stream_id);
        if (rejection != session_type::rejection_type::none) {
            start_rejection(stream_id, rejection, finished);
            return;
        }
        if (!finished && !queue_continue_response(stream_id)) {
            return;
        }
    }

    if (const auto* slot = requests_.find(stream_id);
        slot != nullptr && slot->request_started_ && (slot->entry_ == nullptr || slot->entry_->dispatch_.complete())) {
        // A response can finish before peer FIN. Its dispatch has returned the
        // lease; only now may the input owner release receive storage.
        if (finished) {
            (void)session_.release(stream_id);
        }
        return;
    }

    if (session_.stream_state(stream_id) == session_type::stream_state_type::ready) {
        const auto* request = session_.request(stream_id);
        // Both ordinary and extended CONNECT wait for a successful response
        // before sending tunnel data or FIN. Admit their ready heads now.
        if (request != nullptr &&
            (request->request().known_method() == http_known_method::connect || session_.streaming_request(stream_id))) {
            (void)admit_finished_request(stream_id);
            return;
        }
    }

    // The protocol core consumes peer unidirectional streams first so critical
    // stream termination and stream-type errors remain visible. A successfully
    // ignored unknown peer uni stream is not a Web request.
    if (finished && !is_http3_client_unidirectional_stream_id(stream_id)) {
        (void)admit_finished_request(stream_id);
    }
}

bool http3_server_connection::queue_continue_response(std::uint64_t stream_id) {
    const auto* request = session_.request(stream_id);
    if (request == nullptr) {
        return true;
    }
    const auto expectation = request->expectation_plan(http_unsupported_expectation_policy::reject);
    if (expectation.send_continue() == nullptr) {
        return true;
    }
    bool created = false;
    auto* slot = requests_.find_or_create(stream_id, created);
    if (slot == nullptr) {
        close_connection(http3_connection_error_code::excessive_load);
        return false;
    }
    if (slot->interim_sent_) {
        return true;
    }
    auto head = session_.encode_interim_response_head(stream_id, http_interim_response_head(http_status::continue_value));
    const auto peer_limit = session_.peer_max_field_section_size();
    if (head.index() != 0 ||
        (peer_limit && std::get<0>(head).headers_.decoded_field_section_size() > *peer_limit)) {
        slot->status_ = request_status_type::cancelled;
        reset_stream(*slot);
        retire_request_input(stream_id);
        return false;
    }
    slot->interim_sent_ = true;
    slot->interim_response_.emplace(pending_interim_response_type{std::move(std::get<0>(head).headers_)});
    enqueue_for_demand(*slot, true);
    return true;
}

void http3_server_connection::start_rejection(
    std::uint64_t stream_id, session_type::rejection_type rejection, bool receive_finished) {
    const auto* request = session_.request(stream_id);
    if (request == nullptr) {
        close_connection(http3_connection_error_code::internal_error);
        return;
    }
    bool created = false;
    auto* slot = requests_.find_or_create(stream_id, created);
    if (slot == nullptr) {
        close_connection(http3_connection_error_code::excessive_load);
        return;
    }

    auto* entry_value = slot->rejection_entry_;
    bool rejection_created = false;
    if (entry_value == nullptr) {
        if (slot->entry_ != nullptr || (!created && slot->status_ != request_status_type::unknown)) {
            slot->status_ = request_status_type::failed;
            close_connection(http3_connection_error_code::internal_error);
            return;
        }
        const auto status = rejection_status(rejection);
        std::pmr::polymorphic_allocator<rejection_entry_type> allocator(worker_.resource());
        try {
            entry_value = allocator.allocate(1);
            try {
                std::construct_at(entry_value, *this, *slot, stream_id, request->request().known_method(), status);
            } catch (...) {
                allocator.deallocate(entry_value, 1);
                entry_value = nullptr;
                throw;
            }
        } catch (...) {
            slot->status_ = request_status_type::failed;
            close_connection(http3_connection_error_code::internal_error);
            return;
        }
        requests_.attach_rejection(*slot, *entry_value);
        slot->status_ = request_status_type::rejected;
        slot->rejection_ = rejection;
        rejection_created = true;
        if (options_.access_log_.callback_) {
            // The status of a protocol rejection is final once chosen.
            const auto& request_value = request->request();
            http_response_tail(request_value, http_response_coding_policy::disabled(),
                services_.resolve_conn_info(request_value).client().address(), std::chrono::steady_clock::now())
                .record_access(options_.access_log_, status);
        }
    } else if (slot->entry_ != nullptr || slot->rejection_ != rejection) {
        slot->status_ = request_status_type::failed;
        close_connection(http3_connection_error_code::internal_error);
        return;
    }

    if (receive_finished && !entry_value->receive_terminal_) {
        if (!session_.release(stream_id)) {
            close_connection(http3_connection_error_code::internal_error);
            return;
        }
        entry_value->receive_terminal_ = true;
        if (entry_value->output_terminal_) {
            finish_rejection(*entry_value);
            return;
        }
    }
    if (rejection_created) {
        enqueue_for_demand(*slot, true);
    }
}

void http3_server_connection::retire_rejected_receive(rejection_entry_type& entry_value, request_status_type status) noexcept {
    entry_value.receive_terminal_ = true;
    if (!entry_value.output_terminal_) {
        cancel_rejection_output(entry_value, status);
    } else {
        finish_rejection(entry_value);
    }
}

void http3_server_connection::cancel_stream(request_index_slot_type& slot) noexcept {
    if (slot.interim_response_) {
        output_.remove(slot);
        slot.interim_response_.reset();
        slot.status_ = request_status_type::cancelled;
    }
    if (slot.entry_ != nullptr) {
        cancel_entry(*slot.entry_, request_status_type::cancelled);
    } else if (slot.rejection_entry_ != nullptr) {
        retire_rejected_receive(*slot.rejection_entry_, request_status_type::cancelled);
    }
}

void http3_server_connection::cancel_rejection_output(rejection_entry_type& entry_value, request_status_type status) noexcept {
    if (entry_value.output_terminal_) {
        return;
    }
    output_.remove(entry_value.slot_);
    entry_value.output_enabled_ = false;
    if (entry_value.output_) {
        entry_value.output_->stop();
        entry_value.output_.reset();
    }
    entry_value.response_.reset();
    entry_value.output_terminal_ = true;
    entry_value.slot_.status_ = status;
    finish_rejection(entry_value);
}

void http3_server_connection::finish_rejection(rejection_entry_type& entry_value) noexcept {
    if (!entry_value.output_terminal_ || !entry_value.receive_terminal_) {
        return;
    }
    auto& slot = entry_value.slot_;
    if (slot.queue_ != queue_kind_type::none || slot.rejection_entry_ != &entry_value || requests_.rejections() == 0) {
        std::terminate();
    }
    requests_.retire_rejection(slot, entry_value);
    std::pmr::polymorphic_allocator<rejection_entry_type> allocator(worker_.resource());
    std::destroy_at(&entry_value);
    allocator.deallocate(&entry_value, 1);
}

bool http3_server_connection::admit_finished_request(std::uint64_t stream_id) {
    bool created = false;
    auto* slot = requests_.find_or_create(stream_id, created);
    if (slot == nullptr) {
        close_connection(http3_connection_error_code::excessive_load);
        return false;
    }
    if (session_.stream_state(stream_id) != session_type::stream_state_type::ready) {
        slot->status_ = request_status_type::failed;
        close_connection(http3_connection_error_code::internal_error);
        return false;
    }
    if (slot->request_started_ || slot->status_ == request_status_type::cancelled ||
        slot->status_ == request_status_type::protocol_error) {
        return false;
    }
    slot->request_started_ = true;
    slot->status_ = request_status_type::admitting;
    try {
        tasks_.spawn(run_request(stream_id));
        ever_spawned_ = true;
    } catch (...) {
        slot->status_ = request_status_type::failed;
        close_connection(http3_connection_error_code::internal_error);
        return false;
    }
    return true;
}

void http3_server_connection::publication_stopped(request_entry_type& entry_value) noexcept {
    // stop_token callbacks run synchronously on their source thread. The owner
    // contract requires every stop source observed here to fire on this worker;
    // fail before inspecting or touching any intrusive queue otherwise.
    if (!on_worker()) {
        std::terminate();
    }
    if (entry_value.output_terminal_ || entry_value.retirement_granted_ || stop_requested_) {
        return;
    }
    if (entry_value.slot_.queue_ == queue_kind_type::local_runnable) {
        entry_value.deadline_activation_queued_ = true;
        return;
    }
    output_.remove(entry_value.slot_);
    enqueue_runnable(entry_value.slot_, queue_kind_type::local_runnable, false);
    if (!entry_value.deadline_activation_queued_ && entry_value.slot_.queue_ == queue_kind_type::local_runnable) {
        entry_value.deadline_activation_queued_ = true;
        notify_activation();
    }
}

void http3_server_connection::cancel_deadline_entry(request_entry_type& entry_value) noexcept {
    if (entry_value.retirement_granted_) {
        return;
    }
    // Fence output, tombstone input, then reset the stream: an accepted FIN
    // only closed the send direction, and the peer may never finish its own.
    entry_value.output_enabled_ = false;
    entry_value.cancel_requested_ = true;
    output_.remove(entry_value.slot_);
    retire_request_input(entry_value.stream_id_);
    reset_stream(entry_value.slot_);
    cancel_entry(entry_value, request_status_type::cancelled);
}

void http3_server_connection::cancel_entry(request_entry_type& entry_value, request_status_type status) noexcept {
    if (entry_value.retirement_granted_) {
        return;
    }
    entry_value.output_enabled_ = false;
    entry_value.cancel_requested_ = true;
    output_.remove(entry_value.slot_);
    entry_value.slot_.interim_response_.reset();
    // The input RESET/tombstone or connection stop is established by the caller.
    // output_terminal_ only fences publication; it does not retire a still-running
    // request frame. A suspended handler retains its dispatch lease until
    // run_handler() returns; only then may the wrapper consume this permit.
    entry_value.dispatch_.cancel();
    entry_value.output_terminal_ = true;
    entry_value.retirement_granted_ = true;
    entry_value.slot_.status_ = status;
    entry_value.publication_finished_.notify();
    notify_activation();
}

void http3_server_connection::stop_entries() noexcept {
    admission_closed_ = true;

    // First quiesce every output so cancellation of one handler cannot let a
    // sibling publish while the connection is being retired.
    for (auto& slot : requests_.slots()) {
        if (slot.interim_response_) {
            output_.remove(slot);
            slot.interim_response_.reset();
        }
        if (auto* rejection = slot.rejection_entry_; rejection != nullptr && !rejection->output_terminal_) {
            rejection->output_enabled_ = false;
            output_.remove(slot);
        }
        auto* entry_value = slot.entry_;
        if (entry_value == nullptr || entry_value->retirement_granted_) {
            continue;
        }
        entry_value->output_enabled_ = false;
        entry_value->cancel_requested_ = true;
        output_.remove(slot);
    }
    input_.stop();

    for (auto& slot : requests_.slots()) {
        if (auto* rejection = slot.rejection_entry_; rejection != nullptr) {
            if (!rejection->output_terminal_) {
                rejection->output_->stop();
                rejection->output_.reset();
                rejection->response_.reset();
                rejection->output_terminal_ = true;
                slot.status_ = request_status_type::cancelled;
            }
            rejection->receive_terminal_ = true;
            finish_rejection(*rejection);
        }
        auto* entry_value = slot.entry_;
        if (entry_value == nullptr || entry_value->retirement_granted_) {
            continue;
        }
        entry_value->dispatch_.cancel();
        entry_value->output_terminal_ = true;
        entry_value->retirement_granted_ = true;
        entry_value->slot_.status_ = request_status_type::cancelled;
        entry_value->publication_finished_.notify();
    }
    notify_activation();
}

void http3_server_connection::finish_entry(request_entry_type& entry_value) noexcept {
    if (entry_value.slot_.queue_ != queue_kind_type::none || entry_value.dispatch_.handler_active() ||
        !entry_value.output_terminal_ || entry_value.slot_.entry_ != &entry_value || requests_.active() == 0) {
        std::terminate();
    }
    requests_.retire(entry_value.slot_, entry_value);
}

void http3_server_connection::reset_stream(request_index_slot_type& slot, http3_connection_error_code code) noexcept {
    if (!transport_retired_ && !close_requested_) {
        transport_.reset_stream(slot.stream_id_, code);
    }
}

void http3_server_connection::close_connection(http3_connection_error_code code) noexcept {
    if (!close_requested_ && !transport_retired_) {
        close_requested_ = true;
        transport_.close_connection(code);
    }
    stop_requested_ = true;
    if (!admission_closed_) {
        stop_entries();
    }
}

void http3_server_connection::notify_activation() noexcept {
    transport_.wake();
}

http3_server_stream_output& http3_server_connection::output() noexcept {
    return transport_.output();
}

bool http3_server_connection::on_worker() const noexcept {
    return services_.worker().is_current();
}

}  // namespace ruvia::detail
