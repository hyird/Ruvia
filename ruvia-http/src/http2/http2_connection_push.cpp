#include "http2/http2_connection.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_header_block.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http_header_access.h"

namespace ruvia::detail {

bool http2_connection::process_push_promise(const http2_frame_header& header_value, std::string_view payload_value) {
    if (role_ != http2_role::client || !enable_push_ || header_value.stream_id_ == 0 || (header_value.stream_id_ & 1U) == 0) {
        append_goaway(http2_error_code::protocol_error, "PUSH_PROMISE not permitted");
        return false;
    }
    const auto* parent_value = find_stream(header_value.stream_id_);
    const bool canceled_parent = parent_value ? parent_value->is_aborted() &&
                                                    parent_value->local_send().aborted()->source() == http2_stream_close_source::local
                                              : closed_streams_.source(header_value.stream_id_) == http2_stream_close_source::local;
    if (!canceled_parent && (!parent_value || http2_remote_peer_half_closed(*parent_value))) {
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
    try {
        push_header_stream_.emplace(promised, resource_);
        push_associated_stream_id_ = header_value.stream_id_;
        if (!http2_start_header_block(*push_header_stream_, fragment.substr(4))) {
            push_header_stream_.reset();
            append_goaway(http2_error_code::compression_error, "push field block exceeds limit");
            return false;
        }
        if ((header_value.flags_ & http2_flag_end_headers) != 0) {
            const auto result_value = finish_push_promise();
            push_header_stream_.reset();
            return result_value;
        }
        header_continuation_.start(header_value.stream_id_, http2_header_block_kind::push_promise);
        return true;
    } catch (...) {
        push_header_stream_.reset();
        throw;
    }
}

bool http2_connection::process_push_continuation(const http2_frame_header& header_value, std::string_view payload_value) {
    if (!push_header_stream_) {
        append_goaway(http2_error_code::protocol_error, "missing push continuation state");
        return false;
    }
    const auto checkpoint = header_continuation_.checkpoint();
    const auto bytes_value = push_header_stream_->remote_header_block().size();
    try {
        if (!http2_append_header_block(*push_header_stream_, payload_value)) {
            append_goaway(http2_error_code::compression_error, "push field block exceeds limit");
            return false;
        }
        (void)header_continuation_.record_continuation_frame();
        if ((header_value.flags_ & http2_flag_end_headers) != 0) {
            const auto result_value = finish_push_promise();
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

bool http2_connection::finish_push_promise() {
    auto& scratch = *push_header_stream_;
    // PUSH_PROMISE represents a complete request without a request body.
    (void)scratch.record_remote_head_end_stream();
    http2_stream_header_decode_transaction stream_transaction{scratch, true};
    auto hpack_transaction = decoder_.begin_transaction();
    const auto status = decode_header_block(scratch, stream_transaction, hpack_transaction);
    if (status == header_decode_status::compression_error) {
        append_goaway(http2_error_code::compression_error, "push field block decompression failed");
        return false;
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
    http_push_request request(resource_);
    request.method_ = scratch.request_method();
    request.scheme_ = scratch.request_scheme();
    request.authority_ = scratch.request_authority();
    request.path_ = scratch.request_path();
    request.headers_.reserve(scratch.remote_header_count() + static_cast<std::size_t>(scratch.has_cookie()));
    for (std::size_t i = 0; i < scratch.remote_header_count(); ++i) {
        const auto field = scratch.remote_header_at(i);
        request.headers_.push_back(http_header_access::make(field.name_, field.value_, resource_));
    }
    if (scratch.has_cookie()) {
        request.headers_.push_back(http_header_access::make("cookie", scratch.request_cookie(), resource_));
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
        events_.push_back(http2_event::push_promise({push_associated_stream_id_, id, std::move(request)}));
    } catch (...) {
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
