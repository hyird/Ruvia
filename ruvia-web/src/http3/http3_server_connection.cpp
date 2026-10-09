#include "http3/http3_server_connection.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http_request_target.h"

#include "router/route_table.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

[[nodiscard]]

[[nodiscard]] http_status_code
rejection_status(
    http3_sans_io_session_engine::rejection_type rejection) {
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

}  // namespace

struct http3_server_connection::pending_push_type final {
    pending_push_type(http3_server_connection& owner_value, transport_intent_token_type token_value)
        : owner_(owner_value),
          token_(token_value),
          ready_(owner_.services_.worker()),
          previous_(owner_.pending_push_tail_),
          intent_previous_(owner_.push_intent_tail_) {
        if (previous_) {
            previous_->next_ = this;
        } else {
            owner_.pending_push_head_ = this;
        }
        owner_.pending_push_tail_ = this;
        if (intent_previous_) {
            intent_previous_->intent_next_ = this;
        } else {
            owner_.push_intent_head_ = this;
        }
        owner_.push_intent_tail_ = this;
        ++owner_.pending_push_count_;
        ++owner_.pending_push_intent_count_;
    }
    ~pending_push_type() {
        if (owed_ || owner_.pending_push_count_ == 0) {
            std::terminate();
        }
        if (previous_) {
            previous_->next_ = next_;
        } else {
            owner_.pending_push_head_ = next_;
        }
        if (next_) {
            next_->previous_ = previous_;
        } else {
            owner_.pending_push_tail_ = previous_;
        }
        --owner_.pending_push_count_;
    }
    void settle(push_stream_open_result_type result_value) noexcept {
        if (!owed_ || owner_.pending_push_intent_count_ == 0) {
            std::terminate();
        }
        opened_ = result_value;
        owed_ = false;
        if (intent_previous_) {
            intent_previous_->intent_next_ = intent_next_;
        } else {
            owner_.push_intent_head_ = intent_next_;
        }
        if (intent_next_) {
            intent_next_->intent_previous_ = intent_previous_;
        } else {
            owner_.push_intent_tail_ = intent_previous_;
        }
        intent_previous_ = nullptr;
        intent_next_ = nullptr;
        --owner_.pending_push_intent_count_;
        ready_.notify();
    }
    http3_server_connection& owner_;
    const transport_intent_token_type token_;
    worker_signal ready_;
    pending_push_type* previous_{};
    pending_push_type* next_{};
    pending_push_type* intent_previous_{};
    pending_push_type* intent_next_{};
    std::optional<push_stream_open_result_type> opened_{};
    bool owed_{true};
    bool cancelled_{};
};

struct http3_server_connection::request_entry_type final {
    request_entry_type(http3_server_connection& owner_value, request_index_slot_type& slot_value,
        std::uint64_t stream_id_value)
        : owner_scanner_(owner_value.connection_scanner_),
          slot_(slot_value),
          stream_id_(stream_id_value),
          dispatch_(owner_value.session_, owner_value.routes_, owner_value.worker_,
              owner_value.services_, owner_value.options_, owner_value.outbound_,
              {owner_value.epoch_, owner_value.connection_generation_, stream_id_, slot_value.push_id_,
                  owner_value.input_.received_early_data(stream_id_)},

              scanner_entry_, owner_value.executor_,
              {.context_ = &owner_value,
                  .attach_scanner_ = &http3_server_connection::attach_tunnel_scanner_thunk,
                  .output_ready_ = &http3_server_connection::tunnel_output_ready_thunk,
                  .abort_ = &http3_server_connection::abort_tunnel_thunk,
                  .input_consumed_ = &http3_server_connection::request_input_consumed_thunk,
                  .push_ = [](void* raw, std::uint64_t parent_value, http_push_request_view request) -> task<bool> {
                      co_return co_await static_cast<http3_server_connection*>(raw)->push_request(parent_value, request);
                  },
                  .send_datagram_ = [](void* raw, std::uint64_t stream_id, std::span<const std::byte> bytes_value) {
                      auto& connection_value=*static_cast<http3_server_connection*>(raw);
                      if(!connection_value.datagram_output_.send_ || connection_value.stop_requested_){ throw std::runtime_error("HTTP Datagram output is unavailable");
}
                      connection_value.datagram_output_.send_(connection_value.datagram_output_.context_,stream_id,bytes_value); }},
              slot_value.response_prelude_bytes_),
          publication_finished_(owner_value.services_.worker()) {}

    ~request_entry_type() {
        if (scanner_registered_) {
            owner_scanner_->unregister_entry(scanner_entry_);
        }
    }

    connection_scanner::entry_type scanner_entry_;
    connection_scanner* owner_scanner_{};

    request_index_slot_type& slot_;
    const std::uint64_t stream_id_;
    dispatch_type dispatch_;
    worker_signal publication_finished_;
    bool scanner_registered_{};
    bool output_enabled_{true};
    // Publication can terminate while the request frame still awaits peer FIN.
    bool output_terminal_{};
    // Independent permission for run_request to retire after cancellation.
    bool retirement_granted_{};
    bool cancel_requested_{};
    bool deadline_activation_queued_{};
};

struct http3_server_connection::rejection_entry_type final {
    rejection_entry_type(http3_server_connection& owner_value, request_index_slot_type& slot_value,
        std::uint64_t stream_id_value, http_known_method method, session::rejection_type rejection)
        : slot_(slot_value),
          stream_id_(stream_id_value),
          response_(std::in_place, http_response::options_type{.resource_ = owner_value.worker_.resource()}) {
        response_->status(rejection_status(rejection));
        response_->header("content-length", "0");
        const auto write_plan = plan_buffered_http_response_write(method, *response_);
        auto created = http3_buffered_response_output::create(*response_, write_plan,
            owner_value.worker_, owner_value.outbound_,
            {owner_value.epoch_, owner_value.connection_generation_, stream_id_}, std::nullopt, slot_value.response_prelude_bytes_);
        if ((created.index() != 0)) {
            throw std::runtime_error("HTTP/3 rejection response preparation failed");
        }
        output_.emplace(std::move(std::get<0>(created)));
    }

    request_index_slot_type& slot_;
    const std::uint64_t stream_id_;
    std::optional<http_response> response_;
    std::optional<http3_buffered_response_output> output_;
    bool output_enabled_{true};
    bool output_terminal_{};
    bool receive_terminal_{};
};

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
            owner_.require_connection_close(transport_close_reason_type::entry_retirement_failure,
                http3_connection_error_code::internal_error);
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
        owner_.require_connection_close(transport_close_reason_type::request_retirement_failure,
            http3_connection_error_code::internal_error);
    }
};

http3_server_connection::http3_server_connection(
    const route_table& routes_value, worker_memory& worker_value, context_services services,
    const http_server_options& options, http3_stream_buffer& outbound,
    activation_ref_type activation, http3_server_connection_config config)
    : services_(services.with_routes(routes_value)),
      worker_(worker_value),
      routes_(routes_value),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch_),
      connection_generation_(config.connection_generation_),
      max_tracked_streams_(config.max_tracked_streams_),
      connection_scanner_(config.connection_scanner_),
      executor_(config.executor_),
      session_(routes_, worker_, config.session_),
      input_(session_, worker_, epoch_, connection_generation_, max_tracked_streams_),
      tasks_(services_.worker(), {.resource_ = worker_.resource()}),
      requests_(worker_.resource(), max_tracked_streams_),
      retirement_(requests_, max_tracked_streams_, epoch_, connection_generation_) {
    datagram_output_ = config.datagram_output_;
    initialize();
}

http3_server_connection::http3_server_connection(
    const route_table& routes_value, worker_memory& worker_value, context_services services,
    const http_server_options& options, http3_stream_buffer& outbound,
    activation_ref_type activation, http3_server_body_budget& body_budget,
    http3_server_connection_config config)
    : services_(services.with_routes(routes_value)),
      worker_(worker_value),
      routes_(routes_value),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch_),
      connection_generation_(config.connection_generation_),
      max_tracked_streams_(config.max_tracked_streams_),
      connection_scanner_(config.connection_scanner_),
      executor_(config.executor_),
      session_(routes_, worker_, body_budget, config.session_),
      input_(session_, worker_, epoch_, connection_generation_, max_tracked_streams_),
      tasks_(services_.worker(), {.resource_ = worker_.resource()}),
      requests_(worker_.resource(), max_tracked_streams_),
      retirement_(requests_, max_tracked_streams_, epoch_, connection_generation_) {
    datagram_output_ = config.datagram_output_;
    initialize();
}

void http3_server_connection::receive_datagram(std::span<const std::byte> bytes_value) noexcept {
    if (!on_worker() || stop_requested_) {
        return;
    }
    const auto code = static_cast<http3_connection_error_code>(http3_datagram_error_code);
    auto decoded = decode_http3_datagram(std::span(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()));
    if ((decoded.index() != 0)) {
        require_connection_close(transport_close_reason_type::connection_protocol_error, code);
        return;
    }
    try {
        const auto status = session_.receive_datagram(std::get<0>(decoded));
        auto* slot = requests_.find(std::get<0>(decoded).stream_id_);
        if (status == http3_datagram_receive_status::connection_error) {
            require_connection_close(transport_close_reason_type::connection_protocol_error, code);
        } else if (status == http3_datagram_receive_status::stream_error && slot) {
            (void)enqueue_reset_intent(*slot, code);
            (void)retire_request_input(slot->stream_id_);
            if (slot->entry_) {
                cancel_entry(*slot->entry_, request_status_type::cancelled);
            }
        } else if (status == http3_datagram_receive_status::deliver && slot && slot->entry_) {
            slot->entry_->dispatch_.notify_tunnel_input();
        }
    } catch (...) {
        require_connection_close(transport_close_reason_type::input_capacity_exhausted);
    }
}
void http3_server_connection::initialize() {
    session_.bind_control_output_wake(this, [](void* raw) noexcept {
        static_cast<http3_server_connection*>(raw)->notify_activation();
    });
    if (!activation_.valid()) {
        throw std::invalid_argument("HTTP/3 connection requires a typed worker activation");
    }
    session_.observe_push_cancellation(this, [](void* raw, std::uint64_t push_id) noexcept {
        static_cast<http3_server_connection*>(raw)->observe_push_cancellation(push_id);
    });
}

