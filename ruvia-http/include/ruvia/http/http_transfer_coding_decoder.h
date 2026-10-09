#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_transfer_coding.h"
#include "ruvia/http/http_transfer_coding_decode_error.h"
#include "ruvia/http/protocol_byte_limit.h"

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
    explicit constexpr http_transfer_coding_decode_need_input(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_output_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_output_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept
        : consumed_bytes_(consumed_bytes),
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
    explicit constexpr http_transfer_coding_decode_complete(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http_transfer_coding_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_failure(std::size_t consumed_bytes, http_transfer_coding_decode_error error) noexcept
        : consumed_bytes_(consumed_bytes),
          error_(error) {}
    std::size_t consumed_bytes_;
    http_transfer_coding_decode_error error_;
};

class http_transfer_coding_decoder_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decoder_failure(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) { return result_value.consumed_bytes(); }, value_);
    }
    [[nodiscard]] const http_transfer_coding_decode_need_input* need_input() const& noexcept {
        return std::get_if<http_transfer_coding_decode_need_input>(&value_);
    }
    const http_transfer_coding_decode_need_input* need_input() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_output_view* output() const& noexcept {
        return std::get_if<http_transfer_coding_decode_output_view>(&value_);
    }
    const http_transfer_coding_decode_output_view* output() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_complete* complete() const& noexcept {
        return std::get_if<http_transfer_coding_decode_complete>(&value_);
    }
    const http_transfer_coding_decode_complete* complete() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_failure* failure() const& noexcept {
        return std::get_if<http_transfer_coding_decode_failure>(&value_);
    }
    const http_transfer_coding_decode_failure* failure() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decoder_failure* decoder_failure() const& noexcept {
        return std::get_if<http_transfer_coding_decoder_failure>(&value_);
    }
    const http_transfer_coding_decoder_failure* decoder_failure() const&& = delete;

private:
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    using value_type = std::variant<http_transfer_coding_decode_need_input, http_transfer_coding_decode_output_view, http_transfer_coding_decode_complete, http_transfer_coding_decode_failure, http_transfer_coding_decoder_failure>;
    template <typename result_type>
    explicit http_transfer_coding_decode_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}
    value_type value_;
};

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse.
// Input is never retained; output views borrow the caller's scratch storage.
// Consume each view before reusing it. Keep the decoder at a stable address and
// its PMR resource alive until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// decode/finish_input return typed failures because incremental drivers must
// retain the exact wire consumption even when a layer fails; failure is terminal.
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const http_transfer_coding> codings,
        std::pmr::memory_resource* resource, protocol_byte_limit decoded_limit);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(
        std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

private:
    struct impl;
    impl* impl_{nullptr};
};

}  // namespace ruvia
