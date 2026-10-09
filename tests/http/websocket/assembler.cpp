#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

#include "test_harness.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_frame_view.h"
#include "websocket/http_websocket_inbound_assembler.h"

namespace {

using ruvia::protocol_byte_limit;
using ruvia::websocket_message;
using ruvia::websocket_opcode;
using ruvia::detail::websocket_frame_view;
using ruvia::detail::websocket_inbound_assembler;
using ruvia::detail::websocket_inbound_content_encoding;
using ruvia::detail::websocket_inbound_result;
using ruvia::detail::websocket_message_access;
using ruvia::detail::websocket_protocol_failure;
using ruvia::detail::websocket_protocol_failure_close_code;

websocket_frame_view frame(websocket_opcode opcode, std::string_view payload_value, bool fin,
    bool continuation = false, bool rsv1 = false) {
    if (continuation) {
        return websocket_frame_view::continuation(payload_value, fin);
    }
    switch (opcode) {
        case websocket_opcode::text:
            return websocket_frame_view::text(payload_value, fin, rsv1);
        case websocket_opcode::binary:
            return websocket_frame_view::binary(payload_value, fin, rsv1);
        case websocket_opcode::close:
            return *websocket_frame_view::close(payload_value);
        case websocket_opcode::ping:
            return *websocket_frame_view::ping(payload_value);
        case websocket_opcode::pong:
            return *websocket_frame_view::pong(payload_value);
    }
    return websocket_frame_view::text(payload_value, fin, rsv1);
}

protocol_byte_limit byte_limit(std::size_t bytes_value) {
    return protocol_byte_limit::limited(bytes_value);
}

class fail_next_allocation_resource final : public std::pmr::memory_resource {
public:
    void fail_next() noexcept {
        fail_next_ = true;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_next_) {
            fail_next_ = false;
            throw std::bad_alloc();
        }
        return std::pmr::get_default_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(p, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool fail_next_{false};
};

// The RFC 6455 §7.4.1 close code the violation must be reported with (0 if none).
std::uint16_t accept_close_code(websocket_inbound_assembler& assembler, const websocket_frame_view& f,
    protocol_byte_limit message_limit) {
    const auto result_value = assembler.accept(f, message_limit);
    const auto* failure = result_value.failure();
    return failure != nullptr ? websocket_protocol_failure_close_code(failure->error()) : 0;
}

}  // namespace

RUVIA_TEST(ws_assembler_control_frames) {
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const auto ping = assembler.accept(frame(websocket_opcode::ping, "p", true), byte_limit(1000));
    RUVIA_CHECK(ping.control_frame() != nullptr);
    RUVIA_CHECK(ping.control_frame()->opcode() == websocket_opcode::ping);
    RUVIA_CHECK_EQ(ping.control_frame()->payload(), std::string_view("p"));

    const auto pong = assembler.accept(frame(websocket_opcode::pong, "", true), byte_limit(1000));
    RUVIA_CHECK(pong.control_frame() != nullptr);
    RUVIA_CHECK(pong.control_frame()->opcode() == websocket_opcode::pong);

    const auto close = assembler.accept(frame(websocket_opcode::close, "", true), byte_limit(1000));
    RUVIA_CHECK(close.control_frame() != nullptr);
    RUVIA_CHECK(close.control_frame()->opcode() == websocket_opcode::close);
}

RUVIA_TEST(ws_assembler_single_frame_messages) {
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const auto text =
        assembler.accept(frame(websocket_opcode::text, "hello", true), byte_limit(1000));
    RUVIA_CHECK(text.message() != nullptr);
    RUVIA_CHECK_EQ(text.message()->message().payload(), std::string_view("hello"));
    RUVIA_CHECK(text.message()->content_encoding() == websocket_inbound_content_encoding::identity);
    // Binary is delivered without UTF-8 checking.
    const std::string binary("\xff\xfe\x00\x01", 4);
    const auto binary_result =
        assembler.accept(frame(websocket_opcode::binary, binary, true), byte_limit(1000));
    RUVIA_CHECK(binary_result.message() != nullptr);
    RUVIA_CHECK_EQ(binary_result.message()->message().payload(), std::string_view(binary));
    // A compressed (RSV1) frame defers to the connection for inflation.
    const auto compressed =
        assembler.accept(frame(websocket_opcode::text, "z", true, false, true), byte_limit(1000));
    RUVIA_CHECK(compressed.message() != nullptr);
    RUVIA_CHECK(compressed.message()->content_encoding() ==
                websocket_inbound_content_encoding::per_message_deflate);
}

RUVIA_TEST(ws_assembler_invalid_utf8_text) {
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const std::string overlong("\xc0\x80", 2);  // overlong encoding of NUL
    const auto result_value =
        assembler.accept(frame(websocket_opcode::text, overlong, true), byte_limit(1000));
    RUVIA_CHECK(result_value.failure() != nullptr);
    RUVIA_CHECK(result_value.failure()->error() == websocket_protocol_failure::invalid_payload_data);
}

RUVIA_TEST(ws_assembler_fragmented_message) {
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const auto first =
        assembler.accept(frame(websocket_opcode::text, "hel", false), byte_limit(1000));
    RUVIA_CHECK(first.continue_reading() != nullptr);
    const auto second =
        assembler.accept(frame(websocket_opcode::text, "lo ", false, true), byte_limit(1000));
    RUVIA_CHECK(second.continue_reading() != nullptr);
    const auto complete_value =
        assembler.accept(frame(websocket_opcode::text, "world", true, true), byte_limit(1000));
    RUVIA_CHECK(complete_value.message() != nullptr);
    RUVIA_CHECK_EQ(complete_value.message()->message().payload(), std::string_view("hello world"));
}

#if !defined(_MSC_VER)
// MSVC's debug pmr::string does not reliably complete a growth operation when
// the resource throws. Keep the exact allocation-failure retry contract on the
// standard libraries where that failure path is well behaved.
RUVIA_TEST(ws_assembler_first_fragment_allocation_failure_is_retryable) {
    fail_next_allocation_resource resource;
    websocket_inbound_assembler assembler(&resource);
    const std::string payload_value(128, 'x');

    resource.fail_next();
    bool threw = false;
    try {
        (void)assembler.accept(frame(websocket_opcode::text, payload_value, false), byte_limit(1000));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    const auto retried =
        assembler.accept(frame(websocket_opcode::text, payload_value, false), byte_limit(1000));
    RUVIA_CHECK(retried.continue_reading() != nullptr);
    const auto complete_value =
        assembler.accept(frame(websocket_opcode::text, "done", true, true), byte_limit(1000));
    RUVIA_CHECK(complete_value.message() != nullptr);
    RUVIA_CHECK_EQ(complete_value.message()->message().payload().size(),
        payload_value.size() + std::string_view("done").size());
}
#endif  // !_MSC_VER

RUVIA_TEST(ws_assembler_control_frame_interleaved_in_fragments) {
    // RFC 6455 §5.4: a control frame may be injected between the fragments of a
    // data message and MUST NOT disrupt the reassembly already in progress.
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const auto first =
        assembler.accept(frame(websocket_opcode::text, "hel", false), byte_limit(1000));
    RUVIA_CHECK(first.continue_reading() != nullptr);
    // A ping arrives mid-message: it is answered but the fragment state is untouched.
    const auto ping = assembler.accept(frame(websocket_opcode::ping, "p", true), byte_limit(1000));
    RUVIA_CHECK(ping.control_frame() != nullptr);
    RUVIA_CHECK(ping.control_frame()->opcode() == websocket_opcode::ping);
    // The continuation still completes the ORIGINAL message intact.
    const auto complete_value =
        assembler.accept(frame(websocket_opcode::text, "lo", true, true), byte_limit(1000));
    RUVIA_CHECK(complete_value.message() != nullptr);
    RUVIA_CHECK_EQ(complete_value.message()->message().payload(), std::string_view("hello"));
}

RUVIA_TEST(ws_assembler_fragmented_compressed_defers_validation) {
    // A compressed message carries RSV1 on its FIRST frame only; the assembler must
    // remember that across continuation frames and, on completion, defer to the
    // connection for inflation (UTF-8 cannot be judged until the bytes are inflated).
    websocket_inbound_assembler assembler(std::pmr::get_default_resource());
    const auto first = assembler.accept(
        frame(websocket_opcode::text, std::string_view("\x01\x02", 2), false, false, true),
        byte_limit(1000));
    RUVIA_CHECK(first.continue_reading() != nullptr);
    const auto complete_value = assembler.accept(
        frame(websocket_opcode::text, std::string_view("\x03", 1), true, true), byte_limit(1000));
    RUVIA_CHECK(complete_value.message() != nullptr);
    RUVIA_CHECK(complete_value.message()->content_encoding() ==
                websocket_inbound_content_encoding::per_message_deflate);
    RUVIA_CHECK_EQ(complete_value.message()->message().payload().size(), std::size_t{3});
}

RUVIA_TEST(ws_assembler_protocol_errors) {
    // A continuation frame with no message in progress is a protocol violation.
    websocket_inbound_assembler no_start(std::pmr::get_default_resource());
    const auto no_start_result =
        no_start.accept(frame(websocket_opcode::text, "x", true, true), byte_limit(1000));
    RUVIA_CHECK(no_start_result.failure() != nullptr);
    RUVIA_CHECK(no_start_result.failure()->error() == websocket_protocol_failure::protocol_error);

    // A new data frame while a fragmented message is in progress is a violation.
    websocket_inbound_assembler interleaved(std::pmr::get_default_resource());
    const auto started =
        interleaved.accept(frame(websocket_opcode::text, "start", false), byte_limit(1000));
    RUVIA_CHECK(started.continue_reading() != nullptr);
    const auto interleaved_result =
        interleaved.accept(frame(websocket_opcode::text, "new", true), byte_limit(1000));
    RUVIA_CHECK(interleaved_result.failure() != nullptr);
    RUVIA_CHECK(interleaved_result.failure()->error() == websocket_protocol_failure::protocol_error);

    // Exceeding the per-message size limit across fragments is an explicit failure.
    websocket_inbound_assembler too_big(std::pmr::get_default_resource());
    const auto within_limit =
        too_big.accept(frame(websocket_opcode::text, "12345", false), byte_limit(10));
    RUVIA_CHECK(within_limit.continue_reading() != nullptr);
    const auto too_big_result =
        too_big.accept(frame(websocket_opcode::text, "678901", true, true), byte_limit(10));
    RUVIA_CHECK(too_big_result.failure() != nullptr);
    RUVIA_CHECK(too_big_result.failure()->error() == websocket_protocol_failure::message_too_large);

    websocket_inbound_assembler first_frame_too_big(std::pmr::get_default_resource());
    const auto first_frame_result =
        first_frame_too_big.accept(frame(websocket_opcode::binary, "123456", false), byte_limit(5));
    RUVIA_CHECK(first_frame_result.failure() != nullptr);
    RUVIA_CHECK(first_frame_result.failure()->error() == websocket_protocol_failure::message_too_large);
}

RUVIA_TEST(ws_assembler_violations_carry_rfc_close_code) {
    // RFC 6455 §7.4.1: framing/fragmentation violations report 1002 (protocol
    // error); a size-limit breach reports 1009 (message too big). The read loop
    // sends this code rather than the generic 1011 (internal error).
    websocket_inbound_assembler no_start(std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(
        accept_close_code(no_start, frame(websocket_opcode::text, "x", true, true), byte_limit(1000)),
        std::uint16_t{1002});  // continuation with no message open

    websocket_inbound_assembler interleaved(std::pmr::get_default_resource());
    (void)interleaved.accept(frame(websocket_opcode::text, "start", false), byte_limit(1000));
    RUVIA_CHECK_EQ(
        accept_close_code(interleaved, frame(websocket_opcode::text, "new", true), byte_limit(1000)),
        std::uint16_t{1002});  // interleaved non-continuation data frame

    websocket_inbound_assembler too_big(std::pmr::get_default_resource());
    (void)too_big.accept(frame(websocket_opcode::text, "12345", false), byte_limit(10));
    RUVIA_CHECK_EQ(
        accept_close_code(too_big, frame(websocket_opcode::text, "678901", true, true), byte_limit(10)),
        std::uint16_t{1009});  // per-message size limit exceeded
}