http3_server_connection::~http3_server_connection() {
    if (std::as_const(output_).queue(queue_kind_type::data_runnable).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::data_runnable).tail_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::control_runnable).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::control_runnable).tail_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::local_runnable).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::local_runnable).tail_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::data_blocked).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::data_blocked).tail_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::control_blocked).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::control_blocked).tail_ != nullptr ||
        output_.ready() != 0 || output_.blocked() != 0 || requests_.active() != 0 ||
        requests_.rejections() != 0 || tasks_.size() != 0 || session_.active_stream_count() != 0 ||
        retirement_.pending() != 0 || pending_push_count_ != 0 || pending_push_intent_count_ != 0 ||
        pending_push_head_ != nullptr || pending_push_tail_ != nullptr || push_intent_head_ != nullptr || push_intent_tail_ != nullptr ||
        (!retirement_.retired() && !retirement_.taken_over() && !retirement_.handed_off()) ||
        (ever_spawned_ && !join_completed_)) {
        std::terminate();
    }
}

bool http3_server_connection::resume_qpack_input() noexcept {
    if (!on_worker() || admission_closed_) {
        return false;
    }
    bool progress_value = false;
    try {
        for (unsigned i = 0; i < 32; ++i) {
            const auto resumed = input_.resume_qpack();
            if (!resumed) {
                break;
            }
            progress_value = true;
            const auto result_value = handle_input_result(resumed->stream_id_, resumed->result_, false);
            if (result_value.connection_close_required_) {
                break;
            }
        }
    } catch (...) {
        require_connection_close(transport_close_reason_type::connection_protocol_error, http3_connection_error_code::internal_error);
    }
    if (progress_value) {
        notify_activation();
    }
    return progress_value;
}

bool http3_server_connection::can_accept_input(std::uint64_t stream_id, std::size_t wire_bytes) const noexcept {
    return admission_closed_ || (input_.can_accept_input(stream_id) && session_.can_accept_input(stream_id, wire_bytes));
}

http3_server_connection::event_result_type http3_server_connection::accept_data(
    const http3_stream_buffer::borrowed_block& block) & {
    if (!on_worker()) {
        return {.status_ = event_status_type::wrong_worker};
    }
    if (admission_closed_) {
        return {.status_ = event_status_type::admission_closed,
            .input_ = {input_type::status_type::stopped},
            .connection_close_required_ = retirement_.close_required()};
    }
    const auto stream_id = block.id().stream_id_;
    auto result_value = handle_input_result(stream_id, input_.accept_data(block), false);
    notify_activation();
    return result_value;
}

http3_server_connection::event_result_type http3_server_connection::accept_peer_stream_data(
    http3_stream_id id, std::span<const std::byte> bytes_value) & {
    if (!on_worker()) {
        return {.status_ = event_status_type::wrong_worker};
    }
    if (admission_closed_) {
        return {.status_ = event_status_type::admission_closed,
            .input_ = {input_type::status_type::stopped},
            .connection_close_required_ = retirement_.close_required()};
    }
    auto result_value = handle_input_result(id.stream_id_, input_.accept_peer_stream_data(id, bytes_value), false);
    notify_activation();
    return result_value;
}

http3_server_connection::event_result_type http3_server_connection::accept_control(
    const http3_stream_control& control) & {
    if (!on_worker()) {
        return {.status_ = event_status_type::wrong_worker};
    }
    if (control.id_.push_id_) {
        if (control.id_.epoch_ != epoch_ || control.id_.connection_generation_ != connection_generation_ ||
            control.kind_ != http3_stream_control::kind::stream_reset) {
            return {.status_ = event_status_type::input_rejected};
        }
        auto* slot = requests_.find(control.id_.stream_id_);
        if (slot == nullptr || slot->push_id_ != control.id_.push_id_) {
            return {.status_ = event_status_type::input_rejected};
        }
        slot->push_cancelled_ = true;
        slot->push_prefix_.reset();
        (void)session_.cancel_request(slot->stream_id_);
        if (slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::cancelled);
        }
        return {.status_ = event_status_type::stream_cancelled};
    }
    if (control.kind_ == http3_stream_control::kind::connection_closed &&
        control.id_.epoch_ == epoch_ &&
        control.id_.connection_generation_ == connection_generation_ && admission_closed_) {
        (void)confirm_transport_retired(
            {.epoch_ = epoch_, .connection_generation_ = connection_generation_});
        return {.status_ = event_status_type::connection_closed,
            .input_ = {input_type::status_type::connection_closed}};
    }
    if (admission_closed_) {
        return {.status_ = event_status_type::admission_closed,
            .input_ = {input_type::status_type::stopped},
            .connection_close_required_ = retirement_.close_required()};
    }

    // Suppress this stream's output before recording RESET in the input tombstone.
    // Identity mismatches must not affect a current request with the same stream ID.
    request_entry_type* reset_entry = nullptr;
    rejection_entry_type* reset_rejection = nullptr;
    if (control.kind_ == http3_stream_control::kind::stream_reset &&
        control.id_.epoch_ == epoch_ &&
        control.id_.connection_generation_ == connection_generation_) {
        auto* slot = requests_.find(control.id_.stream_id_);
        if (slot != nullptr && slot->entry_ != nullptr && !slot->entry_->cancel_requested_) {
            reset_entry = slot->entry_;
            reset_entry->output_enabled_ = false;
            reset_entry->cancel_requested_ = true;
            output_.remove(reset_entry->slot_);
        } else if (slot != nullptr && slot->rejection_entry_ != nullptr &&
                   !slot->rejection_entry_->output_terminal_) {
            reset_rejection = slot->rejection_entry_;
            reset_rejection->output_enabled_ = false;
            output_.remove(reset_rejection->slot_);
        }
    }

    auto result_value = input_.accept_control(control);
    if (reset_entry != nullptr && result_value.status_ != input_type::status_type::reset &&
        result_value.status_ != input_type::status_type::deferred_reset &&
        result_value.status_ != input_type::status_type::protocol_error && result_value.status_ != input_type::status_type::stopped &&
        result_value.status_ != input_type::status_type::connection_closed &&
        result_value.status_ != input_type::status_type::capacity_exhausted &&
        result_value.status_ != input_type::status_type::final_size_error) {
        // The input declined this control without establishing a terminal
        // tombstone. Restore a still-live response rather than silently dropping it.
        reset_entry->cancel_requested_ = false;
        reset_entry->output_enabled_ = true;
        if (reset_entry->dispatch_.response_ready()) {
            enqueue_for_demand(reset_entry->slot_, true);
        }
    } else if (reset_rejection != nullptr && result_value.status_ != input_type::status_type::reset &&
               result_value.status_ != input_type::status_type::deferred_reset &&
               result_value.status_ != input_type::status_type::protocol_error &&
               result_value.status_ != input_type::status_type::stopped &&
               result_value.status_ != input_type::status_type::connection_closed &&
               result_value.status_ != input_type::status_type::capacity_exhausted &&
               result_value.status_ != input_type::status_type::final_size_error) {
        reset_rejection->output_enabled_ = true;
        enqueue_for_demand(reset_rejection->slot_, true);
    }
    return handle_input_result(control.id_.stream_id_, result_value,
        control.kind_ == http3_stream_control::kind::stream_reset);
}

http3_server_connection::work_state_type
http3_server_connection::work_state() const noexcept {
    if (!on_worker()) {
        return {.wrong_worker_ = true};
    }
    const bool critical_pending = !admission_closed_ && (!session_.pending_qpack_encoder_output().empty() || !session_.pending_qpack_decoder_output().empty() || !session_.pending_control_output().empty());
    return {.runnable_ = {.data_ = std::as_const(output_).queue(queue_kind_type::data_runnable).head_ != nullptr || (critical_pending && !critical_output_blocked_),
                .control_ = std::as_const(output_).queue(queue_kind_type::control_runnable).head_ != nullptr,
                .local_ = std::as_const(output_).queue(queue_kind_type::local_runnable).head_ != nullptr},
        .blocked_ = {.data_ = std::as_const(output_).queue(queue_kind_type::data_blocked).head_ != nullptr || (critical_pending && critical_output_blocked_),
            .control_ = std::as_const(output_).queue(queue_kind_type::control_blocked).head_ != nullptr},
        .runnable_count_ = output_.ready() + (critical_pending && !critical_output_blocked_ ? 1 : 0),
        .blocked_count_ = output_.blocked() + (critical_pending && critical_output_blocked_ ? 1 : 0)};
}

