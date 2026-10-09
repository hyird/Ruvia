#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response_stream.h"

#include "client/http_client_access.h"
#include "http2/http2_connection.h"
#include "http2/http2_connection_owner_endpoint.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_builder.h"
#include "http2/http2_websocket_handshake.h"
#include "http_header_access.h"
#include "request/http_request_access.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {

static std::variant<http_request, http_protocol_error> build_http2_server_request(
    detail::http2_connection& connection, std::uint32_t stream_id,
    std::pmr::memory_resource* resource, std::string_view body) {
    auto* stream = connection.stream(stream_id);
    if (stream == nullptr) {
        return http_protocol_error(
            http_status::bad_request, "missing HTTP/2 request stream");
    }
    auto request = detail::http_request_access::make();
    auto result_value = detail::http2_request_builder::build(*stream, request, resource, body);
    if (const auto* failure = result_value.failure()) {
        return failure->protocol_error();
    }
    return request;
}

static websocket_handshake_validation_result validate_server_websocket_handshake(
    detail::http2_connection& connection, std::uint32_t stream_id,
    const http_request& request) noexcept {
    const auto* stream = connection.stream(stream_id);
    if (stream == nullptr) {
        return detail::websocket_handshake_validation_result_access::invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request);
}

http2_request_head_submit_result http2_connection::pin_submitted_request(
    detail::http2_connection& connection, const http2_request_head_submit_result& result_value) {
    if (const auto* submitted = result_value.submitted()) {
        try {
            connection.pin_stream(submitted->stream_id());
        } catch (...) {
            const auto original = std::current_exception();
            try {
                (void)connection.submit_reset(
                    submitted->stream_id(), detail::http2_error_code::cancel);
                // Preserve the original pinning failure; this reset is rollback only.
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
            std::rethrow_exception(original);
        }
        return result_value;
    }
    const auto error = result_value.failure()->error();
    if (error == http2_request_head_submit_error::connection_not_started) {
        std::terminate();
    }
    return result_value;
}

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(
    const detail::http2_stream_state& stream, std::pmr::memory_resource* resource) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value =
        detail::http_client_response_head_access::make(*status, http_protocol_version::http2, resource);
    auto& headers = detail::http_client_response_head_access::headers(result_value);
    headers.reserve(stream.remote_header_count());
    for (std::size_t index = 0; index < stream.remote_header_count(); ++index) {
        const auto field = stream.remote_header_at(index);
        headers.push_back(
            detail::http_header_access::make(field.name_, field.value_, resource));
    }
    return result_value;
}

[[nodiscard]] http2_message_content_semantics message_content_semantics(
    const detail::http2_stream_state& stream) noexcept {
    const auto& content = stream.remote_content();
    return content.metadata_only_without_length() != nullptr ||
                   content.metadata_only_known_length() != nullptr
               ? http2_message_content_semantics::metadata_only
               : http2_message_content_semantics::content;
}

}  // namespace

class http2_connection::impl_type final {
public:
    struct storage_type final {
        storage_type(std::pmr::memory_resource* requested, http2_role public_role, bool enable_push, bool receive_origin_advertisements)
            : resource_(detail::http_pmr_resource_or_default(requested)),
              role_(public_role),
              connection_(resource_, public_role, enable_push, receive_origin_advertisements) {}

        std::pmr::memory_resource* resource_;
        http2_role role_;
        detail::http2_connection connection_;
    };

    impl_type(std::pmr::memory_resource* requested, http2_role public_role, bool enable_push, bool receive_origin_advertisements)
        : storage_(detail::make_http_pmr_object<storage_type>(requested, requested, public_role, enable_push, receive_origin_advertisements)),
          resource_(storage_->resource_),
          role_(public_role),
          connection_(storage_->connection_),
          endpoint_(detail::construct_http_pmr_object<detail::http2_connection_owner_endpoint>(resource_,
              this, &impl_type::abandon_request_thunk, &impl_type::abandon_credit_thunk, resource_)) {}

    ~impl_type() {
        endpoint_->detach();
        endpoint_->retain_storage(storage_.release(), &impl_type::destroy_storage);
        endpoint_->release();
    }

    impl_type(const impl_type&) = delete;
    impl_type& operator=(const impl_type&) = delete;

    static void destroy_storage(void* raw) noexcept {
        auto* storage = static_cast<storage_type*>(raw);
        auto* resource = storage->resource_;
        detail::destroy_http_pmr_object(storage, resource);
    }

