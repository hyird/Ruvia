#pragma once

#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <zstd.h>

#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/field/HttpAcceptMediaType.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/MultipartDelimiter.h"
#include "ruvia/http/detail/parser/MultipartPartHeaders.h"
#include "ruvia/http/detail/request/RequestBodyDecoding.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequest.h"

namespace field_parsing_test {

using ruvia::HttpContentCoding;
using ruvia::detail::HttpChunkScanComplete;
using ruvia::detail::HttpChunkScanFailure;
using ruvia::detail::HttpChunkScanNeedMore;
using ruvia::detail::HttpChunkScanResult;
using ruvia::detail::HttpMultipartPartHeaders;

















}  // namespace field_parsing_test

// --- Semicolon parameters: quoted-string awareness -----------------------

// --- Multipart boundary must be a full delimiter line, not a prefix ------

// --- zstd request-body decode: truncation must be rejected ---------------

// --- Accept quality parsing shares the quote-aware parameter scanner ------

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------

using namespace field_parsing_test;  // NOLINT(google-build-using-namespace)
