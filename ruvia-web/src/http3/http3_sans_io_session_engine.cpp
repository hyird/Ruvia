#include "http3/http3_sans_io_session_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_known_method.h"

#include "router/route_endpoint.h"
#include "router/route_table.h"
#include "server/request_body_limit.h"

namespace ruvia::detail {

struct http3_sans_io_session_engine::stream_type final {
    stream_type(worker_memory& worker_value, std::uint64_t stream_id, std::pmr::memory_resource* inbound_pool)
        : id_(stream_id),
          memory_(worker_value),
          trailers_(memory_.resource()),
          tunnel_input_(inbound_pool) {}

    const std::uint64_t id_;
    std::optional<std::uint64_t> push_id_{};
    // request_memory must outlive the head, request and body that use its arena.
    request_memory memory_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_update_{};
    std::optional<http3_server_request> request_;
    route_resolution resolution_;
    stream_state_type state_{stream_state_type::receiving};
    rejection_type rejection_{rejection_type::none};
    std::size_t body_limit_{};
    std::size_t received_body_bytes_{};
    bool streaming_body_{};
    rejection_type body_failure_{rejection_type::none};
    std::pmr::string tunnel_input_;
    std::optional<std::pmr::deque<std::pmr::string>> datagrams_;
    std::size_t tunnel_input_read_offset_{};
    std::size_t tunnel_buffered_bytes_{};
    bool connect_request_{};
    bool tunnel_input_overflow_{};
    bool tunnel_receive_ended_{};
    bool tunnel_reset_{};
    bool pending_finish_{false};
    bool receive_ended_{false};
    bool leased_{false};
    bool dispatched_{false};
    bool retired_{false};
};

void http3_sans_io_session_engine::stream_deleter_type::operator()(stream_type* stream) const noexcept {
    if (stream == nullptr) {
        return;
    }
    std::pmr::polymorphic_allocator<stream_type> allocator(resource_);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, stream);
    allocator.deallocate(stream, 1);
}

http3_sans_io_session_engine::http3_sans_io_session_engine(const route_table& routes_value,
    worker_memory& worker_value, http3_sans_io_session_limits limits)
    : routes_(routes_value),
      worker_(worker_value),
      limits_(limits),
      inbound_buffers_(limits.inbound_buffer_pool_ != nullptr ? limits.inbound_buffer_pool_ : worker_value.resource(),
          limits.max_inbound_buffer_bytes_),
      connection_(http3_peer_role::server, worker_value.resource(), limits.connection_),
      control_output_(worker_value.resource()),
      streams_(worker_value.resource()) {
    if (limits_.max_buffered_body_bytes_ == 0 || limits_.max_live_streams_ == 0 ||
        limits_.max_buffered_bytes_in_flight_ == 0 || limits_.max_tunnel_buffered_bytes_ == 0 || limits_.max_stream_backlog_bytes_ < 4096) {
        throw std::invalid_argument("HTTP/3 session limits must be greater than zero");
    }
}

http3_sans_io_session_engine::http3_sans_io_session_engine(const route_table& routes_value,
    worker_memory& worker_value, http3_server_body_budget& body_budget, http3_sans_io_session_limits limits)
    : http3_sans_io_session_engine(routes_value, worker_value, limits) {
    body_budget_ = &body_budget;
}

http3_sans_io_session_engine::~http3_sans_io_session_engine() {
    if (active_leases_ != 0) {
        std::terminate();
    }
    if (body_budget_ != nullptr) {
        streams_.clear();
        body_budget_->release(buffered_bytes_in_flight_ + tunnel_bytes_in_flight_);
        buffered_bytes_in_flight_ = 0;
        tunnel_bytes_in_flight_ = 0;
    }
}

http3_sans_io_session_engine::request_lease_type::~request_lease_type() {
    if (owner_) {
        owner_->release_lease(*stream_);
    }
}

