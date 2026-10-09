#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_response_body_decoding.h"

namespace ruvia {

enum class http1_client_response_body_error : unsigned char {
    invalid_framing,
    invalid_transfer_coding,
    incomplete_body,
    non_empty205,
};

[[nodiscard]] std::string_view http1_client_response_body_error_message(
    http1_client_response_body_error error) noexcept;

class http1_client_response_body_decoder final {
public:
    class need_input_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }

    private:
        friend class http1_client_response_body_decoder;
        explicit constexpr need_input_type(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class output_view_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
            return bytes_;
        }
        std::string_view bytes() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        constexpr output_view_type(std::size_t consumed, std::string_view bytes_value) noexcept
            : consumed_(consumed),
              bytes_(bytes_value) {}
        std::size_t consumed_;
        std::string_view bytes_;
    };

    class validated_trailers_view_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
            return bytes_;
        }
        std::string_view bytes() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        constexpr validated_trailers_view_type(std::size_t consumed, std::string_view bytes_value) noexcept
            : consumed_(consumed),
              bytes_(bytes_value) {}
        std::size_t consumed_;
        std::string_view bytes_;
    };

    class complete_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr http1_close_policy persistence() const noexcept {
            return persistence_;
        }

    private:
        friend class http1_client_response_body_decoder;
        constexpr complete_type(std::size_t consumed, http1_close_policy persistence) noexcept
            : consumed_(consumed),
              persistence_(persistence) {}
        std::size_t consumed_;
        http1_close_policy persistence_;
    };

    class protocol_failure_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr http1_client_response_body_error error() const noexcept {
            return error_;
        }

    private:
        friend class http1_client_response_body_decoder;
        constexpr protocol_failure_type(std::size_t consumed, http1_client_response_body_error error) noexcept
            : consumed_(consumed),
              error_(error) {}
        std::size_t consumed_;
        http1_client_response_body_error error_;
    };

    class decoder_failure_type final {
    public:
        [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
            return consumed_;
        }

    private:
        friend class http1_client_response_body_decoder;
        explicit constexpr decoder_failure_type(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class result_type final {
    public:
        [[nodiscard]] std::size_t consumed_bytes() const noexcept;
        [[nodiscard]] const need_input_type* need_input() const& noexcept;
        const need_input_type* need_input() const&& = delete;
        [[nodiscard]] const output_view_type* output() const& noexcept;
        const output_view_type* output() const&& = delete;
        [[nodiscard]] const validated_trailers_view_type* trailers() const& noexcept;
        const validated_trailers_view_type* trailers() const&& = delete;
        [[nodiscard]] const complete_type* complete() const& noexcept;
        const complete_type* complete() const&& = delete;
        [[nodiscard]] const protocol_failure_type* protocol_failure() const& noexcept;
        const protocol_failure_type* protocol_failure() const&& = delete;
        [[nodiscard]] const decoder_failure_type* decoder_failure() const& noexcept;
        const decoder_failure_type* decoder_failure() const&& = delete;

    private:
        friend class http1_client_response_body_decoder;
        using value_type = std::variant<need_input_type, output_view_type, validated_trailers_view_type, complete_type,
            protocol_failure_type, decoder_failure_type>;
        template <typename t_type>
        explicit result_type(t_type value) noexcept
            : value_(std::move(value)) {}
        value_type value_;
    };

    http1_client_response_body_decoder(http1_client_response_plan plan,
        std::pmr::memory_resource* resource);
    ~http1_client_response_body_decoder() = default;
    http1_client_response_body_decoder(const http1_client_response_body_decoder&) = delete;
    http1_client_response_body_decoder& operator=(const http1_client_response_body_decoder&) = delete;
    http1_client_response_body_decoder(http1_client_response_body_decoder&&) = delete;
    http1_client_response_body_decoder& operator=(http1_client_response_body_decoder&&) = delete;

    // Output borrows available without transfer coding and scratch otherwise;
    // trailers borrow available. Consume views before changing either storage
    // or making the next call. Erase exactly consumed_bytes() from available.
    [[nodiscard]] result_type decode(std::string_view available, std::span<char> scratch);
    // EOF is monotonic. Supply any remaining buffered bytes; later calls may
    // drain decoder output but can never request more network input.
    [[nodiscard]] result_type finish_input(std::string_view available, std::span<char> scratch);
    template <detail::http_temporary_owning_char_string input_type>
    result_type decode(input_type&&, std::span<char>) = delete;
    template <detail::http_temporary_owning_char_string input_type>
    result_type finish_input(input_type&&, std::span<char>) = delete;

private:
    enum class framing_type : unsigned char { no_body,
        fixed,
        chunked,
        close_delimited };
    enum class transfer_phase_type : unsigned char { needs_framed_input,
        drain_output,
        ended };
    [[nodiscard]] result_type step(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] result_type replay_terminal() const noexcept;
    [[nodiscard]] result_type need_input(std::size_t consumed = 0) const noexcept;
    [[nodiscard]] result_type emit(std::size_t consumed, std::string_view bytes) const noexcept;
    [[nodiscard]] result_type validated_trailers(std::size_t consumed, std::string_view bytes);
    [[nodiscard]] result_type complete(std::size_t consumed);
    [[nodiscard]] result_type fail(http1_client_response_body_error error, std::size_t consumed = 0);
    [[nodiscard]] result_type decoder_failure(std::size_t consumed = 0);
    [[nodiscard]] result_type drive_transfer(
        std::string_view input, std::size_t wire_prefix, std::span<char> scratch);
    [[nodiscard]] result_type finish_transfer(std::size_t consumed, std::span<char> scratch);

    using terminal_type = std::variant<complete_type, protocol_failure_type, decoder_failure_type>;
    framing_type framing_;
    http1_close_policy persistence_{http1_close_policy::close_after_response};
    std::optional<http_transfer_coding_stack_decoder> transfer_;
    std::optional<http_response_chunked_body_decoder> chunked_;
    std::optional<terminal_type> terminal_;
    std::size_t remaining_{0};
    // The chunk parser advances through a whole body view. These counters let
    // the caller retain its unconsumed wire suffix without storing a borrowed view.
    std::size_t pending_body_bytes_{0};
    std::size_t pending_delimiter_bytes_{0};
    bool zero_content_{false};
    bool eof_{false};
    bool trailers_reported_{false};
    transfer_phase_type transfer_phase_{transfer_phase_type::needs_framed_input};
};

}  // namespace ruvia
