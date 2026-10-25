#include <vector>

#include "ruvia/http/http_header.h"

#include "http2/http2_connection.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_header_block.h"
#include "http2/http2_remote_receive_semantics.h"

namespace ruvia::detail {

bool http2_connection::process_push_promise(const http2_frame_header& header_value, std::string_view payload_value) {
    if (role_ != http2_role::client || !enable_push_ || header_value.stream_id_ == 0 || (header_value.stream_id_ & 1U) == 0) {
        append_goaway(http2_error_code::protocol_error, "PUSH_PROMISE not permitted");
        return false;
    }
    const auto* parent_value = find_stream(header_value.stream_id_);
    // RFC 9113 section 6.6: a PUSH_PROMISE on a stream that is neither open nor
    // half-closed (local) is a connection error, except when this endpoint sent
    // RST_STREAM on it. A normally completed stream was never reset, so only the
    // recorded reset flag grants the exception.
    const bool canceled_parent = parent_value != nullptr
                                     ? parent_value->is_aborted() &&
                                           parent_value->local_send().aborted()->source() ==
                                               http2_stream_close_source::local
                                     : closed_streams_.local_reset_sent(header_value.stream_id_);
    const bool parent_connected_tunnel = parent_value != nullptr && parent_value->tunnel().open() != nullptr;
    // RFC 9113 section 6.6 admits a PUSH_PROMISE on any open or half-closed
    // (local) peer-initiated stream, including a CONNECT whose response is still
    // pending. A connected tunnel is handled after HPACK decoding, where section
    // 8.5 makes the frame a stream error rather than a connection error.
    if (!parent_connected_tunnel &&
        (!canceled_parent && (!parent_value || http2_remote_peer_half_closed(*parent_value)))) {
        append_goaway(http2_error_code::protocol_error, "PUSH_PROMISE on closed or idle stream");
        return false;
    }
    std::string_view fragment;
    if (http2_strip_pad_and_priority(header_value, payload_value, false, fragment) != http2_frame_payload_status::decoded) {
        append_goaway(http2_error_code::protocol_error, "invalid PUSH_PROMISE padding");
        return false;
    }
    if (fragment.size() < 4) {
        append_goaway(http2_error_code::frame_size_error, "missing promised stream id");
        return false;
    }
    const auto promised = http2_read31(reinterpret_cast<const unsigned char*>(fragment.data()));
    if (promised == 0 || (promised & 1U) != 0 || promised <= last_peer_push_stream_id_) {
        append_goaway(http2_error_code::protocol_error, "invalid promised stream id");
        return false;
    }
    const auto block = fragment.substr(4);
    const bool end_headers = (header_value.flags_ & http2_flag_end_headers) != 0;
    try {
        push_header_stream_.emplace(promised, resource_);
        push_associated_stream_id_ = header_value.stream_id_;
        push_reject_connected_tunnel_ = parent_connected_tunnel;
        if (end_headers ? block.size() > max_http2_encoded_header_block_bytes
                        : !http2_start_header_block(*push_header_stream_, block)) {
            push_header_stream_.reset();
            push_reject_connected_tunnel_ = false;
            append_goaway(http2_error_code::compression_error, "push field block exceeds limit");
            return false;
        }
        if (end_headers) {
            // Decode a complete block directly; only fragmented input needs owned
            // compressed storage. Published promise metadata owns its decoded bytes.
            const auto result_value = finish_push_promise(block);
            push_header_stream_.reset();
            return result_value;
        }
        header_continuation_.start(header_value.stream_id_, http2_header_block_kind::push_promise);
        return true;
    } catch (...) {
        push_header_stream_.reset();
        push_reject_connected_tunnel_ = false;
        throw;
    }
}

bool http2_connection::process_push_continuation(const http2_frame_header& header_value, std::string_view payload_value) {
    if (!push_header_stream_) {
        push_reject_connected_tunnel_ = false;
        append_goaway(http2_error_code::protocol_error, "missing push continuation state");
        return false;
    }
    const auto checkpoint = header_continuation_.checkpoint();
    const auto bytes_value = push_header_stream_->remote_header_block().size();
    try {
        if (!http2_append_header_block(*push_header_stream_, payload_value)) {
            push_reject_connected_tunnel_ = false;
            append_goaway(http2_error_code::compression_error, "push field block exceeds limit");
            return false;
        }
        (void)header_continuation_.record_continuation_frame();
        if ((header_value.flags_ & http2_flag_end_headers) != 0) {
            const auto result_value = finish_push_promise(push_header_stream_->remote_header_block());
            push_header_stream_.reset();
            header_continuation_.reset();
            return result_value;
        }
        return true;
    } catch (...) {
        if (push_header_stream_) {
            push_header_stream_->remote_header_block().resize(bytes_value);
        }
        header_continuation_.restore(checkpoint);
        throw;
    }
}

bool http2_connection::finish_push_promise(std::string_view block) {
    auto& scratch = *push_header_stream_;
    const bool reject_connected_tunnel = push_reject_connected_tunnel_;
    push_reject_connected_tunnel_ = false;
    // PUSH_PROMISE represents a complete request without a request body.
    (void)scratch.record_remote_head_end_stream();
    http2_stream_header_decode_transaction stream_transaction{scratch, true};
    auto hpack_transaction = decoder_.begin_transaction();
    const auto status = decode_header_block(scratch, block, hpack_transaction);
    if (status == header_decode_status::compression_error) {
        append_goaway(http2_error_code::compression_error, "push field block decompression failed");
        return false;
    }
    if (reject_connected_tunnel) {
        // RFC 9113 section 8.5: on a connected stream only DATA, RST_STREAM,
        // WINDOW_UPDATE and PRIORITY are allowed; any other frame is a stream
        // error. The block still had to be decoded so HPACK stays synchronized,
        // and the promised stream is reserved by the frame (section 6.6), so it
        // is cancelled explicitly instead of being left dangling.
        const auto promised_id = scratch.id();
        reserve_stream_close_effects();
        output_.reserve_additional(http2_frame_header_bytes + 4);
        output_.append_rst_stream(push_associated_stream_id_, http2_error_code::protocol_error);
        output_.append_rst_stream(promised_id, http2_error_code::cancel);
        hpack_transaction.commit();
        stream_transaction.commit();
        closed_streams_.remember(promised_id, http2_stream_close_source::local, true);
        last_peer_push_stream_id_ = promised_id;
        last_stream_id_ = promised_id;
        close_stream(push_associated_stream_id_, http2_stream_close_source::local,
            http2_error_code::protocol_error);
        return true;
    }
    if (status != header_decode_status::ok ||
        (scratch.request_method() != "GET" && scratch.request_method() != "HEAD")) {
        output_.append_rst_stream(scratch.id(), http2_error_code::protocol_error);
        hpack_transaction.commit();
        stream_transaction.commit();
        last_peer_push_stream_id_ = scratch.id();
        last_stream_id_ = scratch.id();
        return true;
    }
    if (const auto* graceful_drain = local_connection_state_.graceful_drain();
        graceful_drain != nullptr && scratch.id() > graceful_drain->last_stream_id()) {
        output_.append_rst_stream(scratch.id(), http2_error_code::refused_stream);
        hpack_transaction.commit();
        stream_transaction.commit();
        last_peer_push_stream_id_ = scratch.id();
        last_stream_id_ = scratch.id();
        return true;
    }
    http_push_request request(resource_);
    request.method_ = scratch.request_method();
    request.scheme_ = scratch.request_scheme();
    request.authority_ = scratch.request_authority();
    request.path_ = scratch.request_path();
    request.headers_.reserve(scratch.remote_header_count() + static_cast<std::size_t>(scratch.has_cookie()));
    for (std::size_t i = 0; i < scratch.remote_header_count(); ++i) {
        const auto field = scratch.remote_header_at(i);
        request.headers_.emplace_back(field.name_, field.value_, resource_);
    }
    if (scratch.has_cookie()) {
        request.headers_.emplace_back("cookie", scratch.request_cookie(), resource_);
    }
    reserve_event_slots(1);
    const auto id = scratch.id();
    auto* stream = create_stream(id);
    if (!stream) {
        output_.append_rst_stream(id, http2_error_code::enhance_your_calm);
        hpack_transaction.commit();
        stream_transaction.commit();
        last_peer_push_stream_id_ = id;
        last_stream_id_ = id;
        return true;
    }
    try {
        stream->assign_request_method(request.method_);
        stream->assign_request_scheme(request.scheme_);
        stream->assign_request_authority(request.authority_);
        stream->assign_request_path(request.path_);
        stream->begin_local_content_forbidden();
        (void)stream->commit_local_head_end_stream();
        stream->reserve_push(http2_push_reservation::remote);
        // Later frames in this feed may reset the push before queued response
        // events are materialized. Retain their stream storage at publication.
        pin_stream(id);
        events_.push_back(http2_event(http2_push_promise_event{push_associated_stream_id_, id, std::move(request)}));
    } catch (...) {
        std::erase(pinned_streams_, id);
        streams_.remove(id);
        throw;
    }
    hpack_transaction.commit();
    stream_transaction.commit();
    last_peer_push_stream_id_ = id;
    last_stream_id_ = id;
    // Transactions borrow scratch until their destructors run.
    return true;
}

}  // namespace ruvia::detail