std::size_t http3_server_connection::reactivate_blocked(work_lanes_type lanes) & noexcept {
    if (!on_worker()) {
        return 0;
    }
    std::size_t reactivated = 0;
    const auto reactivate = [this, &reactivated](const intrusive_queue_type& blocked, queue_kind_type runnable) {
        while (blocked.head_ != nullptr) {
            auto& slot = *blocked.head_;
            output_.remove(slot);
            enqueue_runnable(slot, runnable, false);
            ++reactivated;
        }
    };
    if (lanes.data_) {
        if (critical_output_blocked_) {
            critical_output_blocked_ = false;
            ++reactivated;
        }
        reactivate(std::as_const(output_).queue(queue_kind_type::data_blocked), queue_kind_type::data_runnable);
    }
    if (lanes.control_) {
        reactivate(std::as_const(output_).queue(queue_kind_type::control_blocked), queue_kind_type::control_runnable);
    }
    if (reactivated != 0) {
        notify_activation();
    }
    return reactivated;
}

bool http3_server_connection::reactivate_blocked_one(work_lane_type lane) & noexcept {
    if (!on_worker()) {
        return false;
    }
    const intrusive_queue_type* blocked = nullptr;
    queue_kind_type runnable = queue_kind_type::none;
    switch (lane) {
        case work_lane_type::data:
            if (critical_output_blocked_) {
                critical_output_blocked_ = false;
                notify_activation();
                return true;
            }
            blocked = &std::as_const(output_).queue(queue_kind_type::data_blocked);
            runnable = queue_kind_type::data_runnable;
            break;
        case work_lane_type::control:
            blocked = &std::as_const(output_).queue(queue_kind_type::control_blocked);
            runnable = queue_kind_type::control_runnable;
            break;
        case work_lane_type::local:
            return false;
    }
    if (blocked->head_ == nullptr) {
        return false;
    }
    auto& slot = *blocked->head_;
    output_.remove(slot);
    enqueue_runnable(slot, runnable, false);
    if (slot.queue_ != runnable) {
        return false;
    }
    return true;
}

http3_server_connection::publish_attempt_type
http3_server_connection::publish_one(work_lanes_type eligible_lanes) & noexcept {
    if (!on_worker()) {
        return {.status_ = publish_status_type::wrong_worker};
    }
    const auto encoder = session_.pending_qpack_encoder_output();
    const auto decoder = session_.pending_qpack_decoder_output();
    const auto control = session_.pending_control_output();
    if (eligible_lanes.data_ && !admission_closed_ && !critical_output_blocked_ &&
        (!encoder.empty() || !decoder.empty() || !control.empty()) && (prefer_critical_output_ || std::as_const(output_).queue(queue_kind_type::data_runnable).head_ == nullptr)) {
        prefer_critical_output_ = false;
        const std::array pending{encoder, decoder, control};
        auto index = next_critical_output_;
        for (std::size_t attempt_value = 0; pending[index].empty() && attempt_value < pending.size(); ++attempt_value) {
            index = (index + 1) % pending.size();
        }
        next_critical_output_ = (index + 1) % pending.size();
        const auto kind = index == 0 ? ruvia::http3_critical_stream_output::stream_kind::qpack_encoder : index == 1 ? ruvia::http3_critical_stream_output::stream_kind::qpack_decoder
                                                                                                                    : ruvia::http3_critical_stream_output::stream_kind::control;
        const auto bytes_value = pending[index].first(std::min(http3_stream_buffer::max_block_bytes, pending[index].size()));
        const auto sent = outbound_.try_send_critical({epoch_, connection_generation_, kind}, std::as_bytes(bytes_value));
        dispatch_type::publish_result_type result_value;
        switch (sent) {
            case http3_stream_buffer::send_result::sent:
                // The reliable buffer now owns a copy through QUIC acceptance.
                if (!(index == 0 ? session_.consume_qpack_encoder_output(bytes_value.size()) : index == 1 ? session_.consume_qpack_decoder_output(bytes_value.size())
                                                                                                          : session_.consume_control_output(bytes_value.size()))) {
                    std::terminate();
                }
                result_value = {.status_ = dispatch_type::publish_status_type::bytes_published, .bytes_published_ = bytes_value.size()};
                break;
            case http3_stream_buffer::send_result::full:
            case http3_stream_buffer::send_result::no_block:
                critical_output_blocked_ = true;
                result_value = {.status_ = dispatch_type::publish_status_type::backpressured, .block_reason_ = dispatch_type::publish_block_reason_type::data};
                break;
            default:
                require_connection_close(transport_close_reason_type::publish_failure, http3_connection_error_code::internal_error);
                result_value = {.status_ = dispatch_type::publish_status_type::failed};
                break;
        }
        notify_activation();
        return {.status_ = publish_status_type::attempted, .critical_kind_ = kind, .publication_ = result_value};
    }
    auto* slot = output_.select(eligible_lanes);
    if (slot == nullptr) {
        return {};
    }
    prefer_critical_output_ = true;
    if (slot->interim_response_) {
        return publish_interim_response(*slot);
    }
    auto* entry_value = slot->entry_;
    auto* rejection_entry = slot->rejection_entry_;
    if ((entry_value == nullptr) == (rejection_entry == nullptr)) {
        std::terminate();
    }

    output_.remove(*slot);
    output_.advance();
    if (rejection_entry != nullptr) {
        return publish_rejection(*slot, *rejection_entry);
    }
    entry_value->slot_.status_ = request_status_type::publishing;
    const auto result_value = entry_value->dispatch_.publish_step();
    switch (result_value.status_) {
        case dispatch_type::publish_status_type::bytes_published:
        case dispatch_type::publish_status_type::control_published:
            if (entry_value->output_enabled_ && !entry_value->cancel_requested_ && !stop_requested_) {
                entry_value->slot_.status_ = request_status_type::publishing;
                if (entry_value->dispatch_.publication_demand() !=
                    dispatch_type::publication_demand_type::not_ready) {
                    enqueue_for_demand(entry_value->slot_, false);
                }
            } else {
                cancel_entry(*entry_value, request_status_type::cancelled);
            }
            break;
        case dispatch_type::publish_status_type::backpressured:
            if (entry_value->output_enabled_ && !entry_value->cancel_requested_ && !stop_requested_) {
                entry_value->slot_.status_ = request_status_type::ready_to_publish;
                enqueue_blocked(entry_value->slot_, result_value.block_reason_);
            } else {
                cancel_entry(*entry_value, request_status_type::cancelled);
            }
            break;
        case dispatch_type::publish_status_type::not_ready:
            output_.remove(entry_value->slot_);
            entry_value->output_terminal_ = true;
            entry_value->slot_.status_ = request_status_type::failed;
            entry_value->publication_finished_.notify();
            require_connection_close(transport_close_reason_type::unexpected_request_state,
                http3_connection_error_code::internal_error);
            break;
        case dispatch_type::publish_status_type::fin_published:
        case dispatch_type::publish_status_type::complete:
            output_.remove(entry_value->slot_);
            entry_value->output_terminal_ = true;
            entry_value->slot_.status_ = request_status_type::published;
            entry_value->publication_finished_.notify();
            break;
        case dispatch_type::publish_status_type::cancelled: {
            const auto reason = entry_value->dispatch_.cancellation_reason();
            const bool already_cancelled_by_owner = entry_value->cancel_requested_;
            if (reason == dispatch_type::cancellation_reason_type::deadline) {
                cancel_deadline_entry(*entry_value);
            } else {
                output_.remove(entry_value->slot_);
                entry_value->output_enabled_ = false;
                entry_value->cancel_requested_ = true;
                entry_value->output_terminal_ = true;
                entry_value->slot_.status_ = request_status_type::cancelled;
                entry_value->publication_finished_.notify();
            }
            if (reason == dispatch_type::cancellation_reason_type::worker_stop && !stop_requested_) {
                (void)request_stop();
            } else if (!stop_requested_ && !already_cancelled_by_owner &&
                       reason != dispatch_type::cancellation_reason_type::deadline) {
                require_connection_close(transport_close_reason_type::publish_cancellation,
                    http3_connection_error_code::internal_error);
            }
            break;
        }
        case dispatch_type::publish_status_type::peer_limit_rejected:
            output_.remove(entry_value->slot_);
            (void)enqueue_reset_intent(entry_value->slot_);
            entry_value->output_terminal_ = true;
            entry_value->slot_.status_ = request_status_type::failed;
            entry_value->publication_finished_.notify();
            break;
        case dispatch_type::publish_status_type::failed:
        case dispatch_type::publish_status_type::wrong_worker:
            output_.remove(entry_value->slot_);
            entry_value->output_terminal_ = true;
            entry_value->slot_.status_ = request_status_type::failed;
            entry_value->publication_finished_.notify();
            require_connection_close(transport_close_reason_type::publish_failure,
                http3_connection_error_code::internal_error);
            break;
    }
    return {.status_ = publish_status_type::attempted,
        .stream_id_ = entry_value->stream_id_,
        .publication_ = result_value};
}

bool http3_server_connection::request_stop() & noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (stop_requested_) {
        return false;
    }
    require_connection_close(transport_close_reason_type::local_stop,
        http3_connection_error_code::no_error);
    return true;
}

task<void> http3_server_connection::join() & {
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
    if (requests_.active() != 0 || requests_.rejections() != 0 ||
        output_.ready() != 0 || output_.blocked() != 0 ||
        session_.active_stream_count() != 0) {
        std::terminate();
    }
}

http3_server_connection::request_info_type http3_server_connection::request_info(
    std::uint64_t stream_id) const noexcept {
    const auto* slot = requests_.find(stream_id);
    if (slot == nullptr) {
        return {};
    }
    return {slot->status_, slot->rejection_, slot->run_status_};
}

