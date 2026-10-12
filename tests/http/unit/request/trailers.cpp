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
        RUVIA_CHECK((trailers.append("X-Checksum", source_value).index() == 0));
        source_value.assign("mutated");
        RUVIA_CHECK_EQ(trailers.field("x-checksum").value_or(""), std::string(512, 'a'));
        RUVIA_CHECK((trailers.append_http1("x-checksum: final\r\nx-extra: yes\r\n\r\n").index() == 0));
        RUVIA_CHECK_EQ(trailers.field("X-Checksum").value_or(""), "final");
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
        RUVIA_CHECK((trailers.append("Content-Length", "3").index() != 0));
        RUVIA_CHECK((trailers.append("bad name", "x").index() != 0));
        RUVIA_CHECK((trailers.append("x-ok", "bad\r\nvalue").index() != 0));
        RUVIA_CHECK((trailers.append("x-large", std::string(65536, 'x')).index() != 0));
        RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{3});
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(request_trailers_append_http1_is_atomic_and_classifies_forbidden_fields) {
    ruvia::http_request_trailers trailers(std::pmr::new_delete_resource());
    RUVIA_CHECK((trailers.append("X-First", "1").index() == 0));

    const auto forbidden = trailers.append_http1("X-Second: 2\r\nContent-Length: 3\r\n\r\n");
    RUVIA_CHECK(forbidden.index() == 1);
    if (forbidden.index() == 1) {
        RUVIA_CHECK(std::get<1>(forbidden) == ruvia::http_request_trailer_error::forbidden_field);
    }
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{1});
    RUVIA_CHECK(!trailers.field("X-Second").has_value());

    const auto invalid = trailers.append_http1("X-Second: 2\r\nbad line\r\n\r\n");
    RUVIA_CHECK(invalid.index() == 1);
    if (invalid.index() == 1) {
        RUVIA_CHECK(std::get<1>(invalid) == ruvia::http_request_trailer_error::invalid_field);
    }
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{1});

    // A block that would exceed the remaining field budget leaves no partial fields behind.
    std::string block;
    for (std::size_t index = 0; index < ruvia::max_http_header_fields; ++index) {
        block += "X-Fill: v\r\n";
    }
    const auto too_large = trailers.append_http1(block);
    RUVIA_CHECK(too_large.index() == 1);
    if (too_large.index() == 1) {
        RUVIA_CHECK(std::get<1>(too_large) == ruvia::http_request_trailer_error::section_too_large);
    }
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{1});
    // X-First used 12 normalized bytes and the terminating empty line 2; the
    // remaining budget is intact only if the failed block rolled back.
    RUVIA_CHECK((trailers.append("X-Last", std::string(ruvia::max_http_header_bytes - 24, 'z')).index() == 0));
}

RUVIA_TEST(request_trailers_enforce_normalized_section_limit_during_chunk_decoding) {
    for (const std::size_t field_count : {std::size_t{3}, ruvia::max_http_header_fields}) {
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
                    RUVIA_CHECK(trailers.append_http1(complete->trailers()).index() == 0);
                    RUVIA_CHECK_EQ(trailers.fields().size(), field_count);
                    RUVIA_CHECK_EQ(trailers.field("X").value_or(""), std::string(value_size, 'z'));
                }
            } else {
                RUVIA_CHECK(decoded.failure() != nullptr);
                if (const auto* failure = decoded.failure()) {
                    RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
                }
                const auto appended = trailers.append_http1(block);
                RUVIA_CHECK(appended.index() == 1);
                if (appended.index() == 1) {
                    RUVIA_CHECK(std::get<1>(appended) == ruvia::http_request_trailer_error::section_too_large);
                }
            }
        }
    }
}

RUVIA_TEST(chunk_trailer_encoded_limit_is_independent_of_input_fragmentation) {
    for (const auto role : {ruvia::http1_chunk_trailer_role::request, ruvia::http1_chunk_trailer_role::response}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}, std::size_t{4}}) {
            std::string wire = "0\r\nX:";
            wire.append(ruvia::max_http_header_bytes - 7 + excess, ' ');
            wire.append("x\r\n\r\n");
            ruvia::http1_chunked_body_decoder full({.trailer_role_ = role});
            const auto decoded = full.decode(wire);
            ruvia::http1_chunked_body_decoder fragmented({.trailer_role_ = role});
            const auto prefix = std::string_view(wire).substr(0, wire.size() - 1);
            const auto partial = fragmented.decode(prefix);
            if (excess == 0) {
                RUVIA_CHECK(decoded.complete() != nullptr);
                RUVIA_CHECK(partial.need_more() != nullptr);
                const auto completed = fragmented.decode(std::string_view(wire).substr(partial.consumed_bytes()));
                RUVIA_CHECK(completed.complete() != nullptr);
                if (const auto* complete = completed.complete()) {
                    RUVIA_CHECK_EQ(complete->trailers(), std::string_view(wire).substr(3, wire.size() - 7));
                }
            } else {
                RUVIA_CHECK(decoded.failure() != nullptr);
                if (const auto* failure = decoded.failure()) {
                    RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::invalid_framing);
                }
                RUVIA_CHECK(partial.failure() != nullptr);
                if (const auto* failure = partial.failure()) {
                    RUVIA_CHECK(failure->error() == ruvia::http1_chunk_decode_error::framing_limit_exceeded);
                }
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
        RUVIA_CHECK((received.append("X-A", first_value).index() == 0));
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
            RUVIA_CHECK(appended.index() == 0);
            RUVIA_CHECK(!response_rejected);
        } else {
            RUVIA_CHECK(std::get_if<ruvia::http_request_trailer_error>(&appended) != nullptr &&
                        *std::get_if<ruvia::http_request_trailer_error>(&appended) ==
                            ruvia::http_request_trailer_error::section_too_large);
            RUVIA_CHECK(response_rejected);
        }
    }
}

RUVIA_TEST(largest_accepted_response_trailer_section_decodes) {
    // One field whose encoded section "X: <value>\r\n\r\n" is exactly the limit
    // is accepted for sending and decoded by a response-role chunk decoder;
    // one more value byte is rejected before anything is emitted.
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
        ruvia::http1_chunked_body_decoder decoder({.trailer_role_ = ruvia::http1_chunk_trailer_role::response});
        const auto decoded = decoder.decode(wire);
        RUVIA_CHECK(decoded.complete() != nullptr);
        if (const auto* complete = decoded.complete()) {
            RUVIA_CHECK_EQ(complete->trailers(), std::string_view(wire).substr(3, wire.size() - 7));
        }
    }
}