    enum class deferred_release_kind_type : std::uint8_t { after_credits,
        after_tunnel_end,
        abandon };
    struct deferred_release_type final {
        std::uint32_t stream_id_{0};
        deferred_release_kind_type kind_{deferred_release_kind_type::after_credits};
    };
    struct deferred_credit_type final {
        std::uint32_t stream_id_{0};
        std::uint32_t bytes_{0};
    };

    static void abandon_request_thunk(void* target, std::uint32_t stream_id) noexcept {
        static_cast<impl_type*>(target)->abandon_request(stream_id);
    }
    static void abandon_credit_thunk(
        void* target, std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        static_cast<impl_type*>(target)->abandon_credit(stream_id, bytes_value);
    }

    [[nodiscard]] deferred_release_type* deferred(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < deferred_release_count_; ++i) {
            if (deferred_releases_[i].stream_id_ == stream_id) {
                return &deferred_releases_[i];
            }
        }
        return nullptr;
    }

    void defer(std::uint32_t stream_id, deferred_release_kind_type kind) noexcept {
        if (auto* existing = deferred(stream_id)) {
            if (kind == deferred_release_kind_type::after_tunnel_end || kind == deferred_release_kind_type::abandon ||
                (existing->kind_ == deferred_release_kind_type::abandon &&
                    kind == deferred_release_kind_type::after_credits)) {
                existing->kind_ = kind;
            }
            return;
        }
        if (deferred_release_count_ == deferred_releases_.size()) {
            std::terminate();
        }
        deferred_releases_[deferred_release_count_++] = deferred_release_type{stream_id, kind};
    }

    void erase_deferred(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < deferred_release_count_; ++i) {
            if (deferred_releases_[i].stream_id_ != stream_id) {
                continue;
            }
            deferred_releases_[i] = deferred_releases_[--deferred_release_count_];
            return;
        }
    }

    [[nodiscard]] deferred_credit_type* deferred_credit(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < deferred_credit_count_; ++i) {
            if (deferred_credits_[i].stream_id_ == stream_id) {
                return &deferred_credits_[i];
            }
        }
        return nullptr;
    }

    void defer_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        if (auto* existing = deferred_credit(stream_id)) {
            if (bytes_value > (std::numeric_limits<std::uint32_t>::max)() - existing->bytes_) {
                std::terminate();
            }
            existing->bytes_ += bytes_value;
            return;
        }
        if (deferred_credit_count_ == deferred_credits_.size()) {
            std::terminate();
        }
        deferred_credits_[deferred_credit_count_++] = deferred_credit_type{stream_id, bytes_value};
    }

    void erase_deferred_credit(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < deferred_credit_count_; ++i) {
            if (deferred_credits_[i].stream_id_ != stream_id) {
                continue;
            }
            deferred_credits_[i] = deferred_credits_[--deferred_credit_count_];
            return;
        }
    }

    void release_owner_after_credits(std::uint32_t stream_id) {
        auto* stream = connection_.stream(stream_id);
        if ((stream != nullptr && stream->window_debt() != 0) ||
            connection_.has_pending_events(stream_id)) {
            defer(stream_id, deferred_release_kind_type::after_credits);
            return;
        }
        connection_.unpin_stream(stream_id);
        erase_deferred(stream_id);
    }

    void abandon_request_checked(std::uint32_t stream_id) {
        auto* stream = connection_.stream(stream_id);
        if (stream == nullptr) {
            erase_deferred(stream_id);
            return;
        }
        if (!stream->is_aborted()) {
            (void)connection_.submit_reset(stream_id, detail::http2_error_code::cancel);
        }
        release_owner_after_credits(stream_id);
    }

    void retry_deferred_release(std::uint32_t stream_id) noexcept {
        const auto* pending = deferred(stream_id);
        if (pending == nullptr) {
            return;
        }
        if (pending->kind_ == deferred_release_kind_type::abandon) {
            try {
                abandon_request_checked(stream_id);
                // The deferred entry remains queued for the next noexcept retry.
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
            return;
        }
        if (pending->kind_ == deferred_release_kind_type::after_credits || pending->kind_ == deferred_release_kind_type::after_tunnel_end) {
            const auto* stream = connection_.stream(stream_id);
            if ((stream != nullptr && (stream->window_debt() != 0 ||
                                          (pending->kind_ == deferred_release_kind_type::after_tunnel_end && !detail::http2_stream_is_closed(*stream)))) ||
                connection_.has_pending_events(stream_id)) {
                return;
            }
        }
        try {
            connection_.unpin_stream(stream_id);
            erase_deferred(stream_id);
            // The deferred entry remains queued for the next noexcept retry.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
    }

    void retry_deferred_credits() noexcept {
        std::size_t index = 0;
        while (index < deferred_credit_count_) {
            const auto pending = deferred_credits_[index];
            try {
                (void)connection_.release_received_data(pending.stream_id_, pending.bytes_);
                erase_deferred_credit(pending.stream_id_);
                retry_deferred_release(pending.stream_id_);
            } catch (...) {
                ++index;
            }
        }
    }

    void retry_deferred() noexcept {
        retry_deferred_credits();
        std::size_t index = 0;
        while (index < deferred_release_count_) {
            const auto stream_id = deferred_releases_[index].stream_id_;
            retry_deferred_release(stream_id);
            if (index < deferred_release_count_ && deferred_releases_[index].stream_id_ == stream_id) {
                ++index;
            }
        }
    }

    void abandon_request(std::uint32_t stream_id) noexcept {
        try {
            abandon_request_checked(stream_id);
        } catch (...) {
            defer(stream_id, deferred_release_kind_type::abandon);
        }
    }

    void abandon_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        try {
            (void)connection_.release_received_data(stream_id, bytes_value);
            retry_deferred_release(stream_id);
        } catch (...) {
            defer_credit(stream_id, bytes_value);
        }
    }

    std::unique_ptr<storage_type, detail::http_pmr_object_deleter<storage_type>> storage_;
    std::pmr::memory_resource* resource_;
    http2_role role_;
    detail::http2_connection& connection_;
    detail::http2_connection_owner_endpoint* endpoint_;
    std::array<deferred_release_type, detail::http2_local_settings::max_concurrent_streams>
        deferred_releases_{};
    std::size_t deferred_release_count_{0};
    std::array<deferred_credit_type, detail::http2_local_settings::max_concurrent_streams> deferred_credits_{};
    std::size_t deferred_credit_count_{0};
};