std::size_t http3_server_connection::active_request_count() const noexcept {
    return requests_.active();
}

std::size_t http3_server_connection::active_rejection_count() const noexcept {
    return requests_.rejections();
}

std::size_t http3_server_connection::active_task_count() const noexcept {
    return tasks_.size();
}

std::size_t http3_server_connection::ready_request_count() const noexcept {
    return output_.ready();
}

std::size_t http3_server_connection::tracked_request_count() const noexcept {
    return requests_.tracked();
}

std::size_t http3_server_connection::active_session_stream_count() const noexcept {
    return session_.active_stream_count();
}

bool http3_server_connection::drain_ready(
    std::size_t expected_admitted_requests) const noexcept {
    if (!on_worker() || admission_closed_ || stop_requested_ || retirement_.close_required() ||
        input_.stopped() || input_.observed_request_stream_count() != expected_admitted_requests ||
        input_.active_request_stream_count() != 0 || requests_.rejections() != 0 ||
        output_.ready() != 0 ||
        output_.blocked() != 0 || std::as_const(output_).queue(queue_kind_type::data_runnable).head_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::control_runnable).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::local_runnable).head_ != nullptr ||
        std::as_const(output_).queue(queue_kind_type::data_blocked).head_ != nullptr || std::as_const(output_).queue(queue_kind_type::control_blocked).head_ != nullptr ||
        session_.active_stream_count() != 0 || pending_transport_intent_count() != 0 ||
        retirement_.close_pending() || retirement_.handed_off()) {
        return false;
    }
    for (const auto& slot : requests_.slots()) {
        if ((slot.entry_ != nullptr &&
                (!slot.entry_->output_terminal_ || slot.entry_->dispatch_.handler_active())) ||
            slot.rejection_entry_ != nullptr || slot.reset_intent_pending_) {
            return false;
        }
    }
    return true;
}

bool http3_server_connection::stopped() const noexcept {
    return admission_closed_;
}

bool http3_server_connection::transport_close_required() const noexcept {
    return retirement_.close_required();
}

std::optional<http3_server_connection::transport_intent_type>
http3_server_connection::peek_transport_intent() const noexcept {
    auto selected = retirement_.next_intent();
    if (selected && selected->token_.kind_ == transport_intent_kind_type::connection_close) {
        return selected;
    }
    if (push_intent_head_ != nullptr && (!selected || push_intent_head_->token_.sequence_ < selected->token_.sequence_)) {
        selected = transport_intent_type{.token_ = push_intent_head_->token_};
    }
    return selected;
}

bool http3_server_connection::ack_transport_intent(
    const transport_intent_token_type& token, std::optional<push_stream_open_result_type> opened) & noexcept {
    if (!on_worker() || token.id_.epoch_ != epoch_ ||
        token.id_.connection_generation_ != connection_generation_) {
        return false;
    }
    if (token.kind_ == transport_intent_kind_type::open_push_stream) {
        if (!opened || !token.id_.push_id_ ||
            (opened->status_ == push_stream_open_result_type::status_type::opened &&
                http3_stream_id_type(opened->stream_id_) != http3_stream_id_type::server_unidirectional)) {
            return false;
        }
        for (auto* pending = pending_push_head_; pending != nullptr; pending = pending->next_) {
            if (pending != nullptr && pending->owed_ && pending->token_ == token) {
                pending->settle(*opened);
                notify_activation();
                return true;
            }
        }
        return false;
    }
    if (opened || !retirement_.acknowledge(token)) {
        return false;
    }
    notify_activation();
    return true;
}

bool http3_server_connection::take_over_transport_retirement(
    transport_retirement_takeover_type takeover) & noexcept {
    if (!on_worker() || takeover.epoch_ != epoch_ ||
        takeover.connection_generation_ != connection_generation_ || !stop_requested_ ||
        !retirement_.take_over()) {
        return false;
    }
    // The close intent and queued reset intents remain debts until each exact
    // token is acknowledged, even though the transport owner has retired it.
    notify_activation();
    return true;
}

bool http3_server_connection::confirm_transport_retired(
    transport_retirement_confirmation_type confirmation) & noexcept {
    if (!on_worker() || confirmation.epoch_ != epoch_ ||
        confirmation.connection_generation_ != connection_generation_ || !retirement_.confirm()) {
        return false;
    }
    // Preserve the complete intent ledger until network publishes an exact ACK
    // for each token. Retirement changes settlement disposition, not ownership.
    if (!admission_closed_) {
        stop_requested_ = true;
        stop_entries(false);
    }
    notify_activation();
    return true;
}

std::size_t http3_server_connection::pending_transport_intent_count() const noexcept {
    return retirement_.pending() + pending_push_intent_count_;
}

bool http3_server_connection::transport_retired() const noexcept {
    return retirement_.retired();
}

task<void> http3_server_connection::run_request(std::uint64_t stream_id) {
    auto* slot = requests_.find(stream_id);
    if (slot != nullptr && slot->push_id_ && (slot->push_cancelled_ || admission_closed_)) {
        slot->push_prefix_.reset();
        (void)session_.cancel_request(stream_id);
        slot->status_ = request_status_type::cancelled;
        (void)enqueue_reset_intent(*slot);
        co_return;
    }
    if (slot == nullptr || slot->status_ != request_status_type::admitting || slot->entry_ != nullptr) {
        if (slot != nullptr) {
            slot->status_ = request_status_type::failed;
        }
        notify_activation();
        require_connection_close(transport_close_reason_type::unexpected_request_state,
            http3_connection_error_code::internal_error);
        co_return;
    }

    request_retirement_type request_retirement{*this, stream_id};
    std::optional<request_entry_type> request;
    try {
        request.emplace(*this, *slot, stream_id);
    } catch (...) {
        slot->status_ = request_status_type::failed;
        notify_activation();
        require_connection_close(transport_close_reason_type::request_construction_failure,
            http3_connection_error_code::internal_error);
        co_return;
    }
    requests_.attach(*slot, *request);
    slot->status_ = request_status_type::running;
    entry_retirement_type retirement{*this, *request};
    auto& entry_value = *request;

    dispatch_type::run_status_type run_status = dispatch_type::run_status_type::failed;
    try {
        if (slot->push_prefix_) {
            auto prefix = std::move(*slot->push_prefix_);
            slot->push_prefix_.reset();
            co_await entry_value.dispatch_.publish_response_bytes(prefix);
        }
        run_status = co_await entry_value.dispatch_.run_handler();
        entry_value.slot_.run_status_ = run_status;
    } catch (...) {
        if (entry_value.cancel_requested_ || admission_closed_) {
            run_status = dispatch_type::run_status_type::cancelled;
            entry_value.slot_.run_status_ = run_status;
        } else {
            output_.remove(entry_value.slot_);
            entry_value.dispatch_.cancel();
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            notify_activation();
            require_connection_close(transport_close_reason_type::handler_failure,
                http3_connection_error_code::internal_error);
            co_return;
        }
    }

    if (run_status == dispatch_type::run_status_type::cancelled && !entry_value.cancel_requested_ &&
        !admission_closed_) {
        switch (entry_value.dispatch_.cancellation_reason()) {
            case dispatch_type::cancellation_reason_type::worker_stop:
                (void)request_stop();
                break;
            case dispatch_type::cancellation_reason_type::deadline:
                cancel_deadline_entry(entry_value);
                break;
            case dispatch_type::cancellation_reason_type::explicit_value:
                cancel_entry(entry_value, request_status_type::cancelled);
                break;
            case dispatch_type::cancellation_reason_type::none:
                entry_value.output_terminal_ = true;
                entry_value.slot_.status_ = request_status_type::failed;
                notify_activation();
                require_connection_close(transport_close_reason_type::handler_failure,
                    http3_connection_error_code::internal_error);
                break;
        }
    }

    if (run_status == dispatch_type::run_status_type::peer_field_section_limit) {
        output_.remove(entry_value.slot_);
        (void)enqueue_reset_intent(entry_value.slot_);
        entry_value.output_terminal_ = true;
        entry_value.slot_.status_ = request_status_type::failed;
        notify_activation();
        co_return;
    }

    if (run_status == dispatch_type::run_status_type::tunnel_complete || run_status == dispatch_type::run_status_type::output_complete) {
        if (!entry_value.output_terminal_ || entry_value.slot_.status_ != request_status_type::published) {
            entry_value.output_terminal_ = true;
            entry_value.slot_.status_ = request_status_type::failed;
            require_connection_close(transport_close_reason_type::unexpected_request_state,
                http3_connection_error_code::internal_error);
        }
        co_return;
    }

    if (run_status == dispatch_type::run_status_type::response_ready && entry_value.output_enabled_ &&
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
            notify_activation();
            require_connection_close(transport_close_reason_type::handler_failure,
                http3_connection_error_code::internal_error);
            co_return;
        }
        while (!entry_value.output_terminal_ && !entry_value.retirement_granted_) {
            co_await entry_value.publication_finished_.wait();
        }
        co_return;
    }

    if (run_status == dispatch_type::run_status_type::cancelled || entry_value.cancel_requested_ ||
        admission_closed_ || !entry_value.output_enabled_) {
        while (!entry_value.retirement_granted_) {
            co_await entry_value.publication_finished_.wait();
        }
        co_return;
    }

    entry_value.output_terminal_ = true;
    entry_value.slot_.status_ = request_status_type::failed;
    notify_activation();
    require_connection_close(transport_close_reason_type::handler_failure,
        http3_connection_error_code::internal_error);
}

