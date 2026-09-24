#include <string_view>

#include "ruvia/http/Http2CleartextPreface.h"
#include "ruvia/http/Http2Framing.h"

#include "test_harness.h"

namespace {

using ruvia::Http2CleartextPrefaceProbe;
using ruvia::kHttp2ClientPreface;
using ruvia::probeHttp2CleartextPreface;

}  // namespace

RUVIA_TEST(http2_cleartext_preface_accepts_only_prior_knowledge) {
    RUVIA_CHECK(probeHttp2CleartextPreface("GET / HTTP/1.1\r\n") ==
                Http2CleartextPrefaceProbe::kHttp1);
    RUVIA_CHECK(probeHttp2CleartextPreface("") == Http2CleartextPrefaceProbe::kHttp1);
    RUVIA_CHECK(
        probeHttp2CleartextPreface("PRI * HT") == Http2CleartextPrefaceProbe::kNeedMorePreface);
    RUVIA_CHECK(probeHttp2CleartextPreface(kHttp2ClientPreface) ==
                Http2CleartextPrefaceProbe::kCompletePreface);
    RUVIA_CHECK(probeHttp2CleartextPreface("PRI /not-http2\r\n") ==
                Http2CleartextPrefaceProbe::kDropConnection);
}
