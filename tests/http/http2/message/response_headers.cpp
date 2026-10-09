#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response.h"

#include "http2/http2_hpack.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_stream_state.h"
#include "response/http_response_headers_access.h"
#include "test_harness.h"

namespace {

using ruvia::http_interim_response_head;
using ruvia::http_response;
using ruvia::detail::append_http2_interim_response_headers;
using ruvia::detail::append_http2_response_headers;
using ruvia::detail::hpack_decoder;
using ruvia::detail::http2_response_head_plan;
using ruvia::detail::http2_response_head_plan_result;
using ruvia::detail::http2_stream_state;

enum class response_head_mode : std::uint8_t { buffered,
    streaming };

struct collector final {
    std::vector<std::pair<std::string, std::string>> headers_;
};

void add_unchecked_header(http_response& response, std::string_view name, std::string_view value) {
    auto& headers = const_cast<ruvia::http_response_headers&>(response.headers());
    (void)ruvia::detail::http_response_headers_access::add(headers, name, value, 0);
}

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<collector*>(target)->headers_.emplace_back(std::string(name), std::string(value));
    return true;
}

bool append_buffered_response_headers(http2_stream_state& stream, const http_response& response,
    ruvia::http_known_method method = ruvia::http_known_method::get) {
    const auto plan_result = ruvia::detail::http2_buffered_response_head_plan(
        ruvia::plan_buffered_http_response_write(method, response), response);
    const auto* plan = plan_result.plan();
    const auto control_result = ruvia::detail::http2_final_response_control_plan(response);
    const auto* http2_control = control_result.control();
    if (plan == nullptr || http2_control == nullptr) {
        return false;
    }
    if (!append_http2_response_headers(stream, response, *plan, *http2_control)) {
        return false;
    }
    return true;
}

bool decode_response_headers(const http_response& response, collector& out,
    response_head_mode mode = response_head_mode::buffered,
    ruvia::http_known_method method = ruvia::http_known_method::get) {
    http2_stream_state stream(1, std::pmr::get_default_resource());
    if (mode == response_head_mode::buffered) {
        if (!append_buffered_response_headers(stream, response, method)) {
            return false;
        }
    } else {
        const auto body_plan = ruvia::plan_http_response_body(method, response.status());
        const auto plan_result = ruvia::detail::http2_streaming_response_head_plan(body_plan, response);
        const auto* plan = plan_result.plan();
        const auto control_result = ruvia::detail::http2_final_response_control_plan(response);
        const auto* http2_control = control_result.control();
        if (plan == nullptr || http2_control == nullptr) {
            return false;
        }
        if (!append_http2_response_headers(stream, response, *plan, *http2_control)) {
            return false;
        }
    }

    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    const auto result_value = decoder.decode(stream.local_header_block(), &out, &collect);
    return result_value.decoded();
}

bool decode_interim_response_headers(const http_interim_response_head& response, collector& out) {
    http2_stream_state stream(1, std::pmr::get_default_resource());
    if (append_http2_interim_response_headers(stream, response) !=
        ruvia::detail::http2_interim_response_header_encode_status::ok) {
        return false;
    }

    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    const auto result_value = decoder.decode(stream.local_header_block(), &out, &collect);
    return result_value.decoded();
}

bool has_header(const collector& headers, std::string_view name, std::string_view value) {
    for (const auto& header : headers.headers_) {
        if (header.first == name && header.second == value) {
            return true;
        }
    }
    return false;
}

