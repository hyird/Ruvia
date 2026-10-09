#include "http2/cleartext_upgrade.h"

#include <string_view>

#include "ruvia/http/http2_framing.h"

#include "test_harness.h"

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::detail::probe_cleartext_http2_preface;

}  // namespace

RUVIA_TEST(auto_https_reserves_cleartext_listener_for_http1) {
    RUVIA_CHECK(probe_cleartext_http2_preface(http2_client_preface, false) ==
                http2_cleartext_preface_probe::complete_preface);
    RUVIA_CHECK(probe_cleartext_http2_preface(http2_client_preface, true) ==
                http2_cleartext_preface_probe::http1);
    RUVIA_CHECK(probe_cleartext_http2_preface("PRI * HT", true) == http2_cleartext_preface_probe::http1);
}