http3_sans_io_session_engine::request_lease_type::request_lease_type(request_lease_type&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      stream_(std::exchange(other.stream_, nullptr)) {}

const http3_server_request& http3_sans_io_session_engine::request_lease_type::request() const& noexcept {
    if (!owner_) {
        std::terminate();
    }
    return *stream_->request_;
}

const route_resolution& http3_sans_io_session_engine::request_lease_type::resolution() const& noexcept {
    if (!owner_) {
        std::terminate();
    }
    return stream_->resolution_;
}

std::optional<http3_sans_io_session_engine::request_lease_type> http3_sans_io_session_engine::acquire_request(
    std::uint64_t stream_id) & noexcept {
    const auto found = streams_.find(stream_id);
    if (terminated_ || found == streams_.end() || found->second->dispatched_ ||
        found->second->retired_ || found->second->state_ != stream_state_type::ready) {
        return std::nullopt;
    }
    found->second->leased_ = true;
    found->second->dispatched_ = true;
    ++active_leases_;
    return request_lease_type(*this, *found->second);
}

std::variant<std::pmr::vector<char>, http3_connection_error_code> http3_sans_io_session_engine::admit_push_stream(
    std::uint64_t stream_id, std::uint64_t push_id) {
    const auto* head = connection_.promised_request(push_id);
    if (terminated_ || head == nullptr || streams_.contains(stream_id) || streams_.size() >= limits_.max_live_streams_) {
        return http3_connection_error_code::request_rejected;
    }
    std::pmr::polymorphic_allocator<stream_type> allocator(worker_.resource());
    auto* raw_stream = allocator.allocate(1);
    try {
        allocator.construct(raw_stream, worker_, stream_id, &inbound_buffers_);
    } catch (...) {
        allocator.deallocate(raw_stream, 1);
        throw;
    }
    stream_ptr_type stream(raw_stream, stream_deleter_type{worker_.resource()});
    stream->push_id_ = push_id;
    stream->request_.emplace(*head, stream->memory_.resource(), &inbound_buffers_);
    stream->request_->finish_body();
    const auto& request = stream->request_->request();
    // Push admission and completed receive state remain separate from ordinary
    // request admission. Promise targets are already normalized by HTTP.
    stream->resolution_ = routes_.resolve(route_request_view{request.known_method(),
        request.method(), request.path(), request.path(), stream->request_->extended_connect_protocol()});
    stream->state_ = stream_state_type::ready;
    stream->receive_ended_ = true;
    auto [inserted, fresh] = streams_.emplace(stream_id, std::move(stream));
    if (!fresh) {
        std::terminate();
    }
    try {
        auto prefix = connection_.prepare_push_stream(stream_id, push_id);
        if ((prefix.index() != 0)) {
            streams_.erase(inserted);
        }
        return prefix;
    } catch (...) {
        streams_.erase(inserted);
        throw;
    }
}

void http3_sans_io_session_engine::observe_push_cancellation(void* context_value, void (*cancel)(void*, std::uint64_t) noexcept) {
    if (push_cancellation_ != nullptr || context_value == nullptr || cancel == nullptr) {
        throw std::invalid_argument("HTTP/3 push cancellation requires a single stable owner");
    }
    push_cancellation_context_ = context_value;
    push_cancellation_ = cancel;
}

void http3_sans_io_session_engine::release_lease(stream_type& stream) noexcept {
    if (!stream.leased_ || active_leases_ == 0) {
        std::terminate();
    }
    stream.leased_ = false;
    --active_leases_;
    if (stream.retired_) {
        release_stream(stream.id_);
    }
}

void http3_sans_io_session_engine::bind_control_output_wake(void* context_value, void (*wake)(void*) noexcept) {
    if ((context_value == nullptr) != (wake == nullptr) || control_wake_ != nullptr) {
        throw std::invalid_argument("HTTP/3 control output requires one stable wake owner");
    }
    control_wake_context_ = context_value;
    control_wake_ = wake;
}
bool http3_sans_io_session_engine::queue_origin_advertisement(std::span<const std::string_view> origins) {
    if (terminated_) {
        return false;
    }
    auto frame = connection_.prepare_origin_advertisement(origins);
    if ((frame.index() != 0) || std::get<0>(frame).size() > max_http_header_bytes - std::min(control_output_.size(), max_http_header_bytes)) {
        return false;
    }
    control_output_.append(std::get<0>(frame).data(), std::get<0>(frame).size());
    if (control_wake_ != nullptr) {
        control_wake_(control_wake_context_);
    }
    return true;
}
bool http3_sans_io_session_engine::consume_control_output(std::size_t bytes_value) noexcept {
    if (bytes_value > control_output_.size()) {
        return false;
    }
    control_output_.erase(0, bytes_value);
    if (control_output_.empty()) {
        std::pmr::string(worker_.resource()).swap(control_output_);
    }
    return true;
}

http3_connection_result http3_sans_io_session_engine::feed(std::uint64_t stream_id,
    std::string_view bytes_value, bool fin, bool reset) noexcept {
    if (terminated_) {
        return failure_;
    }
    try {
        const auto input = std::span<const char>(bytes_value.data(), bytes_value.size());
        const auto result_value = connection_.feed(
            stream_id, input, fin, reset, &http3_sans_io_session_engine::on_connection_event, this);
        if (result_value.scope_ == http3_connection_error_scope::connection ||
            result_value.status_ == http3_connection_status::connection_error) {
            terminate(result_value);
            return result_value;
        }
        if (result_value.scope_ == http3_connection_error_scope::stream || reset ||
            result_value.status_ == http3_connection_status::reset) {
            // The core has retired this receive stream. Do not retain its Web
            // body or headers while unrelated requests continue on this QUIC
            // connection. RESET after request FIN need not emit a core event;
            // it still retires Web state, deferring destruction for a lease.
            release_stream(stream_id);
            return result_value;
        }
        // The callback only marked a candidate. Publish it after this stream's
        // feed returned successfully, without walking unrelated live streams.
        const auto found = streams_.find(stream_id);
        if (found != streams_.end() && found->second->pending_finish_ &&
            found->second->state_ == stream_state_type::receiving) {
            found->second->request_->finish_body();
            found->second->state_ = stream_state_type::ready;
            found->second->pending_finish_ = false;
        }
        return result_value;
    } catch (const std::length_error&) {
        const http3_connection_result failure{http3_connection_status::connection_error,
            http3_connection_error_scope::connection, http3_connection_error_code::excessive_load};
        terminate(failure);
        return failure;
    } catch (...) {
        const http3_connection_result failure{http3_connection_status::connection_error,
            http3_connection_error_scope::connection, http3_connection_error_code::internal_error};
        terminate(failure);
        return failure;
    }
}

void http3_sans_io_session_engine::on_connection_event(void* context_value,
    const http3_connection_event& event) {
    static_cast<http3_sans_io_session_engine*>(context_value)->handle_event(event);
}

void http3_sans_io_session_engine::handle_event(const http3_connection_event& event) {
    if (event.kind_ == http3_connection_event_kind::push_canceled) {
        if (event.push_id_ && push_cancellation_ != nullptr) {
            push_cancellation_(push_cancellation_context_, *event.push_id_);
        }
        return;
    }
    if (event.kind_ == http3_connection_event_kind::priority_update) {
        if (event.priority_update_ && !event.priority_update_->push_) {
            if (const auto found = streams_.find(event.priority_update_->element_id_); found != streams_.end()) {
                found->second->priority_update_ = event.priority_update_->fields_.request_priority();
            }
        } else if (event.priority_update_) {
            for (auto& [id, stream] : streams_) {
                if (stream->push_id_ == event.priority_update_->element_id_) {
                    stream->priority_update_ = event.priority_update_->fields_.request_priority();
                    break;
                }
            }
        }
        return;
    }
    if (event.kind_ == http3_connection_event_kind::reset) {
        if (const auto found = streams_.find(event.stream_id_); found != streams_.end()) {
            found->second->tunnel_reset_ = true;
        }
        release_stream(event.stream_id_);
        return;
    }
    if (event.kind_ == http3_connection_event_kind::request_head) {
        if (event.head_ == nullptr || streams_.contains(event.stream_id_)) {
            throw std::logic_error("invalid or duplicate HTTP/3 request head event");
        }
        if (streams_.size() >= limits_.max_live_streams_) {
            throw std::length_error("HTTP/3 live stream budget exhausted");
        }
        std::pmr::polymorphic_allocator<stream_type> allocator(worker_.resource());
        auto* raw_stream = allocator.allocate(1);
        try {
            allocator.construct(raw_stream, worker_, event.stream_id_, &inbound_buffers_);
        } catch (...) {
            allocator.deallocate(raw_stream, 1);
            throw;
        }
        stream_ptr_type stream(raw_stream, stream_deleter_type{worker_.resource()});
        stream->request_.emplace(*event.head_, stream->memory_.resource(),
            &inbound_buffers_);
        const auto& request = stream->request_->request();
        stream->connect_request_ = request.known_method() == http_known_method::connect;
        stream->resolution_ = routes_.resolve(route_request_view{request.known_method(),
            request.method(), request.path(), request.authority(), stream->request_->extended_connect_protocol()});
        stream->body_limit_ = request_body_byte_limit(request_body_mode::buffered,
            std::nullopt, limits_.max_buffered_body_bytes_)
                                  .read_ceiling();

        const auto expectation = stream->request_->expectation_plan(http_unsupported_expectation_policy::reject);
        if (expectation.rejection() != nullptr) {
            stream->state_ = stream_state_type::rejected;
            stream->rejection_ = rejection_type::expectation_unsupported;
        } else if (stream->connect_request_) {
            const auto* resolved = stream->resolution_.resolved();
            if (resolved == nullptr ||
                (resolved->route().endpoint().tunnel() == nullptr && resolved->route().endpoint().get_websocket() == nullptr)) {
                stream->state_ = stream_state_type::rejected;
                stream->rejection_ = rejection_type::connect_unsupported;
            } else {
                stream->state_ = stream_state_type::ready;
            }
        } else if (const auto* resolved = stream->resolution_.resolved()) {
            const auto& endpoint = resolved->route().endpoint();
            if (endpoint.get_websocket() != nullptr) {
                stream->state_ = stream_state_type::rejected;
                stream->rejection_ = rejection_type::websocket_unsupported;
            } else if (const auto* buffered = endpoint.buffered()) {
                stream->streaming_body_ = buffered->request_body_mode() == request_body_mode::stream;
                stream->body_limit_ = request_body_byte_limit(buffered->request_body_mode(),
                    limits_.max_stream_body_bytes_, limits_.max_buffered_body_bytes_,
                    resolved->route().max_request_body_bytes())
                                          .read_ceiling();
                if (stream->streaming_body_) {
                    stream->state_ = stream_state_type::ready;
                }
            }
        }
        if (stream->state_ != stream_state_type::rejected && !stream->connect_request_ &&
            event.head_->content_length_.has_value() &&
            *event.head_->content_length_ > stream->body_limit_) {
            stream->state_ = stream_state_type::rejected;
            stream->rejection_ = rejection_type::body_too_large;
        }
        streams_.emplace(event.stream_id_, std::move(stream));
        return;
    }

    const auto found = streams_.find(event.stream_id_);
    if (found == streams_.end()) {
        return;
    }
    auto& stream = *found->second;
    if (stream.state_ == stream_state_type::rejected) {
        if (event.kind_ == http3_connection_event_kind::message_end) {
            stream.receive_ended_ = true;
        }
        return;
    }
    switch (event.kind_) {
        case http3_connection_event_kind::body: {
            if (stream.streaming_body_) {
                if (stream.body_failure_ != rejection_type::none) {
                    return;
                }
                if (event.body_.size() > stream.body_limit_ - stream.received_body_bytes_) {
                    stream.body_failure_ = rejection_type::body_too_large;
                    return;
                }
                stream.received_body_bytes_ += event.body_.size();
                if (stream.tunnel_buffered_bytes_ > limits_.max_stream_backlog_bytes_ ||
                    event.body_.size() > limits_.max_stream_backlog_bytes_ - stream.tunnel_buffered_bytes_ ||
                    buffered_bytes_in_flight_ > limits_.max_buffered_bytes_in_flight_ ||
                    tunnel_bytes_in_flight_ > limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ ||
                    event.body_.size() > limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ - tunnel_bytes_in_flight_ ||
                    (body_budget_ != nullptr && !body_budget_->try_reserve(event.body_.size()))) {
                    stream.body_failure_ = rejection_type::in_flight_body_capacity;
                    return;
                }
                try {
                    if (stream.tunnel_input_read_offset_ != 0) {
                        stream.tunnel_input_.erase(0, stream.tunnel_input_read_offset_);
                        stream.tunnel_input_read_offset_ = 0;
                    }
                    stream.tunnel_input_.append(event.body_.data(), event.body_.size());
                } catch (const inbound_buffer_limit_error&) {
                    if (body_budget_ != nullptr) {
                        body_budget_->release(event.body_.size());
                    }
                    stream.body_failure_ = rejection_type::worker_body_budget_exhausted;
                    return;
                } catch (...) {
                    if (body_budget_ != nullptr) {
                        body_budget_->release(event.body_.size());
                    }
                    throw;
                }
                stream.tunnel_buffered_bytes_ += event.body_.size();
                tunnel_bytes_in_flight_ += event.body_.size();
                return;
            }
            if (stream.request_ == std::nullopt || stream.state_ != stream_state_type::receiving) {
                throw std::logic_error("HTTP/3 body arrived outside a buffered request");
            }
            const auto current = stream.request_->body_bytes();
            if (current > stream.body_limit_ || event.body_.size() > stream.body_limit_ - current) {
                reject_body(stream, rejection_type::body_too_large);
                return;
            }
            if (buffered_bytes_in_flight_ > limits_.max_buffered_bytes_in_flight_ ||
                tunnel_bytes_in_flight_ > limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ ||
                event.body_.size() > limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ - tunnel_bytes_in_flight_) {
                reject_body(stream, rejection_type::in_flight_body_capacity);
                return;
            }
            if (body_budget_ != nullptr && !body_budget_->try_reserve(event.body_.size())) {
                reject_body(stream, rejection_type::worker_body_budget_exhausted);
                return;
            }
            try {
                stream.request_->append_body(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.body_.data()), event.body_.size()));
            } catch (const inbound_buffer_limit_error&) {
                if (body_budget_ != nullptr) {
                    body_budget_->release(event.body_.size());
                }
                reject_body(stream, rejection_type::worker_body_budget_exhausted);
                return;
            } catch (...) {
                if (body_budget_ != nullptr) {
                    body_budget_->release(event.body_.size());
                }
                throw;
            }
            buffered_bytes_in_flight_ += event.body_.size();
            return;
        }
        case http3_connection_event_kind::trailer_field:
            if (stream.trailers_.append(event.trailer_.name_, event.trailer_.value_).index() != 0) {
                throw std::logic_error("HTTP/3 decoder published invalid request trailers");
            }
            return;
        case http3_connection_event_kind::tunnel_data: {
            if (!stream.connect_request_ || stream.tunnel_input_overflow_ ||
                stream.tunnel_receive_ended_) {
                return;
            }
            const auto queued = stream.tunnel_buffered_bytes_;
            if (queued > limits_.max_tunnel_buffered_bytes_ ||
                event.body_.size() > limits_.max_tunnel_buffered_bytes_ - queued ||
                buffered_bytes_in_flight_ > limits_.max_buffered_bytes_in_flight_ ||
                tunnel_bytes_in_flight_ >
                    limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ ||
                event.body_.size() > limits_.max_buffered_bytes_in_flight_ -
                                         buffered_bytes_in_flight_ - tunnel_bytes_in_flight_) {
                stream.tunnel_input_overflow_ = true;
                return;
            }
            if (body_budget_ != nullptr && !body_budget_->try_reserve(event.body_.size())) {
                stream.tunnel_input_overflow_ = true;
                return;
            }
            try {
                if (stream.tunnel_input_read_offset_ != 0 &&
                    stream.tunnel_input_.capacity() - stream.tunnel_input_.size() < event.body_.size()) {
                    stream.tunnel_input_.erase(0, stream.tunnel_input_read_offset_);
                    stream.tunnel_input_read_offset_ = 0;
                }
                stream.tunnel_input_.append(event.body_.data(), event.body_.size());
            } catch (...) {
                if (body_budget_ != nullptr) {
                    body_budget_->release(event.body_.size());
                }
                stream.tunnel_input_overflow_ = true;
                return;
            }
            stream.tunnel_buffered_bytes_ += event.body_.size();
            tunnel_bytes_in_flight_ += event.body_.size();
            return;
        }
        case http3_connection_event_kind::message_end:
            stream.receive_ended_ = true;
            if (stream.connect_request_ || stream.streaming_body_) {
                stream.tunnel_receive_ended_ = true;
                if (stream.streaming_body_) {
                    stream.request_->finish_body();
                }
            } else {
                stream.pending_finish_ = stream.state_ == stream_state_type::receiving;
            }
            return;
        case http3_connection_event_kind::push_stream:
        case http3_connection_event_kind::push_promise:
        case http3_connection_event_kind::push_canceled:
        case http3_connection_event_kind::origin_advertisement:
        case http3_connection_event_kind::priority_update:
        case http3_connection_event_kind::informational_head:
        case http3_connection_event_kind::final_head:
        case http3_connection_event_kind::request_head:
        case http3_connection_event_kind::reset:
            return;
    }
}

