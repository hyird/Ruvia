#include <type_traits>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"

#include "test_harness.h"

namespace {

using ruvia::http_known_method;
using ruvia::http_response_content_semantics;
using ruvia::detail::classify_http_response_content_semantics;

}  // namespace

RUVIA_TEST(response_content_semantics_owns_method_status_precedence) {
    const auto informational =
        classify_http_response_content_semantics(http_known_method::get, ruvia::http_status::early_hints);
    RUVIA_CHECK(informational == http_response_content_semantics::informational);

    const auto protocol_switch = classify_http_response_content_semantics(
        http_known_method::get, ruvia::http_status::switching_protocols);
    RUVIA_CHECK(protocol_switch == http_response_content_semantics::protocol_switch);

    const auto tunnel =
        classify_http_response_content_semantics(http_known_method::connect, ruvia::http_status::no_content);
    RUVIA_CHECK(tunnel == http_response_content_semantics::connect_tunnel);

    for (const ruvia::http_status_code status :
        {ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        const auto without_content = classify_http_response_content_semantics(http_known_method::get, status);
        RUVIA_CHECK(without_content == http_response_content_semantics::without_content);
    }

    const auto head = classify_http_response_content_semantics(http_known_method::head, ruvia::http_status::ok);
    RUVIA_CHECK(head == http_response_content_semantics::without_content);

    const auto rejected_connect =
        classify_http_response_content_semantics(http_known_method::connect, ruvia::http_status::not_found);
    RUVIA_CHECK(rejected_connect == http_response_content_semantics::with_content);

    // RFC 9110 Section 6.4.1 deliberately classifies 205 among responses that
    // have content (necessarily zero-length by Section 15.3.6), unlike 204.
    const auto reset_content =
        classify_http_response_content_semantics(http_known_method::get, ruvia::http_status::reset_content);
    RUVIA_CHECK(reset_content == http_response_content_semantics::with_content);
}

RUVIA_TEST(response_content_semantics_preserves_case_sensitive_method_tokens) {
    const auto head = classify_http_response_content_semantics("HEAD", ruvia::http_status::ok);
    const auto lower_head = classify_http_response_content_semantics("head", ruvia::http_status::ok);
    const auto connect = classify_http_response_content_semantics("CONNECT", ruvia::http_status::ok);
    const auto lower_connect = classify_http_response_content_semantics("connect", ruvia::http_status::ok);
    RUVIA_CHECK(head == http_response_content_semantics::without_content);
    RUVIA_CHECK(lower_head == http_response_content_semantics::with_content);
    RUVIA_CHECK(connect == http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(lower_connect == http_response_content_semantics::with_content);
}
