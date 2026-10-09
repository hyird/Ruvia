#include <cstdint>
#include <string_view>

#include "ruvia/http/http_request_content_semantics.h"

#include "http2/http2_connection.h"
#include "http2/http2_header_block.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_window_update.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool decode_initial_header_field(
    void* target, std::string_view name, std::string_view value) {
    return http2_on_decoded_initial_header(
        *static_cast<http2_header_decode_context*>(target), name, value);
}

}  // namespace

header_decode_status http2_connection::decode_header_block(http2_stream_state& stream,
    http2_stream_header_decode_transaction& stream_transaction,
    hpack_decoder::decode_transaction_type& hpack_transaction) {
    http2_header_decode_context context_value{stream, &stream_transaction};
    const auto result_value = decoder_.decode(
        stream.remote_header_block(), &context_value, decode_initial_header_field, hpack_transaction);
    if (const auto status = http2_classify_header_decode_result(result_value);
        status != header_decode_status::ok) {
        return status;
    }
    if (!stream.has_method()) {
        return header_decode_status::protocol_error;
    }
    if (stream.has_protocol()) {
        if (preface_phase_ != preface_phase_type::ready ||
            stream.request_known_method() != http_known_method::connect || !stream.has_scheme() ||
            !stream.has_path() || !stream.has_authority() ||
            !http2_is_valid_request_authority(stream.request_scheme(), stream.request_authority()) ||
            !http2_is_valid_extended_connect_path(stream.request_scheme(), stream.request_path()) ||
            stream.remote_content().allowed_known_length() != nullptr) {
            return header_decode_status::protocol_error;
        }
        // RFC 8441 defines websocket extended CONNECT only for HTTP(S) URI
        // schemes. Other extended protocols retain the full RFC 3986 space.
        if (stream.protocol_is_websocket() &&
            !http_ascii_equals_ignore_case(stream.request_scheme(), "http") &&
            !http_ascii_equals_ignore_case(stream.request_scheme(), "https")) {
            return header_decode_status::protocol_error;
        }
        if (!stream.begin_extended_connect()) {
            return header_decode_status::protocol_error;
        }
    } else if (stream.request_known_method() == http_known_method::connect) {
        request_target_view connect_target;
        if (!stream.has_authority() || stream.has_scheme() || stream.has_path() ||
            stream.remote_content().allowed_known_length() != nullptr ||
            !parse_request_target(
                http_known_method::connect, stream.request_authority(), connect_target)) {
            return header_decode_status::protocol_error;
        }
        if (!stream.begin_standard_connect()) {
            return header_decode_status::protocol_error;
        }
    } else if (!stream.has_scheme() || !stream.has_path() ||
               (stream.has_authority() && !http2_is_valid_request_authority(
                                              stream.request_scheme(), stream.request_authority())) ||
               !http2_is_valid_regular_request_path(
                   stream.request_known_method(), stream.request_scheme(), stream.request_path()) ||
               !http2_has_required_request_authority(stream)) {
        return header_decode_status::protocol_error;
    }
    if (role_ == http2_role::server && stream.request_known_method() != http_known_method::connect) {
        const auto content_semantics = http_request_content_semantics(stream.request_method());
        if (content_semantics == http_request_content_semantics::forbidden) {
            // A declared length is an explicit content signal, including zero.
            // Without a length, retain the open remote half for a legal empty
            // DATA(END_STREAM), but make non-empty DATA unrepresentable as content.
            if (stream.remote_content().allowed_known_length() != nullptr ||
                !stream.select_remote_content_metadata_only()) {
                return header_decode_status::protocol_error;
            }
        } else if (content_semantics == http_request_content_semantics::content_type_required) {
            // A declared length (including zero) or an open peer send half is
            // explicit OPTIONS content in the same cases modeled by the HTTP/2
            // request writer. RFC 9110 section 9.3.7 requires a valid media type.
            const bool explicit_content = stream.remote_content().allowed_known_length() != nullptr ||
                                          stream.remote_receive().head_pending() != nullptr;
            if (explicit_content && !stream.has_singleton_request_header(singleton_request_header_bit(
                                        request_header_kind::content_type))) {
                return header_decode_status::protocol_error;
            }
        }
    }
    const bool remote_head_finalized = stream.tunnel().pending() != nullptr
                                           ? stream.finalize_remote_connect_head()
                                           : stream.finalize_remote_content_head();
    if (!remote_head_finalized) {
        return header_decode_status::protocol_error;
    }
    if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
        return header_decode_status::protocol_error;
    }
    // NOTE (sans-I/O): resolve_stream_route is deliberately NOT called here -- route
    // resolution and body-mode selection are application policy the owner applies
    // after pulling the message_head event.
    return header_decode_status::ok;
}

