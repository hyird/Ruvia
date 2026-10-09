#include "ruvia/http/http1_client_response_body_decoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ruvia {

std::string_view http1_client_response_body_error_message(http1_client_response_body_error error) noexcept {
    switch (error) {
        case http1_client_response_body_error::invalid_framing:
            return "invalid HTTP/1 response body framing";
        case http1_client_response_body_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_client_response_body_error::incomplete_body:
            return "incomplete HTTP/1 response body";
        case http1_client_response_body_error::non_empty205:
            return "HTTP 205 response content is not empty";
    }
    return "invalid HTTP/1 response body";
}

http1_client_response_body_decoder::http1_client_response_body_decoder(
    http1_client_response_plan plan, std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/1 response body decoder requires a memory resource");
    }
    if (plan.informational() || plan.connect_tunnel() || plan.protocol_upgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    const auto configure_coding = [this, resource](const http_transfer_codings& codings) {
        if (!codings.empty()) {
            transfer_.emplace(std::span<const http_transfer_coding>(
                                  codings.values_.data(), codings.values_.size()),
                resource, protocol_byte_limit::unlimited());
        }
    };
    if (const auto* without = plan.without_content()) {
        framing_ = framing_type::no_body;
        persistence_ = without->persistence();
    } else if (const auto* known = plan.known_length()) {
        framing_ = framing_type::fixed;
        remaining_ = known->content_length();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = framing_type::chunked;
        persistence_ = chunked->persistence();
        configure_coding(chunked->transfer_codings());
        chunked_.emplace(protocol_byte_limit::unlimited());
    } else if (const auto* close = plan.close_delimited()) {
        framing_ = framing_type::close_delimited;
        configure_coding(close->transfer_codings());
    } else if (const auto* zero = plan.zero_content()) {
        zero_content_ = true;
        if (const auto* zero_known = zero->known_length()) {
            framing_ = framing_type::fixed;
            remaining_ = zero_known->content_length();
            persistence_ = zero_known->persistence();
        } else if (const auto* zero_chunked = zero->chunked()) {
            framing_ = framing_type::chunked;
            persistence_ = zero_chunked->persistence();
            configure_coding(zero_chunked->transfer_codings());
            chunked_.emplace(protocol_byte_limit::unlimited());
        } else if (const auto* zero_close = zero->close_delimited()) {
            framing_ = framing_type::close_delimited;
            configure_coding(zero_close->transfer_codings());
        } else {
            throw std::invalid_argument("invalid zero-content HTTP/1 response plan");
        }
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decode(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    if (eof_) {
        return fail(http1_client_response_body_error::incomplete_body);
    }
    return step(available, scratch, false);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_input(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (terminal_) {
        return replay_terminal();
    }
    if (trailers_reported_) {
        if (transfer_) {
            return finish_transfer(0, scratch);
        }
        return complete(0);
    }

    std::size_t consumed = 0;
    for (;;) {
        // A full output step can leave inflate output buffered after consuming
        // all wire input. Drain it before asking framing or transport for more.
        if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
            const auto pending = drive_transfer({}, consumed, scratch);
            if (pending.output() || pending.protocol_failure() || pending.decoder_failure()) {
                return pending;
            }
        }
        if (pending_body_bytes_ != 0) {
            const auto input = available.substr(consumed,
                std::min(pending_body_bytes_, available.size() - consumed));
            const auto driven = drive_transfer(input, consumed, scratch);
            const auto input_consumed = driven.consumed_bytes() - consumed;
            pending_body_bytes_ -= input_consumed;
            consumed += input_consumed;
            if (driven.output() || driven.protocol_failure() || driven.decoder_failure()) {
                if (pending_body_bytes_ == 0) {
                    const auto delimiter = std::min(pending_delimiter_bytes_, available.size() - consumed);
                    consumed += delimiter;
                    pending_delimiter_bytes_ -= delimiter;
                }
                return std::visit([consumed](const auto& payload_value) -> result_type {
                    using t_type = std::decay_t<decltype(payload_value)>;
                    if constexpr (std::is_same_v<t_type, output_view_type>) {
                        return result_type(output_view_type(consumed, payload_value.bytes()));
                    } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
                        return result_type(protocol_failure_type(consumed, payload_value.error()));
                    } else if constexpr (std::is_same_v<t_type, decoder_failure_type>) {
                        return result_type(decoder_failure_type(consumed));
                    } else {
                        return result_type(payload_value);
                    }
                },
                    driven.value_);
            }
            if (pending_body_bytes_ != 0) {
                if (eof) {
                    return fail(http1_client_response_body_error::incomplete_body, consumed);
                }
                return need_input(consumed);
            }
            if (pending_delimiter_bytes_ != 0) {
                const auto delimiter = std::min(pending_delimiter_bytes_, available.size() - consumed);
                consumed += delimiter;
                pending_delimiter_bytes_ -= delimiter;
                if (pending_delimiter_bytes_ != 0) {
                    if (eof) {
                        return fail(http1_client_response_body_error::incomplete_body, consumed);
                    }
                    return need_input(consumed);
                }
            }
            if (driven.need_input() && consumed == available.size()) {
                if (eof) {
                    if (framing_ == framing_type::chunked) {
                        return fail(http1_client_response_body_error::incomplete_body, consumed);
                    }
                    if (framing_ == framing_type::fixed && remaining_ == 0) {
                        return finish_transfer(consumed, scratch);
                    }
                    if (framing_ == framing_type::close_delimited) {
                        return finish_transfer(consumed, scratch);
                    }
                    return fail(http1_client_response_body_error::incomplete_body, consumed);
                }
                return need_input(consumed);
            }
            continue;
        }

        if (pending_delimiter_bytes_ != 0) {
            const auto delimiter = std::min(pending_delimiter_bytes_, available.size() - consumed);
            consumed += delimiter;
            pending_delimiter_bytes_ -= delimiter;
            if (pending_delimiter_bytes_ != 0) {
                if (eof) {
                    return fail(http1_client_response_body_error::incomplete_body, consumed);
                }
                return need_input(consumed);
            }
        }

        const auto rest = available.substr(consumed);
        switch (framing_) {
            case framing_type::no_body:
                return complete(consumed);
            case framing_type::fixed: {
                if (remaining_ == 0) {
                    if (transfer_) {
                        return finish_transfer(consumed, scratch);
                    }
                    return complete(consumed);
                }
                if (rest.empty()) {
                    if (eof) {
                        return fail(http1_client_response_body_error::incomplete_body, consumed);
                    }
                    return need_input(consumed);
                }
                const auto input_length = std::min(remaining_, rest.size());
                if (transfer_) {
                    const auto driven = drive_transfer(rest.substr(0, input_length), consumed, scratch);
                    const auto input_consumed = driven.consumed_bytes() - consumed;
                    remaining_ -= input_consumed;
                    if (driven.output() || driven.protocol_failure() || driven.decoder_failure()) {
                        return driven;
                    }
                    consumed += input_consumed;
                    if (remaining_ == 0) {
                        if (driven.need_input()) {
                            return finish_transfer(consumed, scratch);
                        }
                    }
                    if (input_consumed == 0 || consumed == available.size()) {
                        if (eof && remaining_ != 0) {
                            return fail(http1_client_response_body_error::incomplete_body, consumed);
                        }
                        return need_input(consumed);
                    }
                    continue;
                }
                const auto output_length = std::min(input_length, scratch.size());
                if (zero_content_ && output_length != 0) {
                    return fail(http1_client_response_body_error::non_empty205, consumed + output_length);
                }
                remaining_ -= output_length;
                return emit(consumed + output_length, rest.substr(0, output_length));
            }
            case framing_type::close_delimited: {
                if (transfer_) {
                    if (transfer_phase_ == transfer_phase_type::ended) {
                        if (!rest.empty()) {
                            return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
                        }
                        if (eof) {
                            return complete(consumed);
                        }
                        return need_input(consumed);
                    }
                    if (!rest.empty()) {
                        const auto driven = drive_transfer(rest, consumed, scratch);
                        const auto input_consumed = driven.consumed_bytes() - consumed;
                        consumed += input_consumed;
                        if (driven.output() || driven.protocol_failure() || driven.decoder_failure()) {
                            return driven;
                        }
                        if (transfer_phase_ == transfer_phase_type::ended) {
                            continue;
                        }
                        if (consumed < available.size() && input_consumed != 0) {
                            continue;
                        }
                        if (consumed == available.size()) {
                            if (eof) {
                                return finish_transfer(consumed, scratch);
                            }
                            return need_input(consumed);
                        }
                        if (eof) {
                            return fail(http1_client_response_body_error::incomplete_body, consumed);
                        }
                        return need_input(consumed);
                    }
                    if (eof) {
                        return finish_transfer(consumed, scratch);
                    }
                    return need_input(consumed);
                }
                if (!rest.empty()) {
                    const auto output_length = std::min(rest.size(), scratch.size());
                    if (zero_content_ && output_length != 0) {
                        return fail(http1_client_response_body_error::non_empty205, consumed + output_length);
                    }
                    return emit(consumed + output_length, rest.substr(0, output_length));
                }
                return eof ? complete(consumed) : need_input(consumed);
            }
            case framing_type::chunked: {
                const auto decoded = chunked_->decode(rest,
                    transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
                const auto child_consumed = decoded.consumed_bytes();
                if (decoded.failure() != nullptr) {
                    return fail(http1_client_response_body_error::invalid_framing, consumed + child_consumed);
                }
                if (decoded.need_more() != nullptr) {
                    consumed += child_consumed;
                    if (eof) {
                        return fail(http1_client_response_body_error::incomplete_body, consumed);
                    }
                    return need_input(consumed);
                }
                if (const auto* complete = decoded.complete()) {
                    trailers_reported_ = true;
                    return validated_trailers(consumed + child_consumed, complete->trailers());
                }

                const auto body = decoded.body_chunk()->bytes();
                if (!transfer_) {
                    if (zero_content_ && !body.empty()) {
                        return fail(http1_client_response_body_error::non_empty205,
                            consumed + child_consumed);
                    }
                    return emit(consumed + child_consumed, body);
                }
                const auto body_offset = static_cast<std::size_t>(body.data() - rest.data());
                if (body_offset > child_consumed || body.size() > child_consumed - body_offset) {
                    return fail(http1_client_response_body_error::invalid_framing, consumed + child_consumed);
                }
                const auto delimiter_debt = child_consumed - body_offset - body.size();
                const auto driven = drive_transfer(body, consumed + body_offset, scratch);
                const auto input_consumed = driven.consumed_bytes() - (consumed + body_offset);
                pending_body_bytes_ = body.size() - std::min(body.size(), input_consumed);
                pending_delimiter_bytes_ = delimiter_debt;
                const auto wire_consumed = driven.consumed_bytes();
                if (pending_body_bytes_ == 0) {
                    const auto available_delimiter = std::min(
                        pending_delimiter_bytes_, available.size() - wire_consumed);
                    pending_delimiter_bytes_ -= available_delimiter;
                    if (pending_delimiter_bytes_ == 0) {
                        // Delimiter was already validated by the chunk decoder.
                        if (driven.output()) {
                            return emit(wire_consumed + available_delimiter, driven.output()->bytes());
                        }
                        if (driven.protocol_failure()) {
                            return fail(driven.protocol_failure()->error(),
                                wire_consumed + available_delimiter);
                        }
                        if (driven.decoder_failure()) {
                            return decoder_failure(wire_consumed + available_delimiter);
                        }
                        consumed = wire_consumed + available_delimiter;
                        if (driven.need_input()) {
                            if (consumed == available.size()) {
                                if (eof) {
                                    return fail(http1_client_response_body_error::incomplete_body, consumed);
                                }
                                return need_input(consumed);
                            }
                            continue;
                        }
                    }
                }
                if (driven.output()) {
                    return emit(wire_consumed, driven.output()->bytes());
                }
                if (driven.protocol_failure()) {
                    return fail(driven.protocol_failure()->error(), wire_consumed);
                }
                if (driven.decoder_failure()) {
                    return decoder_failure(wire_consumed);
                }
                if (pending_body_bytes_ == 0 && pending_delimiter_bytes_ == 0) {
                    consumed = wire_consumed;
                    continue;
                }
                if (eof) {
                    return fail(http1_client_response_body_error::incomplete_body, wire_consumed);
                }
                return need_input(wire_consumed);
            }
        }
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::drive_transfer(
    std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.output()) {
        if (zero_content_ && !output->bytes().empty()) {
            return fail(http1_client_response_body_error::non_empty205, consumed);
        }
        transfer_phase_ = transfer_phase_type::drain_output;
        return emit(consumed, output->bytes());
    }
    if (decoded.need_input() != nullptr) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return need_input(consumed);
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return need_input(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
    }
    return decoder_failure(consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_transfer(
    std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    auto decoded = transfer_->decode({}, scratch);
    if (const auto* output = decoded.output()) {
        if (zero_content_ && !output->bytes().empty()) {
            return fail(http1_client_response_body_error::non_empty205, consumed);
        }
        transfer_phase_ = transfer_phase_type::drain_output;
        return emit(consumed, output->bytes());
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    if (decoded.need_input() != nullptr) {
        decoded = transfer_->finish_input();
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    if (const auto* output = decoded.output()) {
        if (zero_content_ && !output->bytes().empty()) {
            return fail(http1_client_response_body_error::non_empty205, consumed);
        }
        transfer_phase_ = transfer_phase_type::drain_output;
        return emit(consumed, output->bytes());
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    return fail(http1_client_response_body_error::incomplete_body, consumed);
}

http1_client_response_body_decoder::result_type
http1_client_response_body_decoder::replay_terminal() const noexcept {
    return std::visit([](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(0, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(0, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(0));
        }
    },
        *terminal_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input(
    std::size_t consumed) const noexcept {
    return result_type(need_input_type(consumed));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit(
    std::size_t consumed, std::string_view bytes_value) const noexcept {
    return result_type(output_view_type(consumed, bytes_value));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::validated_trailers(
    std::size_t consumed, std::string_view bytes_value) {
    return result_type(validated_trailers_view_type(consumed, bytes_value));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::complete(
    std::size_t consumed) {
    const auto completed = complete_type(consumed, persistence_);
    terminal_ = completed;
    return result_type(completed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::fail(
    http1_client_response_body_error error, std::size_t consumed) {
    const auto failure = protocol_failure_type(consumed, error);
    terminal_ = failure;
    return result_type(failure);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decoder_failure(
    std::size_t consumed) {
    const auto failure = decoder_failure_type(consumed);
    terminal_ = failure;
    return result_type(failure);
}

std::size_t http1_client_response_body_decoder::result_type::consumed_bytes() const noexcept {
    return std::visit([](const auto& payload_value) { return payload_value.consumed_bytes(); }, value_);
}

const http1_client_response_body_decoder::need_input_type*
http1_client_response_body_decoder::result_type::need_input() const& noexcept {
    return std::get_if<need_input_type>(&value_);
}

const http1_client_response_body_decoder::output_view_type*
http1_client_response_body_decoder::result_type::output() const& noexcept {
    return std::get_if<output_view_type>(&value_);
}

const http1_client_response_body_decoder::validated_trailers_view_type*
http1_client_response_body_decoder::result_type::trailers() const& noexcept {
    return std::get_if<validated_trailers_view_type>(&value_);
}

const http1_client_response_body_decoder::complete_type*
http1_client_response_body_decoder::result_type::complete() const& noexcept {
    return std::get_if<complete_type>(&value_);
}

const http1_client_response_body_decoder::protocol_failure_type*
http1_client_response_body_decoder::result_type::protocol_failure() const& noexcept {
    return std::get_if<protocol_failure_type>(&value_);
}

const http1_client_response_body_decoder::decoder_failure_type*
http1_client_response_body_decoder::result_type::decoder_failure() const& noexcept {
    return std::get_if<decoder_failure_type>(&value_);
}

}  // namespace ruvia