const http3_server_request* http3_sans_io_session_engine::request(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() || !found->second->request_.has_value()
               ? nullptr
               : &*found->second->request_;
}

const route_resolution* http3_sans_io_session_engine::resolution(
    std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? nullptr : &found->second->resolution_;
}

http3_sans_io_session_engine::stream_state_type http3_sans_io_session_engine::stream_state(
    std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? stream_state_type::rejected : found->second->state_;
}

http3_sans_io_session_engine::rejection_type http3_sans_io_session_engine::rejection(
    std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? rejection_type::none : found->second->rejection_;
}

const http_request_trailers* http3_sans_io_session_engine::request_trailers(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? nullptr : &found->second->trailers_;
}
const std::optional<http_priority>* http3_sans_io_session_engine::request_priority_update(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? nullptr : &found->second->priority_update_;
}

bool http3_sans_io_session_engine::streaming_request(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found != streams_.end() && found->second->streaming_body_;
}

bool http3_sans_io_session_engine::can_accept_input(std::uint64_t stream_id, std::size_t wire_bytes) const noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end() || !found->second->streaming_body_ || found->second->retired_ || found->second->body_failure_ != rejection_type::none) {
        return true;
    }
    return found->second->tunnel_buffered_bytes_ <= limits_.max_stream_backlog_bytes_ &&
           buffered_bytes_in_flight_ <= limits_.max_buffered_bytes_in_flight_ &&
           tunnel_bytes_in_flight_ <= limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ &&
           wire_bytes <= limits_.max_stream_backlog_bytes_ - found->second->tunnel_buffered_bytes_ &&
           wire_bytes <= limits_.max_buffered_bytes_in_flight_ - buffered_bytes_in_flight_ - tunnel_bytes_in_flight_ &&
           (body_budget_ == nullptr || wire_bytes <= body_budget_->available());
}

