#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_chunked_body_decoder.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/http_response_server.h"

#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

RUVIA_TEST(request_trailers_own_fields_preserve_duplicates_and_bound_section_size) {
    counting_resource resource;
    {
        ruvia::http_request_trailers trailers(&resource);
        std::string source_value(512, 'a');
        RUVIA_CHECK(!trailers.append("X-Checksum", source_value).has_value());
        source_value.assign("mutated");
        RUVIA_CHECK_EQ(trailers.field("x-checksum").value_or(""), std::string(512, 'a'));
        RUVIA_CHECK(!trailers.append_http1("x-checksum: final\r\nx-extra: yes\r\n\r\n").has_value());
        RUVIA_CHECK_EQ(trailers.field("X-Checksum").value_or(""), "final");
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
        const auto allocations_before_rejected_batches = resource.allocations_;
        const std::array forbidden_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"Content-Length", "3"},
        };
        RUVIA_CHECK(trailers.append(forbidden_batch) == ruvia::http_request_trailer_error::forbidden_field);
        const std::array invalid_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"x-ok", "bad\r\nvalue"},
        };
        RUVIA_CHECK(trailers.append(invalid_batch) == ruvia::http_request_trailer_error::invalid_field);
        const std::string large_value(65536, 'x');
        const std::array oversized_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"x-large", large_value},
        };
        RUVIA_CHECK(trailers.append(oversized_batch) == ruvia::http_request_trailer_error::section_too_large);
        RUVIA_CHECK_EQ(resource.allocations_, allocations_before_rejected_batches);
        RUVIA_CHECK(trailers.append("Content-Length", "3").has_value());
        RUVIA_CHECK(trailers.append("bad name", "x").has_value());
        RUVIA_CHECK(trailers.append("x-ok", "bad\r\nvalue").has_value());
        RUVIA_CHECK(trailers.append("x-large", large_value).has_value());
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
        const std::array accepted_batch{
            ruvia::http_header_view{"X-Batch-One", "1"},
            ruvia::http_header_view{"X-Batch-Two", "2"},
            ruvia::http_header_view{"X-Batch-Three", "3"},
        };
        RUVIA_CHECK(!trailers.append(accepted_batch).has_value());
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{6});
        RUVIA_CHECK_EQ(trailers.field("x-batch-three").value_or(""), "3");
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(request_trailers_append_http1_is_atomic_and_discards_fields_not_permitted_as_trailers) {
    ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
    RUVIA_CHECK(!trailers.append("X-First", "1").has_value());
    RUVIA_CHECK(trailers.append("Content-Length", "3") == ruvia::http_request_trailer_error::forbidden_field);

    // RFC 9112 section 7.1.2: a recipient may discard received trailer fields;
    // the sender-only prohibition of RFC 9110 section 6.5.1 does not fail the block.
    RUVIA_CHECK(!trailers.append_http1("X-Second: 2\r\nContent-Length: 3\r\nCache-Control: no-cache\r\n\r\n").has_value());
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});
    RUVIA_CHECK_EQ(trailers.field("X-Second").value_or(""), "2");
    RUVIA_CHECK(!trailers.field("Content-Length").has_value());
    RUVIA_CHECK(!trailers.field("Cache-Control").has_value());

    const auto invalid = trailers.append_http1("X-Third: 3\r\nbad line\r\n\r\n");
    RUVIA_CHECK(invalid == ruvia::http_request_trailer_error::invalid_field);
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});

    // A block that would exceed the remaining byte budget leaves no partial fields behind.
    std::string block = "X-Fill: v\r\nX-Oversize: ";
    block.append(ruvia::max_http_header_bytes, 'x');
    block.append("\r\n");
    const auto too_large = trailers.append_http1(block);
    RUVIA_CHECK(too_large == ruvia::http_request_trailer_error::section_too_large);
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});
    // X-First used 12 normalized bytes, X-Second 13, the discarded received
    // fields 19 + 25 and the terminating empty line 2; the remaining budget is
    // intact only if the failed block rolled back.
    RUVIA_CHECK(!trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 81, 'z')).has_value());
}

