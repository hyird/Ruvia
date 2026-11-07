#include <gtest/gtest.h>

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

TEST(Ruvia, request_trailers_own_fields_preserve_duplicates_and_bound_section_size) {
    counting_resource resource;
    {
        ruvia::http_request_trailers trailers(&resource);
        std::string source_value(512, 'a');
        EXPECT_TRUE(!trailers.append("X-Checksum", source_value).has_value());
        source_value.assign("mutated");
        EXPECT_EQ(trailers.field("x-checksum").value_or(""), std::string(512, 'a'));
        EXPECT_TRUE(!trailers.append_http1("x-checksum: final\r\nx-extra: yes\r\n\r\n").has_value());
        EXPECT_EQ(trailers.field("X-Checksum").value_or(""), "final");
        EXPECT_EQ(trailers.fields().size(), std::size_t{3});
        const auto allocations_before_rejected_batches = resource.allocations_;
        const std::array forbidden_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"Content-Length", "3"},
        };
        EXPECT_TRUE(trailers.append(forbidden_batch) == ruvia::http_request_trailer_error::forbidden_field);
        const std::array invalid_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"x-ok", "bad\r\nvalue"},
        };
        EXPECT_TRUE(trailers.append(invalid_batch) == ruvia::http_request_trailer_error::invalid_field);
        const std::string large_value(65536, 'x');
        const std::array oversized_batch{
            ruvia::http_header_view{"X-Batch", "1"},
            ruvia::http_header_view{"x-large", large_value},
        };
        EXPECT_TRUE(trailers.append(oversized_batch) == ruvia::http_request_trailer_error::section_too_large);
        EXPECT_EQ(resource.allocations_, allocations_before_rejected_batches);
        EXPECT_TRUE(trailers.append("Content-Length", "3").has_value());
        EXPECT_TRUE(trailers.append("bad name", "x").has_value());
        EXPECT_TRUE(trailers.append("x-ok", "bad\r\nvalue").has_value());
        EXPECT_TRUE(trailers.append("x-large", large_value).has_value());
        EXPECT_EQ(trailers.fields().size(), std::size_t{3});
        const std::array accepted_batch{
            ruvia::http_header_view{"X-Batch-One", "1"},
            ruvia::http_header_view{"X-Batch-Two", "2"},
            ruvia::http_header_view{"X-Batch-Three", "3"},
        };
        EXPECT_TRUE(!trailers.append(accepted_batch).has_value());
        EXPECT_EQ(trailers.fields().size(), std::size_t{6});
        EXPECT_EQ(trailers.field("x-batch-three").value_or(""), "3");
    }
    EXPECT_EQ(resource.allocations_, resource.deallocations_);
}

TEST(Ruvia, request_trailers_append_http1_is_atomic_and_discards_fields_not_permitted_as_trailers) {
    ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
    EXPECT_TRUE(!trailers.append("X-First", "1").has_value());
    EXPECT_TRUE(trailers.append("Content-Length", "3") == ruvia::http_request_trailer_error::forbidden_field);

    // RFC 9112 section 7.1.2: a recipient may discard received trailer fields;
    // the sender-only prohibition of RFC 9110 section 6.5.1 does not fail the block.
    EXPECT_TRUE(!trailers.append_http1("X-Second: 2\r\nContent-Length: 3\r\nCache-Control: no-cache\r\n\r\n").has_value());
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_EQ(trailers.field("X-Second").value_or(""), "2");
    EXPECT_TRUE(!trailers.field("Content-Length").has_value());
    EXPECT_TRUE(!trailers.field("Cache-Control").has_value());

    const auto invalid = trailers.append_http1("X-Third: 3\r\nbad line\r\n\r\n");
    EXPECT_TRUE(invalid == ruvia::http_request_trailer_error::invalid_field);
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});

    // A block that would exceed the remaining byte budget leaves no partial fields behind.
    std::string block = "X-Fill: v\r\nX-Oversize: ";
    block.append(ruvia::max_http_header_bytes, 'x');
    block.append("\r\n");
    const auto too_large = trailers.append_http1(block);
    EXPECT_TRUE(too_large == ruvia::http_request_trailer_error::section_too_large);
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    // X-First used 12 normalized bytes, X-Second 13, the discarded received
    // fields 19 + 25 and the terminating empty line 2; the remaining budget is
    // intact only if the failed block rolled back.
    EXPECT_TRUE(!trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 81, 'z')).has_value());
}

TEST(Ruvia, request_trailers_append_http1_retains_lowercase_names) {
    ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
    EXPECT_TRUE(!trailers.append("X-Appended", "1").has_value());
    EXPECT_TRUE(!trailers.append_http1("X-Mixed-Case: 2\r\n\r\n").has_value());
    const auto fields = trailers.fields();
    EXPECT_EQ(fields.size(), std::size_t{2});
    if (fields.size() == 2) {
        EXPECT_EQ(fields[0].name(), std::string_view("x-appended"));
        EXPECT_EQ(fields[1].name(), std::string_view("x-mixed-case"));
        EXPECT_EQ(fields[1].value(), std::string_view("2"));
    }
}

