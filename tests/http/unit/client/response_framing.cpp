#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "ruvia/http/http1_client_response_body_decoder.h"
#include "ruvia/http/http_header.h"

#include "http_client_response_fixture.h"

using namespace ruvia::test;

// HTTP/1 client responses: what the head says about the body.

namespace {

}  // namespace

TEST(Ruvia, http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parse_head("GET", "HTTP/1.1 103 Early Hints");
    const auto without_content = parse_head("GET", "HTTP/1.1 204 No Content");
    const auto known_length = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto close_delimited = parse_head("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parse_head("CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, upgrade_headers);

    for (const auto* plan : {&informational.plan(), &without_content.plan(), &known_length.plan(),
             &chunked.plan(), &close_delimited.plan(), &tunnel.plan(), &upgrade.plan()}) {
        EXPECT_EQ(active_plan_alternative_count(*plan), std::size_t{1});
    }
}

TEST(Ruvia, http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header_value = "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parse_head("GET", header_value);
    const auto& known_length = require_known_length(head.plan());
    EXPECT_EQ(known_length.content_length(), std::size_t{5});
    EXPECT_TRUE(known_length.requires_body_consumption());
    EXPECT_TRUE(known_length.persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(head.consumed_bytes(), header_value.size() + 4);

    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    EXPECT_TRUE(!require_known_length(empty.plan()).requires_body_consumption());
}

TEST(Ruvia, http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    EXPECT_EQ(require_known_length(combined.plan()).content_length(), std::size_t{5});

    const auto repeated =
        parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(repeated.plan()).content_length(), std::size_t{5});

    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

TEST(Ruvia, http_client_response_unfolds_fields_before_framing_and_owns_values) {
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    std::string wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0,\r\n \t0\r\n"
        "X-Trace: one \t\r\n\t two\r\n\r\n";
    for (std::size_t size = 1; size < wire.size(); ++size) {
        const auto partial = parser.parse(std::string_view(wire).substr(0, size));
        EXPECT_TRUE(partial.need_more() != nullptr);
    }

    const auto result_value = parser.parse(wire);
    const auto* parsed = result_value.parsed();
    EXPECT_TRUE(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    EXPECT_EQ(parsed->consumed_bytes(), wire.size());
    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{0});
    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    EXPECT_EQ(headers.size(), std::size_t{2});
    if (headers.size() == 2) {
        EXPECT_EQ(headers[0].name(), "Content-Length");
        EXPECT_EQ(headers[0].value(), "0, 0");
        EXPECT_EQ(headers[1].name(), "X-Trace");
        EXPECT_EQ(headers[1].value(), "one two");
    }
}

TEST(Ruvia, http_client_response_unfolding_preserves_framing_validation) {
    const auto transfer = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip,\r\n\tchunked");
    const auto& chunked = require_chunked(transfer.plan());
    EXPECT_EQ(chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_EQ(transfer.head().headers().front().value(), "gzip, chunked");

    const std::pair<std::string_view, http1_client_response_parse_error> invalid[] = {
        {"Content-Length: 4\r\n 2", http1_client_response_parse_error::invalid_content_length},
        {"Content-Length: 4,\r\n 2", http1_client_response_parse_error::conflicting_content_length},
        {"Transfer-Encoding: chunked,\r\n gzip", http1_client_response_parse_error::invalid_transfer_encoding},
        {"Content-Length: 0\r\nTransfer-Encoding: gzip,\r\n chunked",
            http1_client_response_parse_error::content_length_and_transfer_encoding},
        {" Content-Length: 0", http1_client_response_parse_error::invalid_header},
        {"\tContent-Length: 0\r\nX-Trace: one\r\n two", http1_client_response_parse_error::invalid_header},
    };
    for (const auto& [fields, expected_error] : invalid) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields);
        EXPECT_TRUE(parse_failure_error("GET", head) == expected_error);
    }
}