http3_sans_io_session_engine::rejection_type http3_sans_io_session_engine::streaming_body_failure(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() ? rejection_type::none : found->second->body_failure_;
}

http3_sans_io_session_engine::tunnel_read_result_type http3_sans_io_session_engine::read_tunnel_data(
    std::uint64_t stream_id, std::span<char> output) noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        return {.reset_ = true};
    }
    auto& stream = *found->second;
    const auto available = stream.tunnel_input_.size() - stream.tunnel_input_read_offset_;
    const auto count = (std::min)(available, output.size());
    if (count != 0) {
        std::memcpy(output.data(), stream.tunnel_input_.data() + stream.tunnel_input_read_offset_, count);
        stream.tunnel_input_read_offset_ += count;
        if (count > stream.tunnel_buffered_bytes_ || count > tunnel_bytes_in_flight_) {
            std::terminate();
        }
        stream.tunnel_buffered_bytes_ -= count;
        tunnel_bytes_in_flight_ -= count;
        if (body_budget_ != nullptr) {
            body_budget_->release(count);
        }
        if (stream.tunnel_input_read_offset_ == stream.tunnel_input_.size()) {
            std::pmr::string(stream.tunnel_input_.get_allocator()).swap(stream.tunnel_input_);
            stream.tunnel_input_read_offset_ = 0;
        }
    }
    return {.bytes_ = count,
        .ended_ = stream.tunnel_receive_ended_,
        .reset_ = stream.tunnel_reset_,
        .overflow_ = stream.tunnel_input_overflow_};
}