http2_received_data_credit::http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, std::uint32_t bytes_value) noexcept
    : endpoint_(endpoint),
      stream_id_(stream_id),
      bytes_(bytes_value) {
    endpoint_->retain();
}

http2_received_data_credit::~http2_received_data_credit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_credit(stream_id_, bytes_);
    endpoint_->release();
}

http2_received_data_credit::http2_received_data_credit(http2_received_data_credit&& other) noexcept
    : endpoint_(std::exchange(other.endpoint_, nullptr)),
      stream_id_(std::exchange(other.stream_id_, 0)),
      bytes_(std::exchange(other.bytes_, 0)) {}

http2_received_data_credit_merge_status http2_received_data_credit::merge(
    http2_received_data_credit&& other) noexcept {
    if (this == &other || !valid() || !other.valid()) {
        return http2_received_data_credit_merge_status::invalid_credit;
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        return http2_received_data_credit_merge_status::different_stream;
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        return http2_received_data_credit_merge_status::overflow;
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
    return http2_received_data_credit_merge_status::merged;
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, http_request request, http_request_expectations expectations,
    http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept
    : stream_id_(stream_id),
      request_(std::move(request)),
      snapshot_(snapshot),
      expectations_(expectations),
      content_(content),
      endpoint_(endpoint) {
    endpoint_->retain();
}

http2_request_head_event::~http2_request_head_event() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_request(stream_id_);
    endpoint_->release();
}

http2_request_head_event::http2_request_head_event(http2_request_head_event&& other) noexcept
    : stream_id_(std::exchange(other.stream_id_, 0)),
      request_(std::move(other.request_)),
      snapshot_(other.snapshot_),
      expectations_(other.expectations_),
      content_(other.content_),
      endpoint_(std::exchange(other.endpoint_, nullptr)) {}

void http2_connection::impl_deleter_type::operator()(impl_type* value) const noexcept {
    if (value == nullptr) {
        return;
    }
    auto* resource = value->resource_;
    detail::destroy_http_pmr_object(value, resource);
}

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements) {
    auto* resolved = detail::http_pmr_resource_or_default(resource);
    impl_.reset(detail::construct_http_pmr_object<impl_type>(resolved, resolved, role, enable_push, receive_origin_advertisements));
    impl_->connection_.begin_connection();
}
http2_connection http2_connection::server(http2_connection_options options) {
    return http2_connection(options.resource_, http2_role::server, false, false);
}
http2_connection http2_connection::client(http2_connection_options options) {
    return http2_connection(options.resource_, http2_role::client, options.enable_push_, options.receive_origin_advertisements_);
}
http2_connection::~http2_connection() = default;
http2_connection::http2_connection(http2_connection&&) noexcept = default;
http2_connection& http2_connection::operator=(http2_connection&&) noexcept = default;

