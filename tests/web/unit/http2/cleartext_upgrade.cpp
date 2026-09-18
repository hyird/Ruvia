#include <string_view>

#include "ruvia/http/Http2Framing.h"
#include "ruvia/web/detail/http2/CleartextUpgrade.h"

#include "test_harness.h"

namespace {

using ruvia::kHttp2ClientPreface;
using ruvia::detail::Http2CleartextPrefaceProbe;
using ruvia::detail::probeCleartextHttp2Preface;

}  // namespace

RUVIA_TEST(auto_https_reserves_cleartext_listener_for_http1) {
    RUVIA_CHECK(probeCleartextHttp2Preface(kHttp2ClientPreface, false) ==
                Http2CleartextPrefaceProbe::kCompletePreface);
    RUVIA_CHECK(probeCleartextHttp2Preface(kHttp2ClientPreface, true) ==
                Http2CleartextPrefaceProbe::kHttp1);
    RUVIA_CHECK(probeCleartextHttp2Preface("PRI * HT", true) == Http2CleartextPrefaceProbe::kHttp1);
}
