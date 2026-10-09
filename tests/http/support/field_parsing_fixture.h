#pragma once

#include <zstd.h>

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_content_decoding.h"

#include "field/http_accept_media_type.h"
#include "parser/http_chunk_parser.h"
#include "parser/multipart_delimiter.h"
#include "parser/multipart_part_headers.h"
#include "test_harness.h"

namespace field_parsing_test {

using ruvia::http_content_coding;
using ruvia::detail::http_chunk_scan_complete;
using ruvia::detail::http_chunk_scan_failure;
using ruvia::detail::http_chunk_scan_need_more;
using ruvia::detail::http_chunk_scan_result;
using ruvia::detail::http_multipart_part_headers;

}  // namespace field_parsing_test

// --- Semicolon parameters: quoted-string awareness -----------------------

// --- Multipart boundary must be a full delimiter line, not a prefix ------

// --- zstd request-body decode: truncation must be rejected ---------------

// --- Accept quality parsing shares the quote-aware parameter scanner ------

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------

using namespace field_parsing_test;  // NOLINT(google-build-using-namespace)
