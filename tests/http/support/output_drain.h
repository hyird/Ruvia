#pragma once

#include <cstddef>
#include <deque>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>

#include "ruvia/http/http2_connection.h"

namespace ruvia::testing {

// Replaces `into` with every pending outbound frame through the public batch
// drain. A single unbounded batch moves the whole buffer; on copy failure the
// connection keeps its bytes and the exception propagates.
inline void drain_output(ruvia::http2_connection& connection, std::pmr::string& into) {
    into.clear();
    (void)connection.take_output_batch(std::numeric_limits<std::size_t>::max(), into);
}

// Collects every pending outbound frame into an owning string so a test can
// feed, parse or compare the wire bytes.
inline std::string drain_output(ruvia::http2_connection& connection) {
    std::pmr::string batch;
    drain_output(connection, batch);
    return std::string(batch.data(), batch.size());
}

// Moves bytes between two connections. feed() input must outlive the events it
// produces, so every drained string is retained for the relay's lifetime.
class output_relay final {
public:
    [[nodiscard]] ruvia::http2_feed_result feed(
        ruvia::http2_connection& from, ruvia::http2_connection& to) {
        return to.feed(retained_.emplace_back(drain_output(from)));
    }

    // need_input is the normal outcome when the drained bytes contain no
    // complete event.
    void transfer(ruvia::http2_connection& from, ruvia::http2_connection& to) {
        const auto status = feed(from, to);
        if (status != ruvia::http2_feed_result::accepted &&
            status != ruvia::http2_feed_result::need_input) {
            throw std::runtime_error(
                "HTTP/2 transfer failed, status " + std::to_string(static_cast<int>(status)));
        }
    }

private:
    std::deque<std::string> retained_;
};

}  // namespace ruvia::testing
