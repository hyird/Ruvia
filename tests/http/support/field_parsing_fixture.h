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
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/MultipartBoundary.h"
#include "ruvia/http/detail/parser/MultipartDelimiter.h"
#include "ruvia/http/detail/parser/MultipartPartHeaders.h"
#include "ruvia/http/detail/request/RequestBodyDecoding.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequest.h"

namespace field_parsing_test {

using ruvia::detail::HttpChunkScanComplete;
using ruvia::detail::HttpChunkScanFailure;
using ruvia::detail::HttpChunkScanNeedMore;
using ruvia::detail::HttpChunkScanResult;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpMultipartPartHeaders;

template <typename T>
concept HasChunkScanConsumedBytes = requires(const T& result) {
    { result.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasChunkScanError = requires(const T& result) {
    { result.error() } -> std::same_as<ruvia::detail::HttpChunkScanError>;
};

template <typename T>
concept HasAnyRvalueHttpChunkScanAccessor = requires(T&& result) { std::move(result).needMore(); } || requires(T&& result) { std::move(result).complete(); } || requires(T&& result) { std::move(result).failure(); };

static_assert(std::same_as<decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})), HttpChunkScanResult>);
static_assert(!std::default_initializable<HttpChunkScanResult>);
static_assert(!HasAnyRvalueHttpChunkScanAccessor<HttpChunkScanResult>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanNeedMore>);
static_assert(HasChunkScanConsumedBytes<HttpChunkScanComplete>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanFailure>);
static_assert(!HasChunkScanError<HttpChunkScanNeedMore>);
static_assert(!HasChunkScanError<HttpChunkScanComplete>);
static_assert(HasChunkScanError<HttpChunkScanFailure>);

}  // namespace field_parsing_test

// --- Semicolon parameters: quoted-string awareness -----------------------

// --- Multipart boundary must be a full delimiter line, not a prefix ------

// --- zstd request-body decode: truncation must be rejected ---------------

// --- Accept quality parsing shares the quote-aware parameter scanner ------

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------

using namespace field_parsing_test;  // NOLINT(google-build-using-namespace)