TEST(Ruvia, request_trailers_cap_field_count) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        std::string block;
        for (std::size_t index = 0; index < ruvia::max_http_header_fields + excess; ++index) {
            block.append("X:a\r\n");
        }
        const std::string wire = "0\r\n" + block + "\r\n";
        ruvia::http1_chunked_body_decoder decoder;
        const auto decoded = decoder.decode(wire);
        ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
        const auto appended = trailers.append_http1(block);
        if (excess == 0) {
            EXPECT_TRUE(decoded.complete() != nullptr);
            EXPECT_TRUE(!appended.has_value());
            EXPECT_EQ(trailers.fields().size(), ruvia::max_http_header_fields);
            // A full section admits no further field through either entry.
            EXPECT_TRUE(trailers.append("X", "b") == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_TRUE(trailers.append_http1("X:b\r\n") == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_EQ(trailers.fields().size(), ruvia::max_http_header_fields);
        } else {
            EXPECT_TRUE(decoded.failure() != nullptr);
            if (const auto* failure = decoded.failure()) {
                EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
            }
            EXPECT_TRUE(appended == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_TRUE(trailers.fields().empty());
        }
    }
}

TEST(Ruvia, request_trailers_enforce_normalized_section_limit_during_chunk_decoding) {
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
                EXPECT_TRUE(decoded.complete() != nullptr);
                if (const auto* complete = decoded.complete()) {
                    EXPECT_TRUE(!trailers.append_http1(complete->trailers()).has_value());
                    EXPECT_EQ(trailers.fields().size(), field_count);
                    EXPECT_EQ(trailers.field("X").value_or(""), std::string(value_size, 'z'));
                }
            } else {
                EXPECT_TRUE(decoded.failure() != nullptr);
                if (const auto* failure = decoded.failure()) {
                    EXPECT_TRUE(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
                }
                const auto appended = trailers.append_http1(block);
                EXPECT_TRUE(appended == ruvia::http_request_trailer_error::section_too_large);
            }
        }
    }
}

TEST(Ruvia, request_and_response_trailer_sections_share_one_byte_rule) {
    // Two fields named "X-A": each costs name + value + 4 normalized bytes, and
    // the section pays 2 for its terminating empty line.
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        const auto first_value = std::string(1000, 'a');
        const auto second_value =
            std::string(ruvia::max_http_header_bytes - 2 * (3 + 4) - 2 - first_value.size() + excess, 'b');
        ruvia::http_request_trailers received(std::pmr::new_delete_resource());
        EXPECT_TRUE(!received.append("X-A", first_value).has_value());
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
            EXPECT_TRUE(!appended.has_value());
            EXPECT_TRUE(!response_rejected);
        } else {
            EXPECT_TRUE(appended == ruvia::http_request_trailer_error::section_too_large);
            EXPECT_TRUE(response_rejected);
        }
    }
}

TEST(Ruvia, largest_accepted_response_trailer_section_decodes) {
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
        EXPECT_EQ(rejected, excess != 0);
        if (excess != 0) {
            continue;
        }
        const std::string wire = "0\r\nX: " + value + "\r\n\r\n";
        ruvia::http1_chunked_body_decoder decoder;
        const auto decoded = decoder.decode(wire);
        EXPECT_TRUE(decoded.complete() != nullptr);
        if (const auto* complete = decoded.complete()) {
            EXPECT_EQ(complete->trailers(), std::string_view(wire).substr(3, wire.size() - 7));
        }
    }
}

TEST(Ruvia, received_response_trailers_discard_fields_not_permitted_as_trailers) {
    std::string visited;
    const auto visit = [&visited](std::string_view name, std::string_view value) {
        visited.append(name).append("=").append(value).append(";");
        return true;
    };
    EXPECT_TRUE(ruvia::visit_http_response_trailers(
        "Content-Type: text/plain\r\nX-Sum: abc\r\nContent-Length: 5\r\nETag: \"v\"", visit));
    EXPECT_EQ(visited, "X-Sum=abc;ETag=\"v\";");

    visited.clear();
    EXPECT_TRUE(!ruvia::visit_http_response_trailers("Content-Type: text/plain\r\nbad line", visit));
}

TEST(Ruvia, request_trailers_append_handles_self_aliasing_across_reallocation) {
    ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
    EXPECT_TRUE(!trailers.append("X-One", "first-value").has_value());
    EXPECT_TRUE(!trailers.append("X-Two", "second-value").has_value());
    // Appending the container's own span across capacity growth must not read
    // from invalidated elements.
    for (int round = 0; round < 3; ++round) {
        EXPECT_TRUE(!trailers.append(trailers.fields()).has_value());
    }
    EXPECT_EQ(trailers.fields().size(), std::size_t{16});
    EXPECT_EQ(trailers.fields()[0].name(), "x-one");
    EXPECT_EQ(trailers.fields()[0].value(), "first-value");
    EXPECT_EQ(trailers.fields()[15].name(), "x-two");
    EXPECT_EQ(trailers.fields()[15].value(), "second-value");
}
