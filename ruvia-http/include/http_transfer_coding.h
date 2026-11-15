#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_content_coding.h"
#include "http_protocol.h"

namespace ruvia {

namespace detail {
class transfer_coding_decoder;
}

class http_transfer_coding_decode_need_input final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_need_input(std::size_t consumed_bytes) noexcept :
        consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_output_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_output_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept :
        consumed_bytes_(consumed_bytes),
        bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http_transfer_coding_decode_complete final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_complete(std::size_t consumed_bytes) noexcept :
        consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http_content_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_failure(std::size_t consumed_bytes, http_content_decode_error error) noexcept :
        consumed_bytes_(consumed_bytes),
        error_(error) {}
    std::size_t consumed_bytes_;
    http_content_decode_error error_;
};

class http_transfer_coding_decode_result final {
public:
    http_transfer_coding_decode_result(const http_transfer_coding_decode_result&) = default;
    http_transfer_coding_decode_result(http_transfer_coding_decode_result&&) noexcept = default;
    http_transfer_coding_decode_result& operator=(const http_transfer_coding_decode_result&) = delete;
    http_transfer_coding_decode_result& operator=(http_transfer_coding_decode_result&&) = delete;
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) {
            return result_value.consumed_bytes();
        }, value_);
    }
    [[nodiscard]] const http_transfer_coding_decode_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_need_input>(&value_);
    }
    const http_transfer_coding_decode_need_input* need_input() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_output_view* output() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_output_view>(&value_);
    }
    const http_transfer_coding_decode_output_view* output() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_complete* complete() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_complete>(&value_);
    }
    const http_transfer_coding_decode_complete* complete() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_failure>(&value_);
    }
    const http_transfer_coding_decode_failure* failure() const&& = delete;

private:
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    using value_type = std::variant<http_transfer_coding_decode_need_input, http_transfer_coding_decode_output_view, http_transfer_coding_decode_complete, http_transfer_coding_decode_failure>;
    template <typename result_type>
    explicit http_transfer_coding_decode_result(result_type result_value) noexcept :
        value_(std::move(result_value)) {}
    value_type value_;
};

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse; every
// coding must satisfy is_http_transfer_coding(), else std::invalid_argument.
// Input is never retained; output views borrow the caller's scratch storage.
// Consume each view before reusing it. Keep the decoder at a stable address
// until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// A layer that received no coded bytes before framing EOF completes as empty.
// Data errors are typed failures because incremental drivers must retain the
// exact wire consumption even when a layer fails; failure is terminal. A codec
// that cannot run throws http_content_codec_error; the decoder is then unusable.
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output);
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

private:
    struct impl_type;
    impl_type* impl_{nullptr};
};

}  // namespace ruvia