TEST(Ruvia, http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = require_chunked(head.plan());
    EXPECT_TRUE(chunked.transfer_codings().empty());
    EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combined_chunked = require_chunked(combined.plan());
    EXPECT_EQ(combined_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(combined_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(combined_chunked.persistence() == http1_close_policy::allow_reuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& split_chunked = require_chunked(split.plan());
    EXPECT_EQ(split_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(split_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::deflate);

    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
        "Transfer-Encoding: deflate, chunked");
    const auto& repeated_chunked = require_chunked(repeated.plan());
    EXPECT_EQ(repeated_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(repeated_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(repeated_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_transfer_encoding_ignores_empty_recipient_list_items) {
    for (const std::string_view fields : {
             "Transfer-Encoding: , chunked",
             "Transfer-Encoding: chunked,",
             "Transfer-Encoding: , , chunked, ,",
             "Transfer-Encoding: gzip, , chunked",
             "Transfer-Encoding: \r\nTransfer-Encoding: chunked",
             "Transfer-Encoding: chunked\r\nTransfer-Encoding: , ,",
             "Transfer-Encoding: ,\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: \r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: ,",
         }) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n\r\n";
        const std::string wire = head + "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        const auto& chunked = require_chunked(parsed->plan());
        EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
        const bool gzip = fields.find("gzip") != std::string_view::npos;
        EXPECT_EQ(chunked.transfer_codings().size(), gzip ? std::size_t{1} : std::size_t{0});
        if (gzip && !chunked.transfer_codings().empty()) {
            EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
        }
        EXPECT_EQ(parsed->consumed_bytes(), head.size());
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n");
    }

    const auto non_chunked = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: , gzip, ,\r\nTransfer-Encoding: ");
    const auto& close_delimited = require_close_delimited(non_chunked.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(close_delimited.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);

    // Empty items do not make the field absent or relax framing conflicts.
    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,");
    EXPECT_TRUE(require_close_delimited(empty.plan()).transfer_codings().empty());
    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,\r\nContent-Length: 0") ==
                http1_client_response_parse_error::content_length_and_transfer_encoding);
}

TEST(Ruvia, http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& close_delimited = require_close_delimited(head.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
}

TEST(Ruvia, http_client_rejects_invalid_or_unsupported_transfer_coding) {
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    const auto malformed_after_unknown = parse_result("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress\r\n"
        "Transfer-Encoding: deflate, chunked, gzip");
    EXPECT_TRUE(malformed_after_unknown.failure() != nullptr);
    if (const auto* failure = malformed_after_unknown.failure()) {
        EXPECT_TRUE(*failure == http1_client_response_parse_error::invalid_transfer_encoding);
    }
    const auto stacked = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked");
    const auto& stacked_chunked = require_chunked(stacked.plan());
    EXPECT_EQ(stacked_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(stacked_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(stacked_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_content_length_and_transfer_encoding_rejected_for_body) {
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

TEST(Ruvia, http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parse_head("HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& without_content = require_without_content(head.plan());
    EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);

    const auto not_modified = parse_head("GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    EXPECT_TRUE(not_modified.plan().without_content() != nullptr);

    const auto no_content = parse_head("GET", "HTTP/1.1 204 No Content");
    EXPECT_TRUE(no_content.plan().without_content() != nullptr);
}

TEST(Ruvia, http_client_trailer_declaration_metadata_preserves_framed_body_and_next_response) {
    for (const std::string_view declaration :
        {"Content-Length", "Host", "Bad Field", "\"x-name\"", ",", "X-One,,X-Two"}) {
        for (const bool chunked : {false, true}) {
            std::string head = "HTTP/1.1 200 OK\r\nTrailer: \t";
            head.append(declaration);
            head.append(" \t\r\n");
            head.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n" : "Content-Length: 3\r\n\r\n");
            const std::string wire = head + (chunked ? "3\r\nabc\r\n0\r\n\r\n" : "abc") +
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            const auto result = parse_wire("GET", wire);
            EXPECT_TRUE(result.parsed() != nullptr);
            if (const auto* parsed = result.parsed()) {
                EXPECT_EQ(parsed->head().headers().front().value(), declaration);
                EXPECT_EQ(parsed->consumed_bytes(), head.size());
                if (chunked) {
                    EXPECT_TRUE(parsed->plan().chunked() != nullptr);
                } else {
                    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
                }
                ruvia::http1_client_response_body_decoder decoder(
                    parsed->plan());
                std::array<char, 8> scratch{};
                auto remaining = std::string_view(wire).substr(parsed->consumed_bytes());
                std::string body;
                bool complete = false;
                for (int step = 0; step < 8; ++step) {
                    const auto decoded = decoder.decode(remaining, scratch);
                    EXPECT_TRUE(decoded.protocol_failure() == nullptr);
                    EXPECT_TRUE(decoded.decoder_failure() == nullptr);
                    remaining.remove_prefix(decoded.consumed_bytes());
                    if (const auto* output = decoded.output()) {
                        body.append(output->bytes());
                    }
                    if (decoded.complete()) {
                        complete = true;
                        break;
                    }
                }
                EXPECT_TRUE(complete);
                EXPECT_EQ(body, "abc");
                const auto next = parse_wire("GET", remaining);
                EXPECT_TRUE(next.parsed() != nullptr);
                if (const auto* next_head = next.parsed()) {
                    EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                    EXPECT_EQ(remaining.substr(next_head->consumed_bytes()), "ok");
                }
            }
        }
    }
}

TEST(Ruvia, http_client_bodyless_trailer_declarations_preserve_message_boundaries) {
    for (const auto& [method, status_line] : {
             std::pair{std::string_view("HEAD"), std::string_view("HTTP/1.1 200 OK")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 304 Not Modified")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 204 No Content")},
         }) {
        ruvia::http_client_request_view request;
        request.method_ = method;
        for (const bool transfer_encoding : {false, true}) {
            std::string head(status_line);
            head.append("\r\n");
            if (transfer_encoding) {
                head.append("Transfer-Encoding: chunked\r\n");
            }
            // RFC 9110 section 6.6.2 makes Trailer a declaration, not a promise
            // that trailer fields follow. RFC 9112 section 6.3 ends both of
            // these responses at the empty line regardless of their fields.
            head.append("Trailer: Content-Length, Bad Field, \"x-name\"\r\n\r\n");
            const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            http1_client_response_parser parser =
                exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
            for (std::size_t size = 1; size < head.size(); ++size) {
                const auto partial = parser.parse(std::string_view(wire).substr(0, size));
                EXPECT_TRUE(partial.need_more() != nullptr);
            }
            const auto result = parser.parse(wire);
            const auto* parsed = result.parsed();
            EXPECT_TRUE(parsed != nullptr);
            if (parsed == nullptr) {
                continue;
            }
            EXPECT_TRUE(parsed->plan().without_content() != nullptr);
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().back().name(), "Trailer");
            EXPECT_EQ(parsed->head().headers().back().value(), "Content-Length, Bad Field, \"x-name\"");
            const auto following = std::string_view(wire).substr(parsed->consumed_bytes());
            const auto next = parse_wire("GET", following);
            EXPECT_TRUE(next.parsed() != nullptr);
            if (const auto* next_head = next.parsed()) {
                EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                EXPECT_EQ(following.substr(next_head->consumed_bytes()), "ok");
            }
        }
    }

    constexpr std::string_view tunnel_wire =
        "HTTP/1.1 200 Connection Established\r\nTrailer: Content-Length, Bad Field\r\n\r\ntunnel bytes";
    const auto tunnel = parse_wire("CONNECT", tunnel_wire);
    EXPECT_TRUE(tunnel.parsed() != nullptr);
    if (const auto* parsed = tunnel.parsed()) {
        EXPECT_TRUE(parsed->plan().connect_tunnel() != nullptr);
        EXPECT_EQ(tunnel_wire.substr(parsed->consumed_bytes()), "tunnel bytes");
    }

    const ruvia::http_header_view upgrade_headers[] = {{"Connection", "Upgrade"}, {"Upgrade", "websocket"}};
    constexpr std::string_view upgrade_wire =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Trailer: X-Checksum\r\n\r\nprotocol bytes";
    const auto upgrade = parse_wire("GET", upgrade_wire, http1_close_policy::allow_reuse, upgrade_headers);
    EXPECT_TRUE(upgrade.parsed() != nullptr);
    if (const auto* parsed = upgrade.parsed()) {
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(upgrade_wire.substr(parsed->consumed_bytes()), "protocol bytes");
    }
}

TEST(Ruvia, http_client_auxiliary_response_fields_do_not_change_content_framing) {
    constexpr std::string_view auxiliary =
        "Upgrade: websocket\r\nTrailer: X-Checksum\r\nTE: trailers\r\n";
    for (const std::string_view framing : {
             std::string_view("Content-Length: 2\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: gzip\r\n"),
             std::string_view{},
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "body bytes";
        const auto result = parse_wire("GET", wire);
        EXPECT_TRUE(result.parsed() != nullptr);
        if (const auto* parsed = result.parsed()) {
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "body bytes");
            if (framing.starts_with("Content-Length")) {
                const auto& body = require_known_length(parsed->plan());
                EXPECT_EQ(body.content_length(), std::size_t{2});
                EXPECT_TRUE(body.persistence() == http1_close_policy::allow_reuse);
            } else if (framing == "Transfer-Encoding: chunked\r\n") {
                EXPECT_TRUE(require_chunked(parsed->plan()).persistence() == http1_close_policy::allow_reuse);
            } else {
                EXPECT_TRUE(parsed->plan().close_delimited() != nullptr);
            }
        }
    }
    for (const std::string_view malformed : {
             std::string_view("Content-Length: invalid\r\n"),
             std::string_view("Content-Length: 2\r\nTransfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: chunked, gzip\r\n"),
             std::string_view("Transfer-Encoding: custom-coding\r\n"),
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(malformed);
        EXPECT_TRUE(parse_fails("GET", head));
    }
}

TEST(Ruvia, http_client_informational_auxiliary_fields_preserve_the_final_response_boundary) {
    for (const std::string_view status_line :
        {"HTTP/1.1 100 Continue\r\n", "HTTP/1.1 103 Early Hints\r\n"}) {
        ruvia::http_client_request_view request;
        std::string head(status_line);
        head.append(
            "Content-Length: ignored\r\nTransfer-Encoding: ignored\r\n"
            "Trailer: Content-Length, Bad Field\r\nTE: trailers\r\nUpgrade: websocket\r\n"
            "Content-Type: text/plain\r\nContent-Type: application/json\r\n"
            "Server: first\r\nServer: second\r\n\r\n");
        const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        http1_client_response_parser parser =
            exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
        for (std::size_t size = 1; size < head.size(); ++size) {
            const auto partial = parser.parse(std::string_view(wire).substr(0, size));
            EXPECT_TRUE(partial.need_more() != nullptr);
        }
        const auto interim = parser.parse(wire);
        EXPECT_TRUE(interim.parsed() != nullptr);
        if (const auto* parsed = interim.parsed()) {
            EXPECT_TRUE(parsed->plan().informational() != nullptr);
            if (const auto* informational = parsed->plan().informational()) {
                EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
            }
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().size(), std::size_t{9});
            const auto final_response = parser.parse(std::string_view(wire).substr(parsed->consumed_bytes()));
            EXPECT_TRUE(final_response.parsed() != nullptr);
            if (const auto* final_head = final_response.parsed()) {
                EXPECT_EQ(require_known_length(final_head->plan()).content_length(), std::size_t{2});
                EXPECT_TRUE(require_known_length(final_head->plan()).persistence() == http1_close_policy::allow_reuse);
            }
        }
        head.insert(head.size() - 2, "Connection: close\r\n");
        const auto closing = parse_head("GET", head);
        EXPECT_TRUE(closing.plan().informational() != nullptr);
        if (const auto* informational = closing.plan().informational()) {
            // A 1xx cannot close the connection; its final response decides.
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
}

TEST(Ruvia, http_client_no_body_content_length_metadata_does_not_fail_the_response) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid"},
             std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5, 6"}}) {
        const auto parsed = parse_head(method, response);
        EXPECT_TRUE(parsed.plan().without_content() != nullptr);
        EXPECT_FALSE(parsed.head().content_length().has_value());
    }

    const auto repeated =
        parse_head("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_TRUE(repeated.plan().without_content() != nullptr);
    EXPECT_EQ(repeated.head().content_length(), std::optional<std::uint64_t>(5));
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42");
    EXPECT_EQ(head.head().content_length(), std::optional<std::uint64_t>(42));
    const auto invalid_then_valid = parse_head(
        "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\nContent-Length: 42");
    EXPECT_FALSE(invalid_then_valid.head().content_length().has_value());
}

TEST(Ruvia, http_client_bodyless_responses_ignore_framing_fields) {
    // RFC 9112 section 6.3 rule 1: 1xx, 204, 304 and HEAD responses end at the
    // header section whatever Content-Length or Transfer-Encoding they carry.
    for (const std::string_view head_value : {
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 0"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 5"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: invalid"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"),
         }) {
        const auto head = parse_head("GET", head_value);
        const auto& without_content = require_without_content(head.plan());
        EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);
        EXPECT_EQ(head.consumed_bytes(), head_value.size() + 4);
    }
}

TEST(Ruvia, http_client_bodyless_framing_fields_do_not_desynchronize_the_connection) {
    // A smuggled body after a bodyless head is the next response, not content.
    constexpr std::string_view no_content_head =
        "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string wire(no_content_head);
    wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto no_content = parse_wire("GET", wire);
    EXPECT_TRUE(no_content.parsed() != nullptr);
    if (const auto* parsed = no_content.parsed()) {
        EXPECT_TRUE(parsed->plan().without_content() != nullptr);
        EXPECT_EQ(parsed->consumed_bytes(), no_content_head.size());
        // The next exchange starts exactly after the empty line.
        const auto next = parse_wire("GET", std::string_view(wire).substr(parsed->consumed_bytes()));
        EXPECT_TRUE(next.parsed() != nullptr);
        if (const auto* next_head = next.parsed()) {
            EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
        }
    }

    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    constexpr std::string_view interim_head = "HTTP/1.1 103 Early Hints\r\nContent-Length: 5\r\n\r\n";
    std::string interim_wire(interim_head);
    interim_wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto interim = parser.parse(interim_wire);
    EXPECT_TRUE(interim.parsed() != nullptr);
    if (interim.parsed() == nullptr) {
        return;
    }
    EXPECT_TRUE(interim.parsed()->plan().informational() != nullptr);
    EXPECT_EQ(interim.parsed()->consumed_bytes(), interim_head.size());
    const auto final_response =
        parser.parse(std::string_view(interim_wire).substr(interim.parsed()->consumed_bytes()));
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_EQ(require_known_length(final_response.parsed()->plan()).content_length(), std::size_t{2});
    }
}

TEST(Ruvia, http_client_205_owns_zero_content_framing) {
    const auto zero_length = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    EXPECT_TRUE(zero_length.plan().discards_content());
    const auto* zero_length_body = zero_length.plan().known_length();
    EXPECT_TRUE(zero_length_body != nullptr);
    if (zero_length_body == nullptr) {
        return;
    }
    EXPECT_TRUE(!zero_length_body->requires_body_consumption());
    EXPECT_TRUE(zero_length_body->persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(active_plan_alternative_count(zero_length.plan()), std::size_t{1});

    // RFC 9110 section 15.3.6 binds the sender: delimited content is consumed.
    const auto with_content = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3");
    EXPECT_TRUE(with_content.plan().discards_content());
    EXPECT_EQ(require_known_length(with_content.plan()).content_length(), std::size_t{3});
    EXPECT_TRUE(require_known_length(with_content.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto chunked = parse_head("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunked_zero = chunked.plan();
    EXPECT_TRUE(chunked_zero.discards_content());
    EXPECT_TRUE(chunked_zero.chunked() != nullptr);
    EXPECT_TRUE(chunked_zero.close_delimited() == nullptr);
    if (chunked_zero.chunked() != nullptr) {
        EXPECT_EQ(chunked_zero.chunked()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(chunked_zero.chunked()->transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    }
    EXPECT_EQ(active_plan_alternative_count(chunked.plan()), std::size_t{1});

    const auto transfer_coded =
        parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transfer_coded_zero = transfer_coded.plan();
    EXPECT_TRUE(transfer_coded_zero.discards_content());
    EXPECT_TRUE(transfer_coded_zero.close_delimited() != nullptr);
    if (transfer_coded_zero.close_delimited() != nullptr) {
        EXPECT_EQ(transfer_coded_zero.close_delimited()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(transfer_coded_zero.close_delimited()->transfer_codings()[0] ==
                    ruvia::http_transfer_coding::gzip);
    }

    const auto unframed = parse_head("GET", "HTTP/1.1 205 Reset Content");
    const auto& close_zero = unframed.plan();
    EXPECT_TRUE(close_zero.discards_content());
    EXPECT_TRUE(close_zero.close_delimited() != nullptr);
    EXPECT_TRUE(close_zero.chunked() == nullptr);
    EXPECT_EQ(active_plan_alternative_count(unframed.plan()), std::size_t{1});

    const auto connect =
        parse_head("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    EXPECT_TRUE(connect.plan().connect_tunnel() != nullptr);
    EXPECT_TRUE(!connect.plan().discards_content());
}

// RFC 9110 section 15: a received 000..099 or 600..999 is processed as a 5xx
// response; the status-code is still exactly three digits.
TEST(Ruvia, http_client_processes_out_of_range_status_as_server_error) {
    for (const auto& [line, value] : {std::pair{std::string_view("HTTP/1.1 600 Odd"), 600},
             std::pair{std::string_view("HTTP/1.1 999 "), 999}, std::pair{std::string_view("HTTP/1.1 099 Low"), 99},
             std::pair{std::string_view("HTTP/1.1 000 "), 0}}) {
        std::string response(line);
        response.append("\r\nContent-Length: 2");
        const auto head = parse_head("GET", response);
        EXPECT_TRUE(head.head().status() == ruvia::http_status::internal_server_error);
        EXPECT_EQ(head.head().received_status().value(), value);
        EXPECT_EQ(require_known_length(head.plan()).content_length(), std::size_t{2});
    }
    for (const std::string_view line : {"HTTP/1.1 -00 Signed", "HTTP/1.1 +99 Signed", "HTTP/1.1 0x1 Hex"}) {
        std::string response(line);
        response.append("\r\nContent-Length: 0");
        EXPECT_TRUE(parse_fails("GET", response));
    }
}

// RFC 9112 section 6.3 rule 1 precedes rule 5: HEAD and 304 responses end at
// the header section, so an invalid Content-Length does not fail them.
TEST(Ruvia, http_client_ignores_invalid_content_length_without_content) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: abc"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 1, 2"}}) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(head.plan().without_content() != nullptr);
    }
}

TEST(Ruvia, http_client_final_after_continue_does_not_cancel_released_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    const auto final_response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(!final_response.parsed()->plan().request_content_signal());
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http_client_closing_final_stops_unfinished_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);

    const auto closing_final = parser.parse(
        "HTTP/1.1 413 Content Too Large\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(closing_final.parsed() != nullptr);
    if (closing_final.parsed() != nullptr) {
        EXPECT_TRUE(closing_final.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        EXPECT_TRUE(require_known_length(closing_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

TEST(Ruvia, http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parse_head("GET", "HTTP/1.0 204 No Content");
    const auto http11 = parse_head("GET", "HTTP/1.1 204 No Content");

    EXPECT_TRUE(http10.head().protocol_version() == http_protocol_version::http10);
    EXPECT_TRUE(http11.head().protocol_version() == http_protocol_version::http11);
}

TEST(Ruvia, http_client_unframed_body_response_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().close_delimited() != nullptr);
}

TEST(Ruvia, http_client_http10_transfer_encoding_is_faulty_framing_only_with_content) {
    // RFC 9112 section 6.1: the codings are ignored; the message falls back to
    // Content-Length, else close delimiting, and closes after it.
    const auto unframed = parse_head("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked");
    const auto* close_delimited = unframed.plan().close_delimited();
    EXPECT_TRUE(close_delimited != nullptr);
    if (close_delimited != nullptr) {
        EXPECT_TRUE(close_delimited->transfer_codings().empty());
    }
    const auto framed = parse_head(
        "GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(framed.plan()).content_length(), std::size_t{5});
    EXPECT_TRUE(require_known_length(framed.plan()).persistence() ==
                http1_close_policy::close_after_response);
    // RFC 9112 section 6.3 rule 1: HEAD, 204 and 304 responses end at the
    // header section regardless of framing fields. RFC 9112 section 6.1 still
    // treats the HTTP/1.0 message as faulty: the connection closes after it,
    // whatever keep-alive it requests.
    for (const auto& [method, response] : {
             std::pair{"HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 304 Not Modified\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 204 No Content\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
         }) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(require_without_content(head.plan()).persistence() == http1_close_policy::close_after_response);
    }
    const auto keep_alive = parse_head("GET", "HTTP/1.0 304 Not Modified\r\nConnection: keep-alive");
    EXPECT_TRUE(require_without_content(keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_head_method_is_case_sensitive) {
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parse_head("head", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().without_content() != nullptr);
    EXPECT_TRUE(lowercase.plan().close_delimited() != nullptr);
}
