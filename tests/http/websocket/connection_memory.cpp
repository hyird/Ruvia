#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_deflate_codecs.h"

#include "test_exceptions.h"

namespace {
using namespace ruvia;

bool zero_mask(void*, websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

struct client_endpoint final {
    explicit client_endpoint(websocket_compression compression)
        : protocol_(input_, {.message_limit_ = protocol_byte_limit::unlimited(),
                                .compression_ = compression,
                                .role_ = websocket_connection_role::client,
                                .mask_key_generator_ = &zero_mask}) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

// Takes every pending output byte of a sender.
template <typename sender_type>
[[nodiscard]] std::string drain(sender_type& sender) {
    const auto plan = sender.output_plan();
    std::string bytes_value(plan.bytes());
    bytes_value.append(plan.borrowed_bytes());
    EXPECT_TRUE(sender.consume_output(bytes_value.size()) == websocket_output_consume_status::drained);
    return bytes_value;
}

struct server_endpoint final {
    explicit server_endpoint(websocket_connection_options options)
        : input_(),
          protocol_(input_, [&] {
              options.message_limit_ = protocol_byte_limit::unlimited();
              return options;
          }()) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

}  // namespace

// A direction without context takeover produces the same wire bytes whether
// it borrows the shared codecs or owns a per-connection codec, and with the
// shared codecs the connection's resource never holds codec state.
