#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http_content_coding.h"
#include "http_protocol.h"

namespace ruvia {

class http_codec_memory;

using http_transfer_coding_decode_failure = decode_failure<http_content_decode_error>;

// One decode() or finish_input() outcome: more input needed, decoded bytes in
// the caller's output storage, the end of every coding, or a data failure.
using http_transfer_coding_decode_result = decode_step<decode_need_input, decode_output, decode_complete, http_transfer_coding_decode_failure>;

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse; every
// coding must satisfy is_http_transfer_coding(), else std::invalid_argument.
// Input is never retained and must not overlap the caller's scratch storage.
// Output views borrow that storage.
// Consume each view before reusing it. Keep the decoder at a stable address
// until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// A layer that received no coded bytes before framing EOF completes as empty.
// Data errors are typed failures because incremental drivers must retain the
// exact wire consumption even when a layer fails. A finished or failed
// decoder repeats its terminal result without consuming input (the caller
// decides what bytes after a finished coding mean); only reset() starts
// another coded body. A codec fault throws and leaves the stack failed; any
// subsequent decode() or finish_input() reports invalid_content until reset().
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit, http_codec_memory* memory = nullptr);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output RUVIA_LIFETIMEBOUND);
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

    [[nodiscard]] decoder_state state() const noexcept;
    // Starts a new coded body with the constructed codings and limit, keeping
    // the codec storage. Throws http_content_codec_error when a codec cannot
    // restart; the decoder is then unusable.
    void reset();

private:
    struct impl_type;
    impl_type* impl_{nullptr};
};

}  // namespace ruvia