RUVIA_TEST(request_trailers_enforce_normalized_section_limit_during_chunk_decoding) {
    for (const std::size_t field_count : {std::size_t{3}, std::size_t{80}}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
            // Each "X:a" field costs 6 normalized bytes and the last field 5 plus
            // its value; the section also pays 2 for its terminating empty line.
            const auto value_size = ruvia::max_http_header_bytes - 6 * (field_count - 1) - 5 - 2 + excess;
            std::string block;
            for (std::size_t index = 1; index < field_count; ++index) {
                block.append("X:a\r\n");
            }
            block.append("X:");
            block.append(value_size, 'z');
            const std::string wire = "0\r\n" + block + "\r\n\r\n";
            ruvia::http1_chunked_body_decoder decoder;
            const auto decoded = decoder.decode(wire);
            ruvia::http_request_trailers trailers(nullptr);
            if (excess == 0) {
                RUVIA_CHECK(decoded.complete() != nullptr);
                if (const auto* complete = decoded.complete()) {
                    RUVIA_CHECK(!trailers.append_http1(complete->trailers()).has_value());
                    RUVIA_CHECK_EQ(trailers.fields().size(), field_count);
                    RUVIA_CHECK_EQ(trailers.field("X").value_or(""), std::string(value_size, 'z'));
                }
            } else {
                RUVIA_CHECK(decoded.failure() != nullptr);
                if (const auto* failure = decoded.failure()) {
                    RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
                }
                const auto appended = trailers.append_http1(block);
                RUVIA_CHECK(appended == ruvia::http_request_trailer_error::section_too_large);
            }
        }
    }
}

RUVIA_TEST(request_and_response_trailer_sections_share_one_byte_rule) {
    // Two fields named "X-A": each costs name + value + 4 normalized bytes, and
    // the section pays 2 for its terminating empty line.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        const auto first_value = std::string(1000, 'a');
        const auto second_value =
            std::string(ruvia::max_http_header_bytes - 2 * (3 + 4) - 2 - first_value.size() + excess, 'b');
        ruvia::http_request_trailers received(std::pmr::new_delete_resource());
        RUVIA_CHECK(!received.append("X-A", first_value).has_value());
        const auto appended = received.append("X-A", second_value);
        const std::array forwarded{ruvia::http_header_view{"X-A", first_value},
            ruvia::http_header_view{"X-A", second_value}};
        bool response_rejected = false;
        try {
            (void)ruvia::validate_http_response_trailers(forwarded);
        } catch (const std::exception&) {
            response_rejected = true;
        }
        if (excess == 0) {
            // A received request section can always be forwarded as response trailers.
            RUVIA_CHECK(!appended.has_value());
            RUVIA_CHECK(!response_rejected);
        } else {
            RUVIA_CHECK(appended == ruvia::http_request_trailer_error::section_too_large);
            RUVIA_CHECK(response_rejected);
        }
    }
}

RUVIA_TEST(largest_accepted_response_trailer_section_decodes) {
    // One field whose encoded section "X: <value>\r\n\r\n" is exactly the limit
    // is accepted for sending and decoded by a chunk decoder; one more value
    // byte is rejected before anything is emitted.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        const std::string value(ruvia::max_http_header_bytes - 1 - 4 - 2 + excess, 'v');
        const std::array trailers{ruvia::http_header_view{"X", value}};
        bool rejected = false;
        try {
            (void)ruvia::validate_http_response_trailers(trailers);
        } catch (const std::exception&) {
            rejected = true;
        }
        RUVIA_CHECK_EQ(rejected, excess != 0);
        if (excess != 0) {
            continue;
        }
        const std::string wire = "0\r\nX: " + value + "\r\n\r\n";
        ruvia::http1_chunked_body_decoder decoder;
        const auto decoded = decoder.decode(wire);
        RUVIA_CHECK(decoded.complete() != nullptr);
        if (const auto* complete = decoded.complete()) {
            RUVIA_CHECK_EQ(complete->trailers(), std::string_view(wire).substr(3, wire.size() - 7));
        }
    }
}

RUVIA_TEST(received_response_trailers_discard_fields_not_permitted_as_trailers) {
    std::string visited;
    const auto visit = [&visited](std::string_view name, std::string_view value) {
        visited.append(name).append("=").append(value).append(";");
        return true;
    };
    RUVIA_CHECK(ruvia::visit_http_response_trailers(
        "Content-Type: text/plain\r\nX-Sum: abc\r\nContent-Length: 5\r\nETag: \"v\"", visit));
    RUVIA_CHECK_EQ(visited, "X-Sum=abc;ETag=\"v\";");

    visited.clear();
    RUVIA_CHECK(!ruvia::visit_http_response_trailers("Content-Type: text/plain\r\nbad line", visit));
}