http3_datagram_receive_status http3_sans_io_session_engine::receive_datagram(http3_datagram_view datagram) {
    auto found = streams_.find(datagram.stream_id_);
    auto* stream = found == streams_.end() ? nullptr : found->second.get();
    const auto* resolved = stream ? stream->resolution_.resolved() : nullptr;
    const auto* tunnel = resolved ? resolved->route().endpoint().tunnel() : nullptr;
    const auto status = plan_http3_datagram_receive(datagram, {.local_h3_datagram_ = limits_.connection_.enable_datagrams_,
                                                                  .stream_exists_ = stream && stream->request_.has_value(),
                                                                  .receive_open_ = stream && !stream->retired_ && !stream->receive_ended_ && !stream->tunnel_receive_ended_ && !stream->tunnel_reset_,
                                                                  .supports_datagrams_ = tunnel && tunnel->config().datagrams_});
    if (status == http3_datagram_receive_status::deliver && (!stream->datagrams_ || stream->datagrams_->size() < 16)) {
        // Store the decoded opaque value. Quarter Stream ID is already validated
        // by the network/connection owner and is reconstructed for the adapter.
        std::array<char, 8> prefix{};
        auto size = encode_http3_datagram_prefix(prefix, datagram.stream_id_);
        if ((size.index() != 0)) {
            throw std::runtime_error("invalid HTTP Datagram stream ID");
        }
        std::pmr::string bytes_value(prefix.data(), std::get<0>(size), worker_.resource());
        bytes_value.append(datagram.payload_.data(), datagram.payload_.size());
        if (!stream->datagrams_) {
            stream->datagrams_.emplace(worker_.resource());
        }
        stream->datagrams_->push_back(std::move(bytes_value));
    }
    return status;
}
std::optional<std::pmr::string> http3_sans_io_session_engine::take_datagram(std::uint64_t stream_id) {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end() || (!found->second->datagrams_ || found->second->datagrams_->empty())) {
        return std::nullopt;
    }
    auto bytes_value = std::move(found->second->datagrams_->front());
    found->second->datagrams_->pop_front();
    return bytes_value;
}
http_datagram_session_config http3_sans_io_session_engine::datagram_config(std::uint64_t stream_id) const {
    return {.http3_stream_id_ = stream_id, .local_h3_datagram_ = limits_.connection_.enable_datagrams_, .peer_h3_datagram_ = connection_.peer_settings() && connection_.peer_settings()->h3_datagram_, .quic_datagram_ = limits_.max_quic_datagram_payload_bytes_ != 0, .max_quic_payload_bytes_ = limits_.max_quic_datagram_payload_bytes_};
}
bool http3_sans_io_session_engine::tunnel_input_overflowed(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found != streams_.end() && found->second->tunnel_input_overflow_;
}