std::variant<http_request, http_protocol_error> make_http2_server_request(
    http2_connection& connection, std::uint32_t stream_id,
    std::pmr::memory_resource* resource, std::string_view body) {
    return build_http2_server_request(connection.impl_->connection_, stream_id, resource, body);
}

websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request) noexcept {
    return validate_server_websocket_handshake(connection.impl_->connection_, stream_id, request);
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation) {
    auto result_value = impl_->connection_.submit_websocket_handshake(stream_id, request, validation);
    if (const auto* negotiation = result_value.submitted()) {
        return http2_websocket_handshake_submit_result(http2_websocket_negotiation(
            negotiation->subprotocol(), negotiation->compression()));
    }
    const auto error = result_value.failure()->error();
    return http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure(
        error == detail::http2_websocket_handshake_submit_error::closed
            ? http2_websocket_handshake_submit_error::closed
            : http2_websocket_handshake_submit_error::invalid_state));
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation,
    http2_websocket_server_handshake_options options) {
    auto negotiation = detail::make_websocket_server_negotiation(request, {
                                                                              .supported_subprotocols_ = options.supported_subprotocols_,
                                                                              .response_headers_ = options.response_headers_,
                                                                              .resource_ = impl_->resource_,
                                                                          });
    auto result_value = impl_->connection_.submit_websocket_handshake(
        stream_id, validation, std::move(negotiation));
    if (const auto* submitted = result_value.submitted()) {
        return http2_websocket_handshake_submit_result(http2_websocket_negotiation(
            submitted->subprotocol(), submitted->compression()));
    }
    const auto error = result_value.failure()->error();
    return http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure(
        error == detail::http2_websocket_handshake_submit_error::closed
            ? http2_websocket_handshake_submit_error::closed
            : http2_websocket_handshake_submit_error::invalid_state));
}

http2_role http2_connection::role() const noexcept {
    return impl_->role_;
}
bool http2_connection::header_block_in_progress() const noexcept {
    return impl_->connection_.header_block_in_progress();
}
bool http2_connection::received_peer_settings() const noexcept {
    return impl_->connection_.received_peer_settings();
}
http2_feed_result http2_connection::feed(std::string_view input) {
    impl_->retry_deferred();
    const auto result_value = impl_->connection_.feed(input);
    if (result_value == http2_feed_result::connection_not_started) {
        std::terminate();
    }
    return result_value;
}

