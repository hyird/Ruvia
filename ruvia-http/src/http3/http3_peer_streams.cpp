#include "ruvia/http/http3_peer_streams.h"

#include <stdexcept>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {

http3_peer_streams::http3_peer_streams(http3_peer_role local_role, std::pmr::memory_resource* resource,
    http3_peer_stream_limits limits)
    : local_role_(local_role),
      limits_(limits),
      streams_(resource) {
    if (resource == nullptr || limits_.max_active_streams_ == 0) {
        throw std::invalid_argument("HTTP/3 peer stream limits and resource must be valid");
    }
}

std::variant<std::monostate, http3_peer_stream_error> http3_peer_streams::validate_peer_uni(
    std::uint64_t stream_id) const noexcept {
    if (!is_http3_peer_unidirectional_stream_id(local_role_, stream_id)) {
        return http3_peer_stream_error::stream_creation_error;
    }
    return {};
}

bool http3_peer_streams::is_critical(http3_peer_stream_kind kind) noexcept {
    return kind == http3_peer_stream_kind::control || kind == http3_peer_stream_kind::qpack_encoder ||
           kind == http3_peer_stream_kind::qpack_decoder;
}

std::variant<http3_peer_stream_feed, http3_peer_stream_error> http3_peer_streams::feed(
    std::uint64_t stream_id, std::span<const char> bytes_value, bool fin, bool reset) {
    if (const auto valid = validate_peer_uni(stream_id); (valid.index() != 0)) {
        return std::get<1>(valid);
    }

    auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        // A FIN/RESET before the stream header is explicitly tolerated.
        if ((fin || reset) && bytes_value.empty()) {
            return http3_peer_stream_feed{.consumed_ = 0, .remaining_ = bytes_value, .fin_ = fin, .reset_ = reset, .closed_ = true};
        }
        if (streams_.size() >= limits_.max_active_streams_) {
            return http3_peer_stream_error::excessive_load;
        }
        found = streams_.try_emplace(stream_id).first;
    }

    auto& state_value = found->second;
    if (state_value.kind_ != http3_peer_stream_kind::unclassified) {
        if ((fin || reset) && is_critical(state_value.kind_)) {
            return http3_peer_stream_error::closed_critical_stream;
        }
        const auto kind = state_value.kind_;
        const auto stream_type_value = state_value.stream_type_;
        if (fin || reset) {
            streams_.erase(found);
        }
        return http3_peer_stream_feed{.kind_ = kind,
            .stream_type_ = stream_type_value,
            .consumed_ = 0,
            .remaining_ = bytes_value,
            .fin_ = fin,
            .reset_ = reset,
            .closed_ = fin || reset};
    }

    std::size_t consumed = 0;
    while (consumed < bytes_value.size() && state_value.type_size_ < state_value.type_bytes_.size()) {
        state_value.type_bytes_[state_value.type_size_++] = bytes_value[consumed++];
        const auto decoded = decode_http3_var_int(
            std::span<const char>(state_value.type_bytes_).first(state_value.type_size_));
        if ((decoded.index() != 0)) {
            continue;
        }

        state_value.stream_type_ = std::get<0>(decoded).value_;
        state_value.kind_ = std::get<0>(decoded).value_ == 0   ? http3_peer_stream_kind::control
                            : std::get<0>(decoded).value_ == 1 ? http3_peer_stream_kind::push
                            : std::get<0>(decoded).value_ == 2 ? http3_peer_stream_kind::qpack_encoder
                            : std::get<0>(decoded).value_ == 3 ? http3_peer_stream_kind::qpack_decoder
                                                               : http3_peer_stream_kind::unknown;
        if (local_role_ == http3_peer_role::server && state_value.kind_ == http3_peer_stream_kind::push) {
            streams_.erase(found);
            return http3_peer_stream_error::stream_creation_error;
        }

        bool* seen = state_value.kind_ == http3_peer_stream_kind::control         ? &control_seen_
                     : state_value.kind_ == http3_peer_stream_kind::qpack_encoder ? &encoder_seen_
                     : state_value.kind_ == http3_peer_stream_kind::qpack_decoder ? &decoder_seen_
                                                                                  : nullptr;
        if (seen != nullptr) {
            if (*seen) {
                streams_.erase(found);
                return http3_peer_stream_error::stream_creation_error;
            }
            *seen = true;
        }
        break;
    }

    if (state_value.kind_ == http3_peer_stream_kind::unclassified) {
        if (fin || reset) {
            streams_.erase(found);
            return http3_peer_stream_feed{.consumed_ = consumed, .remaining_ = bytes_value.subspan(consumed), .fin_ = fin, .reset_ = reset, .closed_ = true};
        }
        return http3_peer_stream_feed{.consumed_ = consumed, .remaining_ = bytes_value.subspan(consumed)};
    }

    if ((fin || reset) && is_critical(state_value.kind_)) {
        return http3_peer_stream_error::closed_critical_stream;
    }
    const auto kind = state_value.kind_;
    const auto stream_type_value = state_value.stream_type_;
    if (fin || reset) {
        streams_.erase(found);
    }
    return http3_peer_stream_feed{.kind_ = kind,
        .stream_type_ = stream_type_value,
        .consumed_ = consumed,
        .remaining_ = bytes_value.subspan(consumed),
        .fin_ = fin,
        .reset_ = reset,
        .closed_ = fin || reset};
}

std::variant<std::monostate, http3_peer_stream_error> http3_peer_streams::accept_bidirectional(
    http3_peer_role local_role, std::uint64_t stream_id) noexcept {
    if (!is_http3_peer_bidirectional_stream_id(local_role, stream_id)) {
        return http3_peer_stream_error::stream_creation_error;
    }
    return {};
}

bool http3_peer_streams::retire_non_critical_stream(std::uint64_t stream_id) noexcept {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end() || is_critical(found->second.kind_)) {
        return false;
    }
    streams_.erase(found);
    return true;
}

std::size_t http3_peer_streams::active_stream_count() const noexcept {
    return streams_.size();
}

}  // namespace ruvia