bool has_header_name(const collector& headers, std::string_view name) {
    for (const auto& header : headers.headers_) {
        if (header.first == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

RUVIA_TEST(http2_response_head_content_length_plan_drives_execution) {
    http_response buffered({.resource_ = std::pmr::get_default_resource()});
    buffered.body("hello");
    const auto buffered_plan_result = ruvia::detail::http2_buffered_response_head_plan(
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, buffered),
        buffered);
    const auto* buffered_plan = buffered_plan_result.plan();
    RUVIA_CHECK(buffered_plan != nullptr);
    if (buffered_plan == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(buffered_plan->content_length(), std::optional<std::uint64_t>{5});
    RUVIA_CHECK(!buffered_plan->streaming_content_length().has_value());

    http_response streaming({.resource_ = std::pmr::get_default_resource()});
    const auto streaming_body_plan =
        ruvia::plan_http_response_body(ruvia::http_known_method::get, streaming.status());
    const auto streaming_plan_result =
        ruvia::detail::http2_streaming_response_head_plan(streaming_body_plan, streaming);
    const auto* streaming_plan = streaming_plan_result.plan();
    RUVIA_CHECK(streaming_plan != nullptr);
    if (streaming_plan == nullptr) {
        return;
    }
    RUVIA_CHECK(!streaming_plan->content_length().has_value());
    RUVIA_CHECK(!streaming_plan->streaming_content_length().has_value());

    streaming.header("Content-Length", "0005");
    const auto explicit_plan_result =
        ruvia::detail::http2_streaming_response_head_plan(streaming_body_plan, streaming);
    const auto* explicit_plan = explicit_plan_result.plan();
    RUVIA_CHECK(explicit_plan != nullptr);
    if (explicit_plan == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(explicit_plan->content_length(), std::optional<std::uint64_t>{5});
    RUVIA_CHECK_EQ(explicit_plan->streaming_content_length(), std::optional<std::uint64_t>{5});

    http_response no_content({.resource_ = std::pmr::get_default_resource()});
    no_content.status(ruvia::http_status::no_content);
    no_content.header("Content-Length", "12");
    const auto forbidden_plan_result = ruvia::detail::http2_streaming_response_head_plan(
        ruvia::plan_http_response_body(ruvia::http_known_method::get, no_content.status()),
        no_content);
    const auto* forbidden_plan = forbidden_plan_result.plan();
    RUVIA_CHECK(forbidden_plan != nullptr);
    if (forbidden_plan == nullptr) {
        return;
    }
    RUVIA_CHECK(!forbidden_plan->content_length().has_value());
    RUVIA_CHECK(!forbidden_plan->streaming_content_length().has_value());

    const auto connect_plan_result =
        ruvia::detail::http2_connect_response_head_plan(ruvia::plan_http_response_body(
            ruvia::http_known_method::connect, ruvia::http_status::ok));
    const auto* connect_plan = connect_plan_result.plan();
    RUVIA_CHECK(connect_plan != nullptr);
    if (connect_plan == nullptr) {
        return;
    }
    RUVIA_CHECK(connect_plan->body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(!connect_plan->content_length().has_value());
    RUVIA_CHECK(!connect_plan->streaming_content_length().has_value());

    const auto invalid_connect_plan = ruvia::detail::http2_connect_response_head_plan(streaming_body_plan);
    RUVIA_CHECK(invalid_connect_plan.plan() == nullptr);
    RUVIA_CHECK(invalid_connect_plan.failure() != nullptr);
    RUVIA_CHECK(invalid_connect_plan.failure()->error() ==
                ruvia::detail::http2_response_head_plan_error::connect_tunnel_required);
}

RUVIA_TEST(http2_response_head_rejects_status_plan_mismatch) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::multi_status);
    response.body("planned");
    const auto buffered_write_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto streaming_body_plan =
        ruvia::plan_http_response_body(ruvia::http_known_method::get, response.status());

    response.status(ruvia::http_status::already_reported);
    const auto buffered = ruvia::detail::http2_buffered_response_head_plan(buffered_write_plan, response);
    RUVIA_CHECK(buffered.plan() == nullptr);
    RUVIA_CHECK(buffered.failure() != nullptr);
    RUVIA_CHECK(buffered.failure()->error() ==
                ruvia::detail::http2_response_head_plan_error::response_status_mismatch);

    const auto streaming =
        ruvia::detail::http2_streaming_response_head_plan(streaming_body_plan, response);
    RUVIA_CHECK(streaming.plan() == nullptr);
    RUVIA_CHECK(streaming.failure() != nullptr);
    RUVIA_CHECK(streaming.failure()->error() ==
                ruvia::detail::http2_response_head_plan_error::response_status_mismatch);
}

RUVIA_TEST(http2_response_head_rejects_representation_plan_mismatch) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto write_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);

    response.body("longer");
    const auto result_value = ruvia::detail::http2_buffered_response_head_plan(write_plan, response);
    RUVIA_CHECK(result_value.plan() == nullptr);
    RUVIA_CHECK(result_value.failure() != nullptr);
    RUVIA_CHECK(result_value.failure()->error() ==
                ruvia::detail::http2_response_head_plan_error::response_representation_mismatch);
}

RUVIA_TEST(http2_interim_response_headers_are_bodyless_exact_and_normalized) {
    const ruvia::http_header_view fields_value[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    const http_interim_response_head response(ruvia::http_status::early_hints, fields_value);

    collector headers;
    RUVIA_CHECK(decode_interim_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, ":status", "103"));
    RUVIA_CHECK(has_header(headers, "link", "</style.css>; rel=preload"));
    RUVIA_CHECK(has_header(headers, "content-type", "text/html; charset=utf-8"));
    RUVIA_CHECK(has_header(headers, "x-hint", "warm"));
    // Protocol encoding is exact: product policy must add optional Server/Date
    // explicitly instead of the generic HTTP core inventing fields.
    RUVIA_CHECK(!has_header_name(headers, "server"));
    RUVIA_CHECK(!has_header_name(headers, "date"));
    RUVIA_CHECK(!has_header_name(headers, "content-length"));
}

RUVIA_TEST(http2_interim_response_header_rejection_is_transactional) {
    http2_stream_state stream(1, std::pmr::get_default_resource());
    const auto rejects = [&](std::span<const ruvia::http_header_view> fields_value) {
        stream.local_header_block().assign("sentinel");
        const http_interim_response_head response(ruvia::http_status::early_hints, fields_value);
        const auto status = append_http2_interim_response_headers(stream, response);
        const bool unchanged = stream.local_header_block() == "sentinel";
        stream.local_header_block().clear();
        return status == ruvia::detail::http2_interim_response_header_encode_status::invalid_header &&
               unchanged;
    };

    const ruvia::http_header_view content_length[] = {{"Content-Length", "0"}};
    const ruvia::http_header_view transfer_encoding[] = {
        {"Transfer-Encoding", "chunked"},
    };
    const ruvia::http_header_view connection[] = {{"Connection", "close"}};
    const ruvia::http_header_view te[] = {{"TE", "trailers"}};
    const ruvia::http_header_view trailer[] = {{"Trailer", "X-Checksum"}};
    const ruvia::http_header_view malformed[] = {{"Bad Name", "value"}};
    const ruvia::http_header_view malformed_content_encoding[] = {
        {"Content-Encoding", "gzip;level=9"},
    };
    const ruvia::http_header_view empty_content_encoding[] = {
        {"Content-Encoding", ""},
    };
    const ruvia::http_header_view malformed_content_type[] = {
        {"Content-Type", "not a media type"},
    };
    const ruvia::http_header_view empty_content_type[] = {
        {"Content-Type", ""},
    };
    const ruvia::http_header_view leading_whitespace[] = {
        {"Link", " </style.css>; rel=preload"},
    };
    const ruvia::http_header_view trailing_whitespace[] = {
        {"Link", "</style.css>; rel=preload\t"},
    };
    const ruvia::http_header_view duplicate_server[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    RUVIA_CHECK(rejects(content_length));
    RUVIA_CHECK(rejects(transfer_encoding));
    RUVIA_CHECK(rejects(connection));
    RUVIA_CHECK(rejects(te));
    RUVIA_CHECK(rejects(trailer));
    RUVIA_CHECK(rejects(malformed));
    RUVIA_CHECK(rejects(malformed_content_encoding));
    RUVIA_CHECK(rejects(empty_content_encoding));
    RUVIA_CHECK(rejects(malformed_content_type));
    RUVIA_CHECK(rejects(empty_content_type));
    RUVIA_CHECK(rejects(leading_whitespace));
    RUVIA_CHECK(rejects(trailing_whitespace));
    RUVIA_CHECK(rejects(duplicate_server));

    const std::string oversized_value(ruvia::max_http_header_bytes, 'x');
    const ruvia::http_header_view oversized[] = {
        {"X-Oversized", oversized_value},
    };
    RUVIA_CHECK(rejects(oversized));

    std::array<ruvia::http_header_view, ruvia::max_http_header_fields + 1> too_many{};
    for (auto& header : too_many) {
        header = {"X-Many", "value"};
    }
    RUVIA_CHECK(rejects(too_many));
}

RUVIA_TEST(http2_response_headers_keep_server_product_policy_explicit) {
    http_response response({.resource_ = std::pmr::get_default_resource()});

    collector defaults;
    RUVIA_CHECK(decode_response_headers(response, defaults));
    RUVIA_CHECK(!has_header_name(defaults, "server"));
    // Date remains protocol-generated for a final 2xx origin response.
    RUVIA_CHECK(has_header_name(defaults, "date"));

    response.header("Server", "custom/1");
    collector explicit_server;
    RUVIA_CHECK(decode_response_headers(response, explicit_server));
    RUVIA_CHECK(has_header(explicit_server, "server", "custom/1"));
}

RUVIA_TEST(http2_response_headers_omit_content_length_for_204) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::no_content);
    response.header("Content-Length", "12");

    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, ":status", "204"));
    RUVIA_CHECK(!has_header_name(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_keep_explicit_content_length_for_304) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::not_modified);
    response.header("Content-Length", "12");

    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, ":status", "304"));
    RUVIA_CHECK(has_header(headers, "content-length", "12"));
}