std::optional<http2_event> http2_connection::next_event() {
    impl_->retry_deferred();
    auto* event = impl_->connection_.peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    if (auto* value = event->origin_advertisement()) {
        auto result_value = http2_event(std::move(*value));
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (auto* value = event->alternative_service_advertisement()) {
        auto result_value = http2_event(std::move(*value));
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->priority_update()) {
        auto result_value = http2_event(*value);
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (auto* value = event->push_promise()) {
        impl_->connection_.pin_stream(value->promised_stream_id_);
        auto result_value = http2_event(std::move(*value));
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (auto* value = event->informational_head()) {
        const auto stream_id = value->stream_id();
        const auto signal = value->request_content_signal();
        auto result_value = http2_event::informational_head(stream_id, std::move(*value).take_head(), signal);
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->message_head()) {
        if (impl_->role_ == http2_role::server) {
            auto* stream = impl_->connection_.stream(value->stream_id());
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 request event has no stream");
            }
            auto request = detail::http_request_access::make();
            const auto build =
                detail::http2_request_builder::build(*stream, request, impl_->resource_, {});
            if (build.built() == nullptr) {
                (void)impl_->connection_.submit_reset(
                    value->stream_id(), detail::http2_error_code::cancel);
                impl_->connection_.consume_event();
                throw std::logic_error("validated HTTP/2 request cannot be materialized");
            }
            impl_->connection_.pin_stream(value->stream_id());
            const auto content = stream->request_content_indication();
            const auto& remote = stream->remote_receive();
            const bool connect_pending = remote.connect_pending() != nullptr ||
                                         remote.connect_pending_end_stream() != nullptr;
            const http2_server_request_snapshot snapshot{
                .content_ = content,
                .body_open_ = content == http_request_content_indication::will_follow &&
                              !connect_pending,
                .connect_pending_ = connect_pending,
            };
            auto result_value = http2_event::request_head(impl_->endpoint_, value->stream_id(),
                std::move(request), stream->request_expectations(), content, snapshot);
            impl_->connection_.consume_event();
            return std::optional<http2_event>(std::move(result_value));
        }
        auto* stream = impl_->connection_.stream(value->stream_id());
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 response event has no stream");
        }
        auto head = response_head_from_stream(*stream, impl_->resource_);
        auto result_value = http2_event::response_head(
            value->stream_id(), std::move(head), value->request_content_signal());
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->message_body_chunk()) {
        auto result_value = http2_event::message_body_chunk(
            impl_->endpoint_, value->stream_id(), value->bytes(), value->flow_control_bytes());
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->message_end()) {
        auto* stream = impl_->connection_.stream(value->stream_id());
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 message end event has no stream");
        }
        auto trailers = stream->take_remote_trailers();
        auto result_value = http2_event::message_end(
            value->stream_id(), std::move(trailers), message_content_semantics(*stream));
        if (impl_->role_ == http2_role::client) {
            impl_->release_owner_after_credits(value->stream_id());
        }
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->tunnel_data()) {
        auto result_value = http2_event::tunnel_data(
            impl_->endpoint_, value->stream_id(), value->bytes(), value->flow_control_bytes());
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->tunnel_end()) {
        auto result_value = http2_event::tunnel_end(value->stream_id());
        if (impl_->role_ == http2_role::client) {
            impl_->defer(value->stream_id(), impl_type::deferred_release_kind_type::after_tunnel_end);
        }
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->stream_closed()) {
        auto result_value =
            http2_event::stream_closed(value->stream_id(), value->source(), value->error());
        if (impl_->role_ == http2_role::client) {
            impl_->release_owner_after_credits(value->stream_id());
        }
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    if (const auto* value = event->request_unprocessed()) {
        auto result_value = http2_event::request_unprocessed(value->stream_id());
        if (impl_->role_ == http2_role::client) {
            impl_->release_owner_after_credits(value->stream_id());
        }
        impl_->connection_.consume_event();
        return std::optional<http2_event>(std::move(result_value));
    }
    const auto* value = event->goaway();
    auto result_value = http2_event::goaway(value->last_stream_id(), value->error());
    impl_->connection_.consume_event();
    return std::optional<http2_event>(std::move(result_value));
}

std::string_view http2_connection::pending_output() const& noexcept {
    impl_->retry_deferred();
    return impl_->connection_.pending_output();
}
http2_output_consume_status http2_connection::consume_output(std::size_t bytes_value) noexcept {
    impl_->retry_deferred();
    return impl_->connection_.consume_output(bytes_value);
}
void http2_connection::take_output(std::pmr::string& output) {
    impl_->retry_deferred();
    impl_->connection_.take_output(output);
}
http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::pmr::string& output, http2_data_output_observer_type observer, void* observer_context) {
    impl_->retry_deferred();
    return impl_->connection_.take_output_batch(max_bytes, output, observer, observer_context);
}
bool http2_connection::wants_write() const noexcept {
    impl_->retry_deferred();
    return impl_->connection_.wants_write();
}

http2_request_head_submit_result http2_connection::submit_request_head(
    const http2_regular_request_head_view& request) {
    std::optional<std::string_view> authority;
    if (request.authority_) {
        authority = request.authority_->view();
    }
    const auto result_value = impl_->connection_.submit_regular_request_head(request.method_.view(),
        request.scheme_.view(), authority, request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_,
        request.expectation_);
    return pin_submitted_request(impl_->connection_, result_value);
}

http2_request_head_submit_result http2_connection::submit_request_head(
    const http2_connect_request_head_view& request) {
    const auto result_value = impl_->connection_.submit_connect_request_head(
        request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_));
    return pin_submitted_request(impl_->connection_, result_value);
}

