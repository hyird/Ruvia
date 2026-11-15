#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"

namespace ruvia {

// The lifecycle of every stateful incremental parser or decoder. Only an
// explicit reset() returns a finished or failed processor to active; until
// then it takes no further input, in one of two ways:
// - A parser whose terminal step hands out a result to extract (parse() of
//   the HTTP/1 request-head and response-head parsers, the PROXY protocol
//   parser and the SSE parser; multipart_parser::feed()) throws
//   std::logic_error when called again.
// - A streaming decoder (decode() / feed() / poll() of the chunk decoder, the
//   transfer-coding stack, http1_body_decoder, the capsule decoder and
//   multipart_parser::poll()) repeats its terminal step with zero consumed
//   bytes and leaves its state unchanged.
enum class decoder_state : std::uint8_t {
    active,
    finished,
    failed,
};

// The step consumed what it could use; the processor needs more input.
struct decode_need_input final {};

// The processor finished its message and the step carries nothing else.
struct decode_complete final {};

// Decoded bytes. The view borrows the storage named by the decoder that
// produced it: its input, or the caller's scratch.
class decode_output final {
public:
    explicit constexpr decode_output(std::string_view bytes) noexcept :
        bytes_(bytes) {}

    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    std::string_view bytes_;
};

// A data failure, carrying the error enum of the family that reported it.
template <typename error_type>
class decode_failure final {
public:
    explicit constexpr decode_failure(error_type error) noexcept :
        error_(error) {}

    [[nodiscard]] constexpr error_type error() const noexcept {
        return error_;
    }

private:
    error_type error_;
};

// One outcome of an incremental parse or decode call: the input bytes it
// consumed and exactly one of the family's alternatives. The category is fixed
// at construction. The caller drops consumed_bytes() from its input and keeps
// the rest for the next call.
template <typename... alternative_types>
class decode_step final {
public:
    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    constexpr decode_step(std::size_t consumed_bytes, alternative_type alternative) noexcept(std::is_nothrow_move_constructible_v<alternative_type>) :
        consumed_bytes_(consumed_bytes),
        alternative_(std::move(alternative)) {}

    decode_step(const decode_step&) = default;
    decode_step(decode_step&&) = default;
    decode_step& operator=(const decode_step&) = delete;
    decode_step& operator=(decode_step&&) = delete;

    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr bool holds() const noexcept {
        return std::holds_alternative<alternative_type>(alternative_);
    }

    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr const alternative_type* get_if() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<alternative_type>(&alternative_);
    }
    // Extraction from an owning alternative (take_*() &&) goes through the
    // step that still holds it.
    template <typename alternative_type>
        requires(std::same_as<alternative_type, alternative_types> || ...)
    [[nodiscard]] constexpr alternative_type* get_if() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<alternative_type>(&alternative_);
    }
    template <typename alternative_type>
    const alternative_type* get_if() const&& = delete;

    // The same alternative reported over a different input span, for a decoder
    // that composes another one behind its own framing.
    [[nodiscard]] constexpr decode_step with_consumed_bytes(std::size_t consumed_bytes) const {
        auto step = *this;
        step.consumed_bytes_ = consumed_bytes;
        return step;
    }

private:
    std::size_t consumed_bytes_;
    std::variant<alternative_types...> alternative_;
};

}  // namespace ruvia
