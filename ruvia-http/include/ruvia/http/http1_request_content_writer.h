#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http_header.h"

namespace ruvia {
enum class http1_request_content_write_error : std::uint8_t {
    awaiting_continue,
    stopped,
    write_pending,
    length_overflow,
    length_mismatch,
    invalid_trailer,
    trailers_require_chunked,
    output_too_small,
    header_too_large,
};

// Bound to a successfully prepared streaming request's immutable framing facts.
// Payload bytes remain borrowed until commit_chunk(); prefixes/suffixes are
// independent scatter-gather segments. A transport failure must abort the writer.
// release_content() is called on 100 Continue or on the driver's finite timeout.
// A final response before upload completion requires abort(), not an implicit FIN.
class http1_request_content_writer final {
public:
    struct chunk final {
        std::array<char, 2 * sizeof(std::size_t) + 2> prefix_{};
        std::size_t prefix_size_{0};
        std::span<const char> payload_{};
        std::string_view suffix_{};

        [[nodiscard]] constexpr std::string_view prefix() const noexcept {
            return std::string_view(prefix_.data(), prefix_size_);
        }
    };
    explicit http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept;
    void release_content() noexcept;
    void abort() noexcept;
    [[nodiscard]] std::variant<chunk, http1_request_content_write_error> plan_chunk(std::span<const char> payload) noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<chunk, http1_request_content_write_error> plan_chunk(payload_type&&) noexcept = delete;
    void commit_chunk();
    // Returns a view into buffer. Transmit it, then commit_finish(). Known-length
    // bodies return an empty view and still require a successful commit_finish().
    // Trailer names and values may borrow buffer. Failure leaves buffer unchanged.
    [[nodiscard]] std::variant<std::string_view, http1_request_content_write_error> plan_finish(
        std::span<char> buffer, std::span<const http_header_view> trailers = {}) noexcept;
    void commit_finish();
    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] std::uint64_t committed_payload_bytes() const noexcept {
        return committed_;
    }

private:
    std::optional<std::uint64_t> length_;
    std::uint64_t committed_{0};
    std::size_t pending_{0};
    bool gated_{false}, stopped_{false}, writing_{false}, finishing_{false}, finished_{false};
};
}  // namespace ruvia