task<bool> http3_server_connection::push_request(std::uint64_t parent_stream_id, http_push_request_view request) {
    auto* parent_value = requests_.find(parent_stream_id);
    const auto* original = session_.request(parent_stream_id);
    const auto maximum = session_.peer_max_push_id();
    if (!on_worker() || admission_closed_ || parent_value == nullptr || parent_value->push_id_ || parent_value->entry_ == nullptr ||
        parent_value->entry_->dispatch_.response_aborted() || original == nullptr || !maximum || next_push_id_ > *maximum || next_push_id_ >= session_.max_remembered_pushes()) {
        co_return false;
    }
    if (!http_ascii_equals_ignore_case(original->request().scheme(), request.scheme_) ||
        !http_authorities_equal(borrowed_text(original->request().authority()), borrowed_text(request.authority_),
            original->request().scheme() == "https" ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    if (pending_push_count_ >= std::min(max_tracked_streams_, std::size_t{32}) || requests_.tracked() + pending_push_count_ >= max_tracked_streams_ ||
        !retirement_.sequence_available()) {
        co_return false;
    }
    const auto push_id = next_push_id_;
    auto promise = session_.prepare_push_promise(parent_stream_id, push_id, request);
    if ((promise.index() != 0)) {
        if (std::get<1>(promise) == http3_connection_error_code::message_error) {
            throw std::invalid_argument("invalid HTTP/3 push request");
        }
        co_return false;
    }
    ++next_push_id_;
    pending_push_type pending(*this,
        {.kind_ = transport_intent_kind_type::open_push_stream,
            .id_ = {epoch_, connection_generation_, parent_stream_id, push_id},
            .sequence_ = retirement_.allocate_sequence()});
    notify_activation();
    while (!pending.opened_) {
        co_await pending.ready_.wait();
    }
    if (pending.opened_->status_ != push_stream_open_result_type::status_type::opened) {
        co_return false;
    }
    const auto stream_id = pending.opened_->stream_id_;
    bool created = false;
    auto* pushed = requests_.find_or_create(stream_id, created);
    if (pushed == nullptr || !created) {
        require_connection_close(transport_close_reason_type::request_index_capacity_exhausted, http3_connection_error_code::excessive_load);
        co_return false;
    }
    pushed->push_id_ = push_id;
    pushed->push_cancelled_ = pending.cancelled_;
    if (admission_closed_ || pending.cancelled_ || parent_value->entry_ == nullptr || parent_value->entry_->dispatch_.response_aborted()) {
        pushed->status_ = request_status_type::cancelled;
        (void)enqueue_reset_intent(*pushed);
        co_return false;
    }
    try {
        auto prefix = session_.admit_push_stream(stream_id, push_id);
        if ((prefix.index() != 0)) {
            pushed->status_ = request_status_type::cancelled;
            (void)enqueue_reset_intent(*pushed);
            co_return false;
        }
        pushed->push_prefix_.emplace(std::move(std::get<0>(prefix)));
        co_await parent_value->entry_->dispatch_.publish_response_bytes(std::get<0>(promise));
        if (admission_closed_ || pushed->push_cancelled_) {
            pushed->push_prefix_.reset();
            (void)session_.cancel_request(stream_id);
            pushed->status_ = request_status_type::cancelled;
            (void)enqueue_reset_intent(*pushed);
            co_return false;
        }
        const auto admitted = admit_finished_request(stream_id, {});
        co_return admitted.status_ == event_status_type::dispatched;
    } catch (...) {
        pushed->push_prefix_.reset();
        (void)session_.cancel_request(stream_id);
        pushed->status_ = request_status_type::cancelled;
        (void)enqueue_reset_intent(*pushed);
        throw;
    }
}

void http3_server_connection::observe_push_cancellation(std::uint64_t push_id) noexcept {
    for (auto* pending = pending_push_head_; pending != nullptr; pending = pending->next_) {
        if (pending != nullptr && pending->token_.id_.push_id_ == push_id) {
            pending->cancelled_ = true;
        }
    }
    for (auto& slot : requests_.slots()) {
        if (slot.occupied_ && slot.push_id_ == push_id) {
            slot.push_cancelled_ = true;
        }
    }
}

void http3_server_connection::process_push_cancellations() noexcept {
    for (auto& slot : requests_.slots()) {
        if (slot.occupied_ && slot.push_id_ && slot.push_cancelled_ && slot.entry_ != nullptr && !slot.entry_->retirement_granted_) {
            (void)session_.cancel_request(slot.stream_id_);
            (void)enqueue_reset_intent(slot);
            cancel_entry(*slot.entry_, request_status_type::cancelled);
        }
    }
}

void http3_server_connection::retire_request_input(std::uint64_t stream_id) noexcept {
    const auto* slot = requests_.find(stream_id);
    if (slot != nullptr && slot->push_id_) {
        (void)session_.cancel_request(stream_id);
    } else {
        (void)input_.cancel_request(stream_id);
    }
}

http3_server_connection::event_result_type
http3_server_connection::handle_input_result(std::uint64_t stream_id,
    input_type::result_type result_value, bool from_control) {
    process_push_cancellations();
    if (result_value.status_ == input_type::status_type::connection_closed) {
        (void)confirm_transport_retired(
            {.epoch_ = epoch_, .connection_generation_ = connection_generation_});
        return {.status_ = event_status_type::connection_closed,
            .input_ = result_value,
            .connection_close_required_ = false};
    }

    if (auto* slot = requests_.find(stream_id); slot != nullptr && slot->entry_ != nullptr) {
        slot->entry_->dispatch_.notify_tunnel_input();
    }

    if (result_value.status_ == input_type::status_type::protocol_error) {
        const bool connection_failure =
            result_value.protocol_.scope_ == http3_connection_error_scope::connection ||
            result_value.protocol_.status_ == http3_connection_status::connection_error || input_.stopped();
        if (connection_failure) {
            require_connection_close(transport_close_reason_type::connection_protocol_error,
                result_value.protocol_.code_);
            return {.status_ = event_status_type::protocol_error,
                .input_ = result_value,
                .connection_close_required_ = true};
        }

        bool created = false;
        auto* slot = requests_.find_or_create(stream_id, created);
        if (slot == nullptr) {
            require_connection_close(transport_close_reason_type::request_index_capacity_exhausted,
                http3_connection_error_code::excessive_load);
            return {.status_ = event_status_type::request_index_full,
                .input_ = result_value,
                .connection_close_required_ = true};
        }
        if (slot->interim_response_) {
            output_.remove(*slot);
            slot->interim_response_.reset();
        }
        if (slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::protocol_error);
        } else if (slot->rejection_entry_ != nullptr) {
            (void)retire_rejected_receive(stream_id, result_value, false);
            slot->status_ = request_status_type::protocol_error;
        } else {
            slot->status_ = request_status_type::protocol_error;
        }
        (void)enqueue_reset_intent(*slot, result_value.protocol_.code_,
            reset_intent_origin_type::stream_protocol_error);
        notify_activation();
        return {.status_ = event_status_type::protocol_error,
            .input_ = result_value,
            .connection_close_required_ = false};
    }

    if (session_.tunnel_input_overflowed(stream_id)) {
        bool created = false;
        auto* slot = requests_.find_or_create(stream_id, created);
        if (slot == nullptr) {
            require_connection_close(transport_close_reason_type::request_index_capacity_exhausted,
                http3_connection_error_code::excessive_load);
            return {.status_ = event_status_type::request_index_full,
                .input_ = result_value,
                .connection_close_required_ = true};
        }
        if (slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::cancelled);
        } else {
            slot->status_ = request_status_type::cancelled;
        }
        (void)enqueue_reset_intent(*slot);
        (void)retire_request_input(stream_id);
        notify_activation();
        return {.status_ = event_status_type::stream_cancelled,
            .input_ = result_value};
    }

    if (result_value.status_ == input_type::status_type::capacity_exhausted ||
        result_value.status_ == input_type::status_type::final_size_error) {
        const bool has_protocol_error =
            result_value.protocol_.code_ != http3_connection_error_code::no_error ||
            result_value.protocol_.scope_ != http3_connection_error_scope::none ||
            result_value.protocol_.status_ == http3_connection_status::stream_error ||
            result_value.protocol_.status_ == http3_connection_status::connection_error;
        const auto error_code = has_protocol_error
                                    ? result_value.protocol_.code_
                                : result_value.status_ == input_type::status_type::capacity_exhausted
                                    ? http3_connection_error_code::excessive_load
                                    : http3_connection_error_code::internal_error;
        require_connection_close(result_value.status_ == input_type::status_type::capacity_exhausted
                                     ? transport_close_reason_type::input_capacity_exhausted
                                     : transport_close_reason_type::final_size_error,
            error_code);
        return {.status_ = event_status_type::input_rejected,
            .input_ = result_value,
            .connection_close_required_ = true};
    }

    if (result_value.status_ == input_type::status_type::stopped) {
        require_connection_close(transport_close_reason_type::input_stopped,
            http3_connection_error_code::internal_error);
        return {.status_ = event_status_type::input_rejected,
            .input_ = result_value,
            .connection_close_required_ = true};
    }

    if (result_value.status_ == input_type::status_type::deferred_reset) {
        if (from_control) {
            auto* slot = requests_.find(stream_id);
            if (slot != nullptr && slot->interim_response_) {
                output_.remove(*slot);
                slot->interim_response_.reset();
                slot->status_ = request_status_type::cancelled;
            }
            if (slot != nullptr && slot->reset_intent_pending_) {
                retirement_.observe_peer_reset(*slot);
                notify_activation();
            }
            if (slot != nullptr && slot->entry_ != nullptr) {
                cancel_entry(*slot->entry_, request_status_type::cancelled);
            } else if (slot != nullptr && slot->rejection_entry_ != nullptr) {
                cancel_rejection_output(*slot->rejection_entry_, request_status_type::cancelled);
            }
        }
        return {.status_ = event_status_type::accepted,
            .input_ = result_value};
    }

    if (result_value.status_ == input_type::status_type::reset) {
        auto* slot = requests_.find(stream_id);
        if (slot != nullptr && slot->interim_response_) {
            output_.remove(*slot);
            slot->interim_response_.reset();
            slot->status_ = request_status_type::cancelled;
        }
        if (slot != nullptr && slot->reset_intent_pending_) {
            retirement_.observe_peer_reset(*slot);
            notify_activation();
        }
        if (slot != nullptr && slot->entry_ != nullptr) {
            cancel_entry(*slot->entry_, request_status_type::cancelled);
            return {.status_ = event_status_type::stream_cancelled,
                .input_ = result_value};
        }
        if (slot != nullptr && slot->rejection_entry_ != nullptr) {
            return retire_rejected_receive(stream_id, result_value, true);
        }
        return {.status_ = event_status_type::accepted,
            .input_ = result_value};
    }

    if (result_value.status_ == input_type::status_type::fed || result_value.status_ == input_type::status_type::finished) {
        const auto rejection = session_.rejection(stream_id);
        if (session_.request(stream_id) != nullptr && rejection != session::rejection_type::none) {
            return start_rejection(stream_id, rejection, result_value,
                result_value.status_ == input_type::status_type::finished);
        }
    }

    if (result_value.status_ == input_type::status_type::fed && session_.request(stream_id) != nullptr && session_.rejection(stream_id) == session::rejection_type::none) {
        if (!queue_continue_response(stream_id)) {
            return {.status_ = event_status_type::stream_cancelled, .input_ = result_value};
        }
    }

    if (result_value.status_ == input_type::status_type::fed || result_value.status_ == input_type::status_type::finished) {
        const auto* slot = requests_.find(stream_id);
        if (slot != nullptr && slot->request_started_ &&
            (slot->entry_ == nullptr || slot->entry_->dispatch_.complete())) {
            // A response can finish before peer FIN. Its dispatch has returned
            // the lease; only now may the input owner release receive storage.
            if (result_value.status_ == input_type::status_type::finished) {
                (void)session_.release(stream_id);
            }
            return {.status_ = event_status_type::accepted, .input_ = result_value};
        }
    }

    if ((result_value.status_ == input_type::status_type::fed ||
            result_value.status_ == input_type::status_type::finished) &&
        session_.stream_state(stream_id) == session::stream_state_type::ready) {
        const auto* request = session_.request(stream_id);
        // Both ordinary and extended CONNECT wait for a successful response
        // before sending tunnel data or FIN. Admit their ready heads now.
        if (request != nullptr && (request->request().known_method() == http_known_method::connect || session_.streaming_request(stream_id))) {
            return admit_finished_request(stream_id, result_value);
        }
    }

    if (result_value.status_ == input_type::status_type::finished) {
        // The protocol core must consume peer unidirectional streams first so
        // critical-stream termination and stream-type errors remain visible.
        // A successfully ignored unknown peer uni stream is not a Web request.
        if (is_http3_client_unidirectional_stream_id(stream_id)) {
            return {.status_ = event_status_type::accepted,
                .input_ = result_value};
        }
        return admit_finished_request(stream_id, result_value);
    }

    if (result_value.status_ == input_type::status_type::fed || result_value.status_ == input_type::status_type::deferred_qpack || result_value.status_ == input_type::status_type::deferred_fin ||
        result_value.status_ == input_type::status_type::ignored_control) {
        return {.status_ = event_status_type::accepted,
            .input_ = result_value};
    }
    return {.status_ = event_status_type::input_rejected,
        .input_ = result_value};
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
        require_connection_close(transport_close_reason_type::request_index_capacity_exhausted, http3_connection_error_code::excessive_load);
        return false;
    }
    if (slot->response_prelude_bytes_ != 0) {
        return true;
    }
    auto head = session_.encode_interim_response_head(stream_id, http_interim_response_head(http_status::continue_value));
    const auto peer_limit = session_.peer_max_field_section_size();
    if ((head.index() != 0) || (peer_limit && std::get<0>(head).field_section_.decoded_field_section_size() > *peer_limit)) {
        slot->status_ = request_status_type::cancelled;
        (void)enqueue_reset_intent(*slot);
        (void)retire_request_input(stream_id);
        notify_activation();
        return false;
    }
    std::pmr::vector<char> frame(worker_.resource());
    frame.resize(http3_frame_header_max_bytes + std::get<0>(head).field_section_.field_section_.size());
    const auto prefix = encode_http3_frame_header(frame, static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(head).field_section_.field_section_.size());
    if ((prefix.index() != 0)) {
        throw std::runtime_error("HTTP/3 continue response framing failed");
    }
    frame.resize(std::get<0>(prefix) + std::get<0>(head).field_section_.field_section_.size());
    std::copy(std::get<0>(head).field_section_.field_section_.begin(), std::get<0>(head).field_section_.field_section_.end(), frame.begin() + static_cast<std::ptrdiff_t>(std::get<0>(prefix)));
    slot->response_prelude_bytes_ = frame.size();
    slot->interim_response_.emplace(pending_interim_response_type{std::move(frame)});
    enqueue_for_demand(*slot, true);
    return true;
}