bool http3_sans_io_session_engine::tunnel_receive_ended(std::uint64_t stream_id) const noexcept {
    const auto found = streams_.find(stream_id);
    return found == streams_.end() || found->second->tunnel_receive_ended_ ||
           found->second->tunnel_reset_;
}

std::optional<std::uint64_t>
http3_sans_io_session_engine::peer_max_field_section_size() const noexcept {
    const auto& settings = connection_.peer_settings();
    return settings.has_value() ? settings->max_field_section_size_ : std::nullopt;
}

std::size_t http3_sans_io_session_engine::active_stream_count() const noexcept {
    return streams_.size();
}

bool http3_sans_io_session_engine::terminated() const noexcept {
    return terminated_;
}

void http3_sans_io_session_engine::release_stream(std::uint64_t stream_id) noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        return;
    }
    if (found->second->leased_) {
        found->second->retired_ = true;
        return;
    }
    if (found->second->request_) {
        const auto bytes_value = found->second->request_->body_bytes();
        if (bytes_value > buffered_bytes_in_flight_) {
            std::terminate();
        }
        buffered_bytes_in_flight_ -= bytes_value;
        if (body_budget_ != nullptr) {
            body_budget_->release(bytes_value);
        }
    }
    if (found->second->tunnel_buffered_bytes_ > tunnel_bytes_in_flight_) {
        std::terminate();
    }
    tunnel_bytes_in_flight_ -= found->second->tunnel_buffered_bytes_;
    if (body_budget_ != nullptr) {
        body_budget_->release(found->second->tunnel_buffered_bytes_);
    }
    streams_.erase(found);
}