RUVIA_TEST(http2_response_headers_do_not_auto_content_length_for_304) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::not_modified);

    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, ":status", "304"));
    RUVIA_CHECK(!has_header_name(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_canonicalize_valid_explicit_content_length_once) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.header("Content-Length", "0005");

    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers, response_head_mode::streaming));
    RUVIA_CHECK(has_header(headers, "content-length", "5"));
    RUVIA_CHECK(!has_header(headers, "content-length", "0005"));
}

RUVIA_TEST(http2_response_headers_canonicalize_205_to_zero_length) {
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::reset_content);
    response.header("Content-Length", "12");

    for (const auto mode : {response_head_mode::streaming, response_head_mode::buffered}) {
        collector headers;
        RUVIA_CHECK(decode_response_headers(response, headers, mode));
        RUVIA_CHECK(has_header(headers, ":status", "205"));
        RUVIA_CHECK(has_header(headers, "content-length", "0"));
        RUVIA_CHECK(!has_header(headers, "content-length", "12"));
        RUVIA_CHECK(!has_header(headers, "content-length", "99"));
    }
}

RUVIA_TEST(http2_response_headers_override_wrong_content_length_for_200) {
    // RFC 9113 8.1.1: a content-length that disagrees with the DATA payload length
    // is malformed and a conformant peer resets the stream. For a buffered 2xx the
    // writer owns the length, so a handler's wrong Content-Length must be dropped
    // and replaced with the real body size -- matching the HTTP/1.1 path -- rather
    // than HPACK-encoded verbatim.
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    response.header("Content-Length", "1000");  // wrong: the real body is 5 bytes
    response.body("hello");

    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, ":status", "200"));
    RUVIA_CHECK(has_header(headers, "content-length", "5"));      // corrected to the body size
    RUVIA_CHECK(!has_header(headers, "content-length", "1000"));  // the wrong user value is gone
}