http3_server_connection::publish_attempt_type http3_server_connection::publish_interim_response(request_index_slot_type& slot) noexcept {
    output_.remove(slot);
    auto& pending = *slot.interim_response_;
    const auto bytes_value = std::span<const char>(pending.frame_).subspan(pending.offset_);
    const auto sent = outbound_.try_send({epoch_, connection_generation_, slot.stream_id_}, std::as_bytes(bytes_value.first(std::min(bytes_value.size(), http3_stream_buffer::max_block_bytes))));
    dispatch_type::publish_result_type result;
    switch (sent) {
        case http3_stream_buffer::send_result::sent: {
            const auto count = std::min(bytes_value.size(), http3_stream_buffer::max_block_bytes);
            pending.offset_ += count;
            result = {.status_ = dispatch_type::publish_status_type::bytes_published, .bytes_published_ = count};
            if (pending.offset_ == pending.frame_.size()) {
                slot.interim_response_.reset();
            }
            if (slot.interim_response_ || slot.rejection_entry_ || (slot.entry_ && slot.entry_->dispatch_.publication_demand() != dispatch_type::publication_demand_type::not_ready)) {
                enqueue_for_demand(slot, false);
            }
            break;
        }
        case http3_stream_buffer::send_result::full:
        case http3_stream_buffer::send_result::no_block:
            result = {.status_ = dispatch_type::publish_status_type::backpressured, .block_reason_ = dispatch_type::publish_block_reason_type::data};
            enqueue_blocked(slot, result.block_reason_);
            break;
        default:
            result = {.status_ = dispatch_type::publish_status_type::failed};
            require_connection_close(transport_close_reason_type::publish_failure, http3_connection_error_code::internal_error);
            break;
    }
    notify_activation();
    return {.status_ = publish_status_type::attempted, .stream_id_ = slot.stream_id_, .publication_ = result};
}

http3_server_connection::event_result_type
http3_server_connection::start_rejection(std::uint64_t stream_id,
    session::rejection_type rejection, input_type::result_type result_value, bool receive_finished) {
    const auto* request = session_.request(stream_id);
    if (request == nullptr || rejection == session::rejection_type::none) {
        return {.status_ = event_status_type::session_not_ready,
            .input_ = result_value,
            .rejection_ = rejection,
            .connection_close_required_ = true};
    }

    bool created = false;
    auto* slot = requests_.find_or_create(stream_id, created);
    if (slot == nullptr) {
        require_connection_close(transport_close_reason_type::request_index_capacity_exhausted,
            http3_connection_error_code::excessive_load);
        return {.status_ = event_status_type::request_index_full,
            .input_ = result_value,
            .rejection_ = rejection,
            .connection_close_required_ = true};
    }

    auto* entry_value = slot->rejection_entry_;
    bool rejection_created = false;
    if (entry_value == nullptr) {
        if (slot->entry_ != nullptr || (!created && slot->status_ != request_status_type::unknown)) {
            slot->status_ = request_status_type::failed;
            require_connection_close(transport_close_reason_type::unexpected_request_state,
                http3_connection_error_code::internal_error);
            return {.status_ = event_status_type::session_not_ready,
                .input_ = result_value,
                .rejection_ = rejection,
                .connection_close_required_ = true};
        }
        std::pmr::polymorphic_allocator<rejection_entry_type> allocator(worker_.resource());
        try {
            entry_value = allocator.allocate(1);
            try {
                std::construct_at(entry_value, *this, *slot, stream_id,
                    request->request().known_method(), rejection);
            } catch (...) {
                allocator.deallocate(entry_value, 1);
                entry_value = nullptr;
                throw;
            }
        } catch (...) {
            slot->status_ = request_status_type::failed;
            require_connection_close(transport_close_reason_type::request_construction_failure,
                http3_connection_error_code::internal_error);
            return {.status_ = event_status_type::session_not_ready,
                .input_ = result_value,
                .rejection_ = rejection,
                .connection_close_required_ = true};
        }
        requests_.attach_rejection(*slot, *entry_value);
        slot->status_ = request_status_type::rejected;
        slot->rejection_ = rejection;
        rejection_created = true;
    } else if (slot->entry_ != nullptr || slot->rejection_ != rejection) {
        slot->status_ = request_status_type::failed;
        require_connection_close(transport_close_reason_type::unexpected_request_state,
            http3_connection_error_code::internal_error);
        return {.status_ = event_status_type::session_not_ready,
            .input_ = result_value,
            .rejection_ = rejection,
            .connection_close_required_ = true};
    }

    if (receive_finished && !entry_value->receive_terminal_) {
        if (!session_.release(stream_id)) {
            require_connection_close(transport_close_reason_type::session_not_ready,
                http3_connection_error_code::internal_error);
            return {.status_ = event_status_type::session_not_ready,
                .input_ = result_value,
                .rejection_ = rejection,
                .connection_close_required_ = true};
        }
        entry_value->receive_terminal_ = true;
        if (entry_value->output_terminal_) {
            finish_rejection(*entry_value);
            return {.status_ = event_status_type::rejected,
                .input_ = result_value,
                .rejection_ = rejection};
        }
    }
    if (rejection_created) {
        enqueue_for_demand(*slot, true);
    }
    return {.status_ = event_status_type::rejected,
        .input_ = result_value,
        .rejection_ = rejection};
}