void http3_sans_io_session_engine::reject_body(stream_type& stream, rejection_type reason) noexcept {
    const auto bytes_value = stream.request_->body_bytes();
    if (bytes_value > buffered_bytes_in_flight_) {
        std::terminate();
    }
    buffered_bytes_in_flight_ -= bytes_value;
    if (body_budget_ != nullptr) {
        body_budget_->release(bytes_value);
    }
    stream.request_->abort_body();
    stream.state_ = stream_state_type::rejected;
    stream.rejection_ = reason;
}

bool http3_sans_io_session_engine::release(std::uint64_t stream_id) noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end() || !found->second->receive_ended_ || found->second->leased_) {
        return false;
    }
    release_stream(stream_id);
    return true;
}

bool http3_sans_io_session_engine::cancel_request(std::uint64_t stream_id) noexcept {
    if (terminated_) {
        return false;
    }
    const auto found = streams_.find(stream_id);
    if (found != streams_.end() && found->second->retired_) {
        return false;
    }
    const bool parser_retired = connection_.retire_server_request(stream_id);
    if (!parser_retired && (found == streams_.end() || !found->second->receive_ended_)) {
        return false;
    }
    release_stream(stream_id);
    return true;
}

void http3_sans_io_session_engine::stop() noexcept {
    terminate({http3_connection_status::connection_error,
        http3_connection_error_scope::connection, http3_connection_error_code::internal_error});
}

void http3_sans_io_session_engine::terminate(http3_connection_result failure) noexcept {
    if (terminated_) {
        return;
    }
    failure_ = failure;
    terminated_ = true;
    std::pmr::string(worker_.resource()).swap(control_output_);
    if (!connection_.retire()) {
        std::terminate();
    }
    for (auto iterator = streams_.begin(); iterator != streams_.end();) {
        const auto id = iterator->first;
        ++iterator;
        release_stream(id);
    }
}

}  // namespace ruvia::detail