http2_request_head_submit_result http2_connection::submit_request_head(
    const http2_extended_connect_request_head_view& request) {
    const auto result_value = impl_->connection_.submit_extended_connect_request_head(request.protocol_.view(),
        request.scheme_.view(), request.authority_.view(), request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_));
    return pin_submitted_request(impl_->connection_, result_value);
}

http2_data_submit_status http2_connection::submit_data(
    std::uint32_t stream_id, std::string_view bytes_value, http2_end_stream end_stream) {
    const auto result_value = impl_->connection_.submit_data(stream_id, bytes_value, end_stream);
    impl_->retry_deferred();
    return result_value;
}
http2_request_content_release_status http2_connection::release_request_content(
    std::uint32_t stream_id) noexcept {
    return impl_->connection_.release_request_content(stream_id);
}
http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_connect_response_head(stream_id, response);
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    return impl_->connection_.submit_interim_response_head(stream_id, response);
}

http2_response_head_submit_result http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response,
    http_buffered_response_write_plan write_plan) {
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

http2_submit_status http2_connection::submit_buffered_response(
    std::uint32_t stream_id, const http_response& response) {
    auto* stream = impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return http2_submit_status::closed;
    }
    const auto& body = detail::response_body(response);
    if (body.file().has_value()) {
        return http2_submit_status::invalid_message;
    }
    const auto plan = plan_buffered_http_response_write(stream->request_known_method(), response);
    const auto result_value = impl_->connection_.submit_response_head(stream_id, response, plan);
    if (const auto* failure = result_value.failure()) {
        switch (failure->error()) {
            case http2_response_head_submit_error::peer_stream_limit_reached:
                return http2_submit_status::peer_capability_unavailable;
            case http2_response_head_submit_error::closed:
                return http2_submit_status::closed;
            case http2_response_head_submit_error::invalid_state:
            case http2_response_head_submit_error::response_plan_mismatch:
                return http2_submit_status::invalid_state;
            case http2_response_head_submit_error::invalid_message:
                return http2_submit_status::invalid_message;
        }
    }
    if (plan.send_body()) {
        const auto data = impl_->connection_.submit_data(
            stream_id, body.bytes(), detail::http2_end_stream::end_stream);
        if (data != detail::http2_data_submit_status::accepted &&
            data != detail::http2_data_submit_status::queued) {
            return http2_submit_status::invalid_state;
        }
    }
    return http2_submit_status::accepted;
}

http2_streaming_response_head_submit_result http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response, http_response_stream_kind kind,
    http_response_trailer_intent trailer_intent) {
    return impl_->connection_.submit_streaming_response_head(
        stream_id, std::move(response), kind, trailer_intent);
}

http2_submit_status http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response) {
    const auto result_value = submit_streaming_response_head(stream_id, std::move(response),
        http_response_stream_kind::generic, http_response_trailer_intent::none);
    if (const auto* failure = result_value.failure()) {
        switch (failure->error()) {
            case http2_response_head_submit_error::peer_stream_limit_reached:
                return http2_submit_status::peer_capability_unavailable;
            case http2_response_head_submit_error::closed:
                return http2_submit_status::closed;
            case http2_response_head_submit_error::invalid_state:
            case http2_response_head_submit_error::response_plan_mismatch:
                return http2_submit_status::invalid_state;
            case http2_response_head_submit_error::invalid_message:
                return http2_submit_status::invalid_message;
        }
    }
    return http2_submit_status::accepted;
}

http2_finish_request_status http2_connection::finish_request(std::uint32_t stream_id,
    std::span<const http_header_view> trailers) {
    return impl_->connection_.finish_request(stream_id, trailers);
}

http2_finish_response_status http2_connection::finish_response(
    std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    switch (impl_->connection_.finish_response(stream_id, trailers)) {
        case detail::http2_finish_submit_status::accepted:
            return http2_finish_response_status::accepted;
        case detail::http2_finish_submit_status::queued:
            return http2_finish_response_status::queued;
        case detail::http2_finish_submit_status::closed:
            return http2_finish_response_status::closed;
        case detail::http2_finish_submit_status::invalid_state:
            return http2_finish_response_status::invalid_state;
        case detail::http2_finish_submit_status::content_length_incomplete:
            return http2_finish_response_status::content_length_incomplete;
    }
    std::terminate();
}

