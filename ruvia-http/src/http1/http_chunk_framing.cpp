#include "ruvia/http/detail/parser/http_chunk_framing.h"

#include <algorithm>
#include <utility>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia::detail {

chunk_framing_result http_chunk_framing::decode(std::string_view available, std::size_t max_body_bytes) noexcept {
    if (!state_) {
        return chunk_framing_failure{0, state_.error()};
    }

    std::size_t cursor = 0;
    for (;;) {
        switch (*state_) {
            case progress::size_line: {
                const auto line_end = available.find("\r\n", cursor);
                if (line_end == std::string_view::npos) {
                    // A future CRLF can begin at most one byte before the end.
                    if (available.size() - cursor >= kMaxHttpHeaderBytes) {
                        return fail(cursor, chunk_framing_error::framing_limit_exceeded);
                    }
                    return chunk_framing_need_more{cursor};
                }
                if (line_end - cursor + 2 > kMaxHttpHeaderBytes) {
                    return fail(cursor, chunk_framing_error::framing_limit_exceeded);
                }
                std::size_t chunk_size = 0;
                switch (parseHttpChunkSizeLine(available.substr(cursor, line_end - cursor), chunk_size)) {
                    case ChunkSizeLineStatus::kOk:
                        break;
                    case ChunkSizeLineStatus::kInvalidSize:
                        return fail(cursor, chunk_framing_error::invalid_size);
                    case ChunkSizeLineStatus::kOverflow:
                        return fail(cursor, chunk_framing_error::size_overflow);
                    case ChunkSizeLineStatus::kInvalidExtension:
                        return fail(cursor, chunk_framing_error::invalid_extension);
                }
                if (const auto error = account_framing(line_end - cursor + 2)) {
                    return fail(cursor, *error);
                }
                cursor = line_end + 2;
                if (chunk_size == 0) {
                    state_ = progress::trailers;
                    trailer_search_offset_ = 0;
                } else {
                    if (config_.body_limit.additionExceeds(decoded_bytes_, chunk_size)) {
                        return fail(cursor, chunk_framing_error::body_limit_exceeded);
                    }
                    decoded_bytes_ += chunk_size;
                    remaining_ = chunk_size;
                    state_ = progress::body;
                }
                break;
            }
            case progress::body: {
                const auto bytes = std::min({remaining_, available.size() - cursor, max_body_bytes});
                if (bytes == 0) {
                    return chunk_framing_need_more{cursor};
                }
                const auto body = available.substr(cursor, bytes);
                remaining_ -= bytes;
                cursor += bytes;
                if (remaining_ == 0) {
                    if (available.size() - cursor >= 2) {
                        if (const auto error = consume_delimiter(available.substr(cursor))) {
                            return fail(cursor, *error);
                        }
                        cursor += 2;
                        state_ = progress::size_line;
                    } else {
                        state_ = progress::delimiter;
                    }
                }
                return chunk_framing_body{cursor, body};
            }
            case progress::delimiter:
                if (available.size() - cursor < 2) {
                    return chunk_framing_need_more{cursor};
                }
                if (const auto error = consume_delimiter(available.substr(cursor))) {
                    return fail(cursor, *error);
                }
                cursor += 2;
                state_ = progress::size_line;
                break;
            case progress::trailers: {
                const auto trailers = available.substr(cursor);
                if (trailers.starts_with("\r\n")) {
                    if (const auto error = account_framing(2)) {
                        return fail(cursor, *error);
                    }
                    state_ = progress::complete;
                    return chunk_framing_complete{cursor + 2, {}};
                }
                const auto trailer_end = trailers.find("\r\n\r\n", trailer_search_offset_);
                if (trailer_end == std::string_view::npos) {
                    // Keep the final three bytes as a possible delimiter prefix.
                    if (trailers.size() >= kMaxHttpHeaderBytes) {
                        return fail(cursor, chunk_framing_error::framing_limit_exceeded);
                    }
                    trailer_search_offset_ = trailers.size() > 3 ? trailers.size() - 3 : 0;
                    return chunk_framing_need_more{cursor};
                }
                const auto trailer_bytes = trailer_end + 4;
                if (config_.trailer_section_limit.exceeds(trailer_bytes)) {
                    return fail(cursor, chunk_framing_error::trailer_limit_exceeded);
                }
                if (const auto error = validate_trailers(trailers.substr(0, trailer_end))) {
                    return fail(cursor, *error);
                }
                if (const auto error = account_framing(trailer_bytes)) {
                    return fail(cursor, *error);
                }
                state_ = progress::complete;
                return chunk_framing_complete{cursor + trailer_bytes, trailers.substr(0, trailer_end)};
            }
            case progress::complete:
                return chunk_framing_complete{cursor, {}};
        }
    }
}

std::optional<chunk_framing_error> http_chunk_framing::account_framing(std::size_t bytes) noexcept {
    if (bytes > config_.framing_limit - framing_bytes_) {
        return chunk_framing_error::framing_limit_exceeded;
    }
    framing_bytes_ += bytes;
    return std::nullopt;
}

std::optional<chunk_framing_error> http_chunk_framing::consume_delimiter(std::string_view available) noexcept {
    if (!available.starts_with("\r\n")) {
        return chunk_framing_error::invalid_crlf;
    }
    return account_framing(2);
}

std::optional<chunk_framing_error> http_chunk_framing::validate_trailers(std::string_view trailers) const noexcept {
    if (config_.trailer_role == chunk_trailer_role::response) {
        return httpResponseTrailerBlockValid(trailers)
                   ? std::nullopt
                   : std::optional(chunk_framing_error::invalid_trailer);
    }
    if (const auto error = validateHttpChunkTrailers(trailers)) {
        return *error == HttpChunkScanError::kTooLarge
                   ? chunk_framing_error::trailer_limit_exceeded
                   : chunk_framing_error::invalid_trailer;
    }
    return std::nullopt;
}

chunk_framing_result http_chunk_framing::fail(std::size_t consumed_bytes, chunk_framing_error error) noexcept {
    state_ = std::unexpected(error);
    return chunk_framing_failure{consumed_bytes, error};
}

}  // namespace ruvia::detail
