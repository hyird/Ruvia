#include <string_view>

#include "ruvia/http/http2_cleartext_preface.h"
#include "ruvia/http/http2_framing.h"

#include "test_harness.h"

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::probe_http2_cleartext_preface;

}  // namespace

RUVIA_TEST(http2_cleartext_preface_accepts_only_prior_knowledge) {
    RUVIA_CHECK(probe_http2_cleartext_preface("GET / HTTP/1.1\r\n") ==
                http2_cleartext_preface_probe::http1);
    RUVIA_CHECK(probe_http2_cleartext_preface("") == http2_cleartext_preface_probe::http1);
    RUVIA_CHECK(
        probe_http2_cleartext_preface("PRI * HT") == http2_cleartext_preface_probe::need_more_preface);
    RUVIA_CHECK(probe_http2_cleartext_preface(http2_client_preface) ==
                http2_cleartext_preface_probe::complete_preface);
    RUVIA_CHECK(probe_http2_cleartext_preface("PRI /not-http2\r\n") ==
                http2_cleartext_preface_probe::drop_connection);
}