std::variant<std::uint32_t, http2_push_submit_error> http2_connection::submit_push_promise(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    return impl_->connection_.submit_push_promise(associated_stream_id, request);
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    const auto id = submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    try {
        auto built = build_http2_server_request(impl_->connection_, std::get<0>(id), impl_->resource_, {});
        if ((built.index() != 0)) {
            (void)impl_->connection_.submit_reset(std::get<0>(id), detail::http2_error_code::cancel);
            return http2_push_submit_error::invalid_request;
        }
        impl_->connection_.pin_stream(std::get<0>(id));
        return http2_request_head_event(impl_->endpoint_, std::get<0>(id), std::move(std::get<0>(built)), {},
            http_request_content_indication::no_content, {});
    } catch (...) {
        const auto original = std::current_exception();
        try {
            (void)impl_->connection_.submit_reset(std::get<0>(id), detail::http2_error_code::cancel);
        } catch (...) {
        }
        std::rethrow_exception(original);
    }
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    return impl_->connection_.submit_origin_advertisement(origins);
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view field_value) {
    return impl_->connection_.submit_alternative_service_advertisement(stream_id, origin, field_value);
}
http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    return impl_->connection_.submit_priority_update(stream_id, fields_value);
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    const auto status = impl_->connection_.submit_reset(stream_id, error);
    if (status == http2_submit_status::accepted && impl_->role_ == http2_role::client) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submit_request_head() here rather than waiting for next_event().
        impl_->release_owner_after_credits(stream_id);
    }
    return status;
}
http2_received_data_acknowledge_status http2_connection::acknowledge(http2_received_data_credit&& credit) {
    if (!credit.valid() || credit.endpoint_ != impl_->endpoint_) {
        return http2_received_data_acknowledge_status::invalid_credit;
    }
    const auto stream_id = credit.stream_id_;
    const auto bytes_value = credit.bytes_;
    const bool acknowledged = impl_->connection_.release_received_data(stream_id, bytes_value);
    auto* endpoint = std::exchange(credit.endpoint_, nullptr);
    credit.stream_id_ = 0;
    credit.bytes_ = 0;
    endpoint->release();
    if (acknowledged) {
        impl_->retry_deferred_release(stream_id);
    }
    return acknowledged ? http2_received_data_acknowledge_status::acknowledged
                        : http2_received_data_acknowledge_status::closed;
}
bool http2_connection::has_queued_data(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.has_queued_data(stream_id);
}

http2_data_queue_state http2_connection::data_queue_state(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.data_queue_state(stream_id);
}
std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.pending_data_output_bytes(stream_id);
}
std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.send_window_state(stream_id);
}
bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_aborted(stream_id);
}
http2_stream_receive_status http2_connection::stream_receive_status(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_receive_status(stream_id);
}
std::optional<http2_server_request_view> detail::http2_connection::server_request_view(
    std::uint32_t stream_id) const noexcept {
    if (role_ != http2_role::server) {
        return std::nullopt;
    }
    const auto* stream_state = streams_.find(stream_id);
    if (stream_state == nullptr || stream_state->request_method().empty()) {
        return std::nullopt;
    }
    return http2_server_request_view{
        .method_ = stream_state->request_method(),
        .path_ = stream_state->request_path(),
        .authority_ = stream_state->request_authority(),
        .protocol_ = stream_state->request_protocol(),
    };
}
std::optional<http2_server_request_view> http2_connection::server_request_view(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.server_request_view(stream_id);
}
std::span<const std::uint32_t> http2_connection::take_drained_data_streams() & noexcept {
    return impl_->connection_.take_drained_data_streams();
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 ||
        request.endpoint_ != impl_->endpoint_) {
        return http2_server_request_release_status::invalid_lease;
    }
    if (impl_->connection_.stream(request.stream_id_) == nullptr) {
        auto* endpoint = std::exchange(request.endpoint_, nullptr);
        request.stream_id_ = 0;
        detail::http_request_access::reset(request.request_);
        endpoint->release();
        return http2_server_request_release_status::closed;
    }
    impl_->release_owner_after_credits(request.stream_id_);
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    detail::http_request_access::reset(request.request_);
    endpoint->release();
    return http2_server_request_release_status::released;
}
void http2_connection::begin_drain() {
    impl_->connection_.begin_drain();
}
bool http2_connection::draining() const noexcept {
    return impl_->connection_.draining();
}
std::optional<http2_error_code> http2_connection::connection_error() const noexcept {
    return impl_->connection_.connection_error();
}

}  // namespace ruvia
