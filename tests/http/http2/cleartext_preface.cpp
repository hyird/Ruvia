#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http2_framing.h"

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::probe_http2_cleartext_preface;

}  // namespace

TEST(Ruvia, http2_cleartext_preface_accepts_only_prior_knowledge) {
    EXPECT_TRUE(probe_http2_cleartext_preface("GET / HTTP/1.1\r\n") ==
                http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("") == http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI * HT") == http2_cleartext_preface_probe::need_more_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface(http2_client_preface) ==
                http2_cleartext_preface_probe::complete_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI /not-http2\r\n") ==
                http2_cleartext_preface_probe::drop_connection);
}