RUVIA_TEST(http2_response_headers_reject_only_preserved_invalid_content_length) {
    http_response streaming({.resource_ = std::pmr::get_default_resource()});
    streaming.status(ruvia::http_status::ok);
    streaming.header("Content-Length", "not-a-number");
    streaming.body("hello");
    http2_stream_state stream(1, std::pmr::get_default_resource());
    const auto streaming_body_plan =
        ruvia::plan_http_response_body(ruvia::http_known_method::get, streaming.status());
    const auto streaming_plan =
        ruvia::detail::http2_streaming_response_head_plan(streaming_body_plan, streaming);
    RUVIA_CHECK(streaming_plan.plan() == nullptr);
    RUVIA_CHECK(streaming_plan.failure() != nullptr);
    RUVIA_CHECK(streaming_plan.failure()->error() ==
                ruvia::detail::http2_response_head_plan_error::invalid_content_length);
    RUVIA_CHECK(stream.local_header_block().empty());

    // A buffered writer owns and canonicalizes the field, so an invalid caller
    // value is filtered before it can make the message malformed.
    collector buffered_headers;
    RUVIA_CHECK(decode_response_headers(streaming, buffered_headers));
    RUVIA_CHECK(has_header(buffered_headers, "content-length", "5"));

    http_response not_modified({.resource_ = std::pmr::get_default_resource()});
    not_modified.status(ruvia::http_status::not_modified);
    not_modified.header("Content-Length", "5, 5");
    const auto not_modified_plan = ruvia::detail::http2_buffered_response_head_plan(
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, not_modified),
        not_modified);
    RUVIA_CHECK(not_modified_plan.plan() == nullptr);
    RUVIA_CHECK(not_modified_plan.failure() != nullptr);
    RUVIA_CHECK(stream.local_header_block().empty());
}