header_decode_status http2_connection::decode_initial_header_block(http2_stream_state& stream,
    http2_stream_header_decode_transaction& stream_transaction,
    hpack_decoder::decode_transaction_type& hpack_transaction) {
    const auto status = role_ == http2_role::client
                            ? decode_response_header_block(stream, stream_transaction, hpack_transaction)
                            : decode_header_block(stream, stream_transaction, hpack_transaction);
    if (status == header_decode_status::ok && http2_remote_final_head_decoded(stream)) {
        emit_request_headers(stream);  // An interim client response does not publish a message head.
    }
    return status;
}

header_decode_status http2_connection::decode_refused_header_block(
    http2_stream_state& stream, hpack_decoder::decode_transaction_type& hpack_transaction) {
    http2_stream_header_decode_transaction transaction{stream, true};
    http2_header_decode_context context_value{stream, &transaction};
    const auto result_value = decoder_.decode(
        stream.remote_header_block(), &context_value, decode_initial_header_field, hpack_transaction);
    return http2_classify_header_decode_result(result_value);
}

header_decode_status http2_connection::decode_discarded_header_block(
    http2_stream_state& stream, hpack_decoder::decode_transaction_type& hpack_transaction) {
    // Even a block whose HTTP semantics are no longer observable must be decoded in
    // full because HPACK's dynamic table is connection-scoped. Keep the decompressed
    // field-list budget, but deliberately avoid mutating request/response state.
    http2_header_decode_context context_value{stream};
    const auto result_value = decoder_.decode(
        stream.remote_header_block(), &context_value,
        [](void* target, std::string_view name, std::string_view value) {
            return http2_accumulate_header_list_bytes(
                *static_cast<http2_header_decode_context*>(target), name, value);
        },
        hpack_transaction);
    return http2_classify_header_decode_result(result_value);
}

bool http2_connection::start_discarded_header_block(
    const http2_frame_header& header_value, std::string_view fragment, discarded_header_action_type action) {
    if (discarded_header_stream_) {
        append_goaway(http2_error_code::protocol_error, "overlapping discarded HEADERS block");
        return false;
    }
    try {
        discarded_header_stream_.emplace(header_value.stream_id_, resource_);
        discarded_header_action_ = action;
        if (!http2_start_header_block(*discarded_header_stream_, fragment)) {
            discarded_header_stream_.reset();
            discarded_header_action_ = discarded_header_action_type::ignore;
            append_goaway(http2_error_code::compression_error, "field block not decompressed");
            return false;
        }
        if ((header_value.flags_ & http2_flag_end_headers) != 0) {
            return finish_discarded_header_block();
        }
        header_continuation_.start(header_value.stream_id_, http2_header_block_kind::discarded);
        return true;
    } catch (...) {
        // The caller retries the complete HEADERS frame after a recoverable PMR
        // failure. Never leave an engaged scratch stream or an action latch behind:
        // either would make the retry look like an overlapping CONTINUATION run.
        discarded_header_stream_.reset();
        discarded_header_action_ = discarded_header_action_type::ignore;
        throw;
    }
}

bool http2_connection::finish_discarded_header_block() {
    if (!discarded_header_stream_) {
        append_goaway(http2_error_code::protocol_error, "missing discarded HEADERS state");
        return false;
    }
    const auto stream_id = discarded_header_stream_->id();
    const auto action = discarded_header_action_;
    auto hpack_transaction = decoder_.begin_transaction();
    const auto status = action == discarded_header_action_type::refuse_stream
                            ? decode_refused_header_block(*discarded_header_stream_, hpack_transaction)
                            : decode_discarded_header_block(*discarded_header_stream_, hpack_transaction);

    if (status == header_decode_status::compression_error) {
        append_goaway(http2_error_code::compression_error, "invalid HPACK block");
        hpack_transaction.rollback();
        http2_reset_header_block(*discarded_header_stream_);
        discarded_header_stream_.reset();
        discarded_header_action_ = discarded_header_action_type::ignore;
        return false;
    }
    if (action == discarded_header_action_type::ignore) {
        hpack_transaction.commit();
        http2_reset_header_block(*discarded_header_stream_);
        discarded_header_stream_.reset();
        discarded_header_action_ = discarded_header_action_type::ignore;
        return true;
    }

    auto error = http2_error_code::protocol_error;
    if (action == discarded_header_action_type::reset_stream_closed) {
        error = http2_error_code::stream_closed;
    } else if (action == discarded_header_action_type::refuse_stream &&
               status == header_decode_status::ok) {
        error = http2_error_code::refused_stream;
    }
    auto* const live_stream = find_stream(stream_id);
    const bool has_live_stream = live_stream != nullptr;
    if (has_live_stream) {
        reserve_stream_close_effects(*live_stream);
    }
    output_.reserve_additional(http2_frame_header_bytes + 4);
    output_.append_rst_stream(stream_id, error);
    hpack_transaction.commit();
    http2_reset_header_block(*discarded_header_stream_);
    if (has_live_stream) {
        close_stream(stream_id, http2_stream_close_source::local, error);
    } else {
        closed_streams_.remember(stream_id, http2_stream_close_source::local);
    }
    discarded_header_stream_.reset();
    discarded_header_action_ = discarded_header_action_type::ignore;
    return true;
}