http3_server_connection::event_result_type
http3_server_connection::retire_rejected_receive(std::uint64_t stream_id,
    input_type::result_type result_value, bool reset) {
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->rejection_entry_ == nullptr) {
        return {.status_ = reset ? event_status_type::accepted : event_status_type::protocol_error,
            .input_ = result_value};
    }
    auto* entry_value = slot->rejection_entry_;
    entry_value->receive_terminal_ = true;
    if (!entry_value->output_terminal_) {
        cancel_rejection_output(*entry_value,
            reset ? request_status_type::cancelled : request_status_type::protocol_error);
    } else {
        finish_rejection(*entry_value);
    }
    return {.status_ = reset ? event_status_type::stream_cancelled : event_status_type::protocol_error,
        .input_ = result_value};
}

http3_server_connection::publish_attempt_type
http3_server_connection::publish_rejection(request_index_slot_type& slot,
    rejection_entry_type& entry_value) noexcept {
    const auto stream_id = entry_value.stream_id_;
    if (!entry_value.output_enabled_ || entry_value.output_terminal_ || !entry_value.output_) {
        slot.status_ = request_status_type::failed;
        require_connection_close(transport_close_reason_type::unexpected_request_state,
            http3_connection_error_code::internal_error);
        return {.status_ = publish_status_type::attempted,
            .stream_id_ = stream_id,
            .publication_ = {dispatch_type::publish_status_type::failed}};
    }

    const auto result_value = entry_value.output_->publish_step();
    dispatch_type::publish_result_type publication{};
    switch (result_value.status_) {
        case http3_buffered_response_output::status_type::bytes:
            publication = {dispatch_type::publish_status_type::bytes_published, result_value.bytes_accepted_};
            enqueue_for_demand(slot, false);
            break;
        case http3_buffered_response_output::status_type::fin:
            publication = {dispatch_type::publish_status_type::fin_published};
            entry_value.output_terminal_ = true;
            slot.status_ = request_status_type::published;
            entry_value.output_.reset();
            entry_value.response_.reset();
            finish_rejection(entry_value);
            break;
        case http3_buffered_response_output::status_type::backpressured: {
            publication.status_ = dispatch_type::publish_status_type::backpressured;
            switch (result_value.block_reason_) {
                case http3_buffered_response_output::block_reason_type::data:
                    publication.block_reason_ = dispatch_type::publish_block_reason_type::data;
                    break;
                case http3_buffered_response_output::block_reason_type::control:
                    publication.block_reason_ = dispatch_type::publish_block_reason_type::control;
                    break;
                case http3_buffered_response_output::block_reason_type::none:
                    publication.status_ = dispatch_type::publish_status_type::failed;
                    slot.status_ = request_status_type::failed;
                    entry_value.output_terminal_ = true;
                    require_connection_close(transport_close_reason_type::publish_failure,
                        http3_connection_error_code::internal_error);
                    break;
            }
            if (publication.status_ == dispatch_type::publish_status_type::backpressured) {
                enqueue_blocked(slot, publication.block_reason_);
            }
            break;
        }
        case http3_buffered_response_output::status_type::complete:
            publication = {dispatch_type::publish_status_type::complete};
            entry_value.output_terminal_ = true;
            slot.status_ = request_status_type::published;
            entry_value.output_.reset();
            entry_value.response_.reset();
            finish_rejection(entry_value);
            break;
        case http3_buffered_response_output::status_type::failed:
            publication = {dispatch_type::publish_status_type::failed, result_value.bytes_accepted_};
            slot.status_ = request_status_type::failed;
            entry_value.output_terminal_ = true;
            require_connection_close(transport_close_reason_type::publish_failure,
                http3_connection_error_code::internal_error);
            break;
    }
    return {.status_ = publish_status_type::attempted,
        .stream_id_ = stream_id,
        .publication_ = publication};
}

void http3_server_connection::cancel_rejection_output(
    rejection_entry_type& entry_value, request_status_type status) noexcept {
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
    notify_activation();
    finish_rejection(entry_value);
}

void http3_server_connection::finish_rejection(rejection_entry_type& entry_value) noexcept {
    if (!entry_value.output_terminal_ || !entry_value.receive_terminal_) {
        return;
    }
    auto& slot = entry_value.slot_;
    if (slot.queue_ != queue_kind_type::none || slot.rejection_entry_ != &entry_value ||
        requests_.rejections() == 0) {
        std::terminate();
    }
    requests_.retire_rejection(slot, entry_value);
    std::pmr::polymorphic_allocator<rejection_entry_type> allocator(worker_.resource());
    std::destroy_at(&entry_value);
    allocator.deallocate(&entry_value, 1);
}

http3_server_connection::event_result_type
http3_server_connection::admit_finished_request(
    std::uint64_t stream_id, input_type::result_type result_value) {
    const auto stream_state = session_.stream_state(stream_id);
    if (stream_state != session::stream_state_type::ready) {
        bool created = false;
        auto* slot = requests_.find_or_create(stream_id, created);
        if (slot != nullptr) {
            slot->status_ = request_status_type::failed;
        }
        notify_activation();
        require_connection_close(transport_close_reason_type::session_not_ready,
            http3_connection_error_code::internal_error);
        return {.status_ = event_status_type::session_not_ready,
            .input_ = result_value,
            .connection_close_required_ = true};
    }

    bool created = false;
    auto* slot = requests_.find_or_create(stream_id, created);
    if (slot == nullptr) {
        require_connection_close(transport_close_reason_type::request_index_capacity_exhausted,
            http3_connection_error_code::excessive_load);
        return {.status_ = event_status_type::request_index_full,
            .input_ = result_value,
            .connection_close_required_ = true};
    }
    if (slot->request_started_ || slot->status_ == request_status_type::cancelled || slot->status_ == request_status_type::protocol_error) {
        return slot->entry_ != nullptr ? event_result_type{.status_ = event_status_type::accepted, .input_ = result_value}
                                       : event_result_type{.status_ = event_status_type::input_rejected, .input_ = result_value};
    }
    slot->request_started_ = true;
    slot->status_ = request_status_type::admitting;
    try {
        auto task_value = run_request(stream_id);
        tasks_.spawn(std::move(task_value));
        ever_spawned_ = true;
    } catch (...) {
        slot->status_ = request_status_type::failed;
        require_connection_close(transport_close_reason_type::dispatch_start_failure,
            http3_connection_error_code::internal_error);
        return {.status_ = event_status_type::dispatch_start_failed,
            .input_ = result_value,
            .connection_close_required_ = true};
    }
    return {.status_ = event_status_type::dispatched,
        .input_ = result_value};
}

void http3_server_connection::enqueue_runnable(
    request_index_slot_type& slot, queue_kind_type kind, bool notify) noexcept {
    if (kind != queue_kind_type::data_runnable && kind != queue_kind_type::control_runnable &&
        kind != queue_kind_type::local_runnable) {
        std::terminate();
    }
    const auto* entry_value = slot.entry_;
    const auto* rejection = slot.rejection_entry_;
    if (slot.queue_ != queue_kind_type::none || stop_requested_ ||
        (!slot.interim_response_ && (entry_value == nullptr) == (rejection == nullptr)) ||
        (entry_value != nullptr && (!entry_value->output_enabled_ || entry_value->output_terminal_)) ||
        (rejection != nullptr && (!rejection->output_enabled_ || rejection->output_terminal_))) {
        return;
    }
    const bool was_lane_empty = output_.enqueue(slot, kind);
    if (notify && was_lane_empty) {
        notify_activation();
    }
}