RUVIA_TEST(http2_response_headers_set_cookie_uses_never_indexed_literal) {
    // RFC 7541 §7.1.3: Set-Cookie carries session credentials and must be emitted
    // as a never-indexed literal (0x10 prefix) so an intermediary never places it
    // in a shared HPACK dynamic table.
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    response.header("Set-Cookie", "sid=secret; HttpOnly");

    http2_stream_state stream(1, std::pmr::get_default_resource());
    RUVIA_CHECK(append_buffered_response_headers(stream, response));
    const auto& block = stream.local_header_block();

    // Byte 0 is the indexed :status 200 (0x88); Set-Cookie is the next field and
    // its representation prefix must be the never-indexed literal nibble (0x10).
    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x10U);

    // The never-indexed hint must not corrupt the round-trip value.
    collector headers;
    RUVIA_CHECK(decode_response_headers(response, headers));
    RUVIA_CHECK(has_header(headers, "set-cookie", "sid=secret; HttpOnly"));
}

RUVIA_TEST(http2_response_headers_non_sensitive_uses_without_indexing) {
    // A non-credential field (content-type) must stay a plain without-indexing
    // literal (0x00 nibble) -- confirms the never-indexed choice discriminates by
    // header name rather than marking everything.
    http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    response.header("Content-Type", "text/plain");

    http2_stream_state stream(1, std::pmr::get_default_resource());
    RUVIA_CHECK(append_buffered_response_headers(stream, response));
    const auto& block = stream.local_header_block();

    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x00U);
}

RUVIA_TEST(http2_response_headers_reject_connection_specific_fields_before_hpack) {
    constexpr std::pair<std::string_view, std::string_view> fields_value[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields_value) {
        http_response response({.resource_ = std::pmr::get_default_resource()});
        if (name == "TE") {
            add_unchecked_header(response, name, value);
        } else {
            response.header(name, value);
        }
        collector headers;
        RUVIA_CHECK(!decode_response_headers(response, headers));
        RUVIA_CHECK(headers.headers_.empty());
    }
}

RUVIA_TEST(http2_response_headers_reject_leading_and_trailing_value_whitespace_before_hpack) {
    for (const auto value : {" value", "value ", "\tvalue", "value\t"}) {
        http_response response({.resource_ = std::pmr::get_default_resource()});
        response.header_stable_view("X-Test", value);

        collector headers;
        RUVIA_CHECK(!decode_response_headers(response, headers));
        RUVIA_CHECK(headers.headers_.empty());
    }
}

RUVIA_TEST(http2_response_headers_reject_malformed_name_and_value_before_hpack) {
    constexpr std::pair<std::string_view, std::string_view> fields_value[] = {
        {"Bad Name", "value"},
        {"X-Test", std::string_view("bad\r\nvalue", 10)},
    };

    for (const auto& [name, value] : fields_value) {
        http_response response({.resource_ = std::pmr::get_default_resource()});
        response.header_stable_view(name, value);

        collector headers;
        RUVIA_CHECK(!decode_response_headers(response, headers));
        RUVIA_CHECK(headers.headers_.empty());
    }
}