header_decode_status http2_connection::finish_trailer_block(http2_stream_state& stream,
    http2_stream_header_decode_transaction& stream_transaction,
    hpack_decoder::decode_transaction_type& hpack_transaction) {
    // A successful trailer block publishes exactly one terminal event. Reserve
    // it before HPACK/application-header decoding so a later vector growth cannot
    // leave the decoded stream half-committed without its message end.
    reserve_event_slots(1);
    http2_header_decode_context context_value{stream, &stream_transaction};
    const auto result_value =
        role_ == http2_role::server
            ? decoder_.decode(
                  stream.remote_header_block(), &context_value,
                  [](void* target, std::string_view name, std::string_view value) {
                      return http2_on_decoded_request_trailer(
                          *static_cast<http2_header_decode_context*>(target), name, value);
                  },
                  hpack_transaction)
            : decoder_.decode(stream.remote_header_block(), &context_value, http2_on_decoded_response_trailer,
                  hpack_transaction);
    if (const auto status = http2_classify_header_decode_result(result_value);
        status != header_decode_status::ok) {
        return status;
    }
    if (!stream.remote_content().terminal_length_valid()) {
        return header_decode_status::protocol_error;
    }
    if (!stream.finish_remote_content()) {
        return header_decode_status::protocol_error;
    }
    events_.push_back(http2_event::message_end(stream.id()));
    release_local_request_stream_if_closed(stream);
    return header_decode_status::ok;
}

void http2_connection::reserve_stream_close_effects(http2_stream_state& stream) {
    reserve_event_slots(1);
    if (const auto debt = stream.window_debt();
        debt != 0 && connection_receive_credit_.ready_after(debt)) {
        output_.reserve_additional(http2_window_update_frame_bytes);
    }
}

bool http2_connection::handle_header_decode_failure(http2_stream_state& stream, header_decode_status status,
    hpack_decoder::decode_transaction_type* hpack_transaction) {
    if (status == header_decode_status::compression_error) {
        append_goaway(http2_error_code::compression_error, "invalid HPACK block");
        http2_reset_header_block(stream);
        return false;
    }
    const bool has_live_stream = find_stream(stream.id()) == &stream;
    if (has_live_stream) {
        reserve_stream_close_effects(stream);
    }
    output_.reserve_additional(http2_frame_header_bytes + 4);
    output_.append_rst_stream(stream.id(), http2_error_code::protocol_error);
    if (hpack_transaction != nullptr && hpack_transaction->active()) {
        hpack_transaction->commit();
    }
    // The error response is now fully reserved and appended. Only this point is
    // safe to consume the compressed block: if either reservation above throws,
    // the caller must be able to retry the exact field block and its HPACK state.
    http2_reset_header_block(stream);
    if (has_live_stream) {
        close_stream(stream.id(), http2_stream_close_source::local,
            http2_error_code::protocol_error);  // emits a typed stream-closed event
    } else {
        (void)stream.abort(http2_stream_close_source::local);
        // Refused-stream scratch is not in the table, but still models the same
        // whole-stream terminal transition as a live stream.
    }
    return true;
}

void http2_connection::emit_request_headers(http2_stream_state& stream) {
    // The head may be followed by one terminal event (message/tunnel end). Reserve
    // exactly the events this branch will publish; retaining one unused slot on
    // every non-terminal request would hide later allocation-failure retries.
    const bool terminal_event =
        http2_remote_peer_half_closed(stream) &&
        !(role_ == http2_role::server && stream.tunnel().pending() != nullptr);
    reserve_event_slots(terminal_event ? 2 : 1);
    const auto request_content_signal = stream.request_content_canceled()
                                            ? std::optional<http_client_request_content_signal>(
                                                  http_client_request_content_signal::exchange_complete)
                                            : std::nullopt;
    events_.push_back(http2_event::message_head(stream.id(), request_content_signal));
    if (role_ == http2_role::server && stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content, but that fact alone says nothing about the
        // peer send half: it can remain open for a tunnel or already carry END_STREAM.
        // Route/accept decisions start from message_head; the typed remote state owns
        // any later tunnel-end signal, so a generic message_end would be misleading.
        return;
    }
    if (role_ == http2_role::client && stream.tunnel().open() != nullptr) {
        if (http2_remote_peer_half_closed(stream)) {
            events_.push_back(http2_event::tunnel_end(stream.id()));
        }
        release_local_request_stream_if_closed(stream);
        return;
    }
    if (http2_remote_peer_half_closed(stream)) {
        events_.push_back(http2_event::message_end(stream.id()));
    }
    release_local_request_stream_if_closed(stream);
}

}  // namespace ruvia::detail