void http3_server_connection::enqueue_for_demand(
    request_index_slot_type& slot, bool notify) noexcept {
    if (slot.interim_response_) {
        if (slot.queue_ != queue_kind_type::data_runnable) {
            output_.remove(slot);
            enqueue_runnable(slot, queue_kind_type::data_runnable, notify);
        }
        return;
    }
    auto* entry_value = slot.entry_;
    auto* rejection = slot.rejection_entry_;
    if ((entry_value == nullptr) == (rejection == nullptr)) {
        std::terminate();
    }
    queue_kind_type desired = queue_kind_type::local_runnable;
    if (rejection != nullptr) {
        switch (rejection->output_->next_step()) {
            case ruvia::http3_buffered_response_cursor::step::bytes:
                desired = queue_kind_type::data_runnable;
                break;
            case ruvia::http3_buffered_response_cursor::step::fin:
                desired = queue_kind_type::control_runnable;
                break;
            case ruvia::http3_buffered_response_cursor::step::complete:
            case ruvia::http3_buffered_response_cursor::step::failed:
                break;
        }
    } else {
        switch (entry_value->dispatch_.publication_demand()) {
            case dispatch_type::publication_demand_type::data:
                desired = queue_kind_type::data_runnable;
                break;
            case dispatch_type::publication_demand_type::control:
                desired = queue_kind_type::control_runnable;
                break;
            case dispatch_type::publication_demand_type::local_complete:
            case dispatch_type::publication_demand_type::local_cancelled:
            case dispatch_type::publication_demand_type::local_buffer_stopped:
            case dispatch_type::publication_demand_type::local_peer_limit_rejected:
            case dispatch_type::publication_demand_type::local_failed:
            case dispatch_type::publication_demand_type::not_ready:
            case dispatch_type::publication_demand_type::wrong_worker:
                break;
        }
    }
    if (slot.queue_ == desired) {
        return;
    }
    output_.remove(slot);
    enqueue_runnable(slot, desired, notify);
}

void http3_server_connection::enqueue_blocked(
    request_index_slot_type& slot, dispatch_type::publish_block_reason_type reason) noexcept {
    queue_kind_type kind = queue_kind_type::none;
    switch (reason) {
        case dispatch_type::publish_block_reason_type::data:
            kind = queue_kind_type::data_blocked;
            break;
        case dispatch_type::publish_block_reason_type::control:
            kind = queue_kind_type::control_blocked;
            break;
        case dispatch_type::publish_block_reason_type::none:
            std::terminate();
    }
    const auto* entry_value = slot.entry_;
    const auto* rejection = slot.rejection_entry_;
    if (slot.queue_ != queue_kind_type::none || stop_requested_ ||
        (!slot.interim_response_ && (entry_value == nullptr) == (rejection == nullptr)) ||
        (entry_value != nullptr && (!entry_value->output_enabled_ || entry_value->output_terminal_)) ||
        (rejection != nullptr && (!rejection->output_enabled_ || rejection->output_terminal_))) {
        return;
    }
    (void)output_.enqueue(slot, kind);
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
    // Fence output before installing the receive tombstone. A published FIN is
    // already accepted by QUIC; the transport owner decides whether its send state
    // still requires a reset, so the worker must not guess by publishing one.
    entry_value.output_enabled_ = false;
    entry_value.cancel_requested_ = true;
    output_.remove(entry_value.slot_);
    (void)retire_request_input(entry_value.stream_id_);
    // FIN publication only closes the local send direction. The transport owner
    // still needs a per-stream retirement token to stop receiving if the peer
    // never finishes its direction.
    (void)enqueue_reset_intent(entry_value.slot_);
    cancel_entry(entry_value, request_status_type::cancelled);
}

void http3_server_connection::cancel_entry(
    request_entry_type& entry_value, request_status_type status) noexcept {
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

void http3_server_connection::stop_entries(bool input_already_stopped) noexcept {
    admission_closed_ = true;
    critical_output_blocked_ = false;

    // First quiesce every output so cancellation of one handler cannot let a
    // sibling publish while the connection is being retired.
    for (auto& slot : requests_.slots()) {
        if (slot.interim_response_) {
            output_.remove(slot);
            slot.interim_response_.reset();
        }
        if (auto* rejection = slot.rejection_entry_;
            rejection != nullptr && !rejection->output_terminal_) {
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
    if (!input_already_stopped) {
        input_.stop();
    }

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

void http3_server_connection::notify_activation() noexcept {
    if (!on_worker() || !activation_.valid()) {
        std::terminate();
    }
    const auto activation = activation_snapshot();
    activation_.activate_(activation_.context_, epoch_, connection_generation_,
        activation_.slot_generation_, activation);
}

http3_server_connection::worker_activation_type
http3_server_connection::activation_snapshot() const noexcept {
    const auto intent = peek_transport_intent();
    return {.work_ = work_state(),
        .transport_intent_ = intent ? std::optional<transport_intent_token_type>(intent->token_) : std::nullopt};
}

bool http3_server_connection::detach_activation_after_join() noexcept {
    if (!on_worker() || !join_completed_ || !admission_closed_ || requests_.active() != 0 ||
        requests_.rejections() != 0 || tasks_.size() != 0 || pending_transport_intent_count() != 0 ||
        (!retirement_.retired() && !retirement_.taken_over() && !retirement_.handed_off())) {
        return false;
    }
    activation_ = {};
    return true;
}

bool http3_server_connection::enqueue_reset_intent(request_index_slot_type& slot, http3_connection_error_code error_code, reset_intent_origin_type origin) noexcept {
    switch (retirement_.enqueue_reset(slot, error_code, origin)) {
        case transport_retirement::reset_result::queued:
            return true;
        case transport_retirement::reset_result::unavailable:
            return false;
        case transport_retirement::reset_result::capacity_exhausted:
            require_connection_close(transport_close_reason_type::transport_intent_capacity_exhausted, http3_connection_error_code::internal_error);
            return false;
        case transport_retirement::reset_result::sequence_exhausted:
            require_connection_close(transport_close_reason_type::transport_intent_sequence_exhausted, http3_connection_error_code::internal_error);
            return false;
    }
    std::terminate();
}

void http3_server_connection::require_connection_close(
    transport_close_reason_type reason, std::optional<http3_connection_error_code> error_code) noexcept {
    retirement_.require_close(reason, error_code);
    stop_requested_ = true;
    if (!admission_closed_) {
        stop_entries(false);
    }
}

void http3_server_connection::finish_entry(request_entry_type& entry_value) noexcept {
    if (entry_value.slot_.queue_ != queue_kind_type::none || entry_value.dispatch_.handler_active() ||
        !entry_value.output_terminal_ || entry_value.slot_.entry_ != &entry_value || requests_.active() == 0) {
        std::terminate();
    }
    requests_.retire(entry_value.slot_, entry_value);
}

bool http3_server_connection::on_worker() const noexcept {
    return services_.worker().is_current();
}

bool http3_server_connection::attach_tunnel_scanner(
    std::uint64_t stream_id, connection_scanner::entry_type& entry_value) noexcept {
    if (!on_worker() || connection_scanner_ == nullptr) {
        return false;
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr ||
        &slot->entry_->scanner_entry_ != &entry_value) {
        return false;
    }
    if (slot->entry_->scanner_registered_) {
        return true;
    }
    connection_scanner_->register_entry(entry_value);
    slot->entry_->scanner_registered_ = true;
    return true;
}

void http3_server_connection::tunnel_output_ready(std::uint64_t stream_id) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr || slot->entry_->output_terminal_ ||
        slot->entry_->cancel_requested_ || !slot->entry_->output_enabled_) {
        return;
    }
    enqueue_for_demand(*slot, true);
}

void http3_server_connection::abort_tunnel(std::uint64_t stream_id) noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    auto* slot = requests_.find(stream_id);
    if (slot == nullptr || slot->entry_ == nullptr || slot->entry_->retirement_granted_ ||
        slot->entry_->cancel_requested_) {
        return;
    }
    auto& entry_value = *slot->entry_;
    // Match deadline cancellation: fence publication, tombstone input, then
    // retain a stream retirement intent even if output FIN was already accepted.
    entry_value.output_enabled_ = false;
    entry_value.cancel_requested_ = true;
    output_.remove(entry_value.slot_);
    (void)retire_request_input(stream_id);
    // Preserve a retirement intent after local FIN; the transport owner chooses
    // STOP_SENDING versus RESET_STREAM from the actual transport send state.
    (void)enqueue_reset_intent(*slot);
    cancel_entry(entry_value, request_status_type::cancelled);
}

bool http3_server_connection::attach_tunnel_scanner_thunk(
    void* context_value, std::uint64_t stream_id, connection_scanner::entry_type& entry_value) noexcept {
    return static_cast<http3_server_connection*>(context_value)->attach_tunnel_scanner(stream_id, entry_value);
}

void http3_server_connection::request_input_consumed_thunk(void* context_value) noexcept {
    auto& owner_value = *static_cast<http3_server_connection*>(context_value);
    auto activation = owner_value.activation_snapshot();
    activation.input_capacity_available_ = true;
    owner_value.activation_.activate_(owner_value.activation_.context_, owner_value.epoch_, owner_value.connection_generation_, owner_value.activation_.slot_generation_, activation);
}

void http3_server_connection::tunnel_output_ready_thunk(
    void* context_value, std::uint64_t stream_id) noexcept {
    static_cast<http3_server_connection*>(context_value)->tunnel_output_ready(stream_id);
}

void http3_server_connection::abort_tunnel_thunk(
    void* context_value, std::uint64_t stream_id) noexcept {
    static_cast<http3_server_connection*>(context_value)->abort_tunnel(stream_id);
}

std::size_t http3_server_connection::index_capacity(std::size_t max_tracked_streams) {
    if (max_tracked_streams == 0 ||
        max_tracked_streams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::invalid_argument("HTTP/3 request index capacity must be positive and bounded");
    }
    const auto needed = max_tracked_streams * 2;
    const auto max_power_of_two = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > max_power_of_two) {
        throw std::length_error("HTTP/3 request index capacity is too large");
    }
    return std::bit_ceil(needed);
}

}  // namespace ruvia::detail
