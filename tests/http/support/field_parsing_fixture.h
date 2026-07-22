#pragma once

#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
#include "ruvia/web/Model.h"

namespace {

using ruvia::detail::HttpContentCoding;
using ruvia::detail::HttpChunkScanComplete;
using ruvia::detail::HttpChunkScanFailure;
using ruvia::detail::HttpChunkScanNeedMore;
using ruvia::detail::HttpChunkScanResult;
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
concept HasAnyRvalueHttpChunkScanAccessor =
    requires(T&& result) { std::move(result).needMore(); } ||
    requires(T&& result) { std::move(result).complete(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(std::same_as<
    decltype(ruvia::detail::scanHttpChunkedBody(std::string_view{})),
    HttpChunkScanResult>);
static_assert(!std::default_initializable<HttpChunkScanResult>);
static_assert(!HasAnyRvalueHttpChunkScanAccessor<HttpChunkScanResult>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanNeedMore>);
static_assert(HasChunkScanConsumedBytes<HttpChunkScanComplete>);
static_assert(!HasChunkScanConsumedBytes<HttpChunkScanFailure>);
static_assert(!HasChunkScanError<HttpChunkScanNeedMore>);
static_assert(!HasChunkScanError<HttpChunkScanComplete>);
static_assert(HasChunkScanError<HttpChunkScanFailure>);

template <typename T>
concept HasCookiesAccessor = requires(const T& request) {
    request.cookies();
};

template <typename T>
concept HasQueryListAccessor = requires(const T& request) {
    request.query();
};

template <typename T>
concept HasQueriesVectorAccessor = requires(const T& request) {
    request.queries(std::string_view{});
};

template <typename T>
concept ParsesAnyRvalueOwningString =
    requires(std::string&& body) {
        T::parse(std::move(body), std::pmr::get_default_resource());
    } ||
    requires(const std::string&& body) {
        T::parse(std::move(body), std::pmr::get_default_resource());
    };

template <typename T>
concept ParsesLvalueOwningString = requires(const std::string& body) {
    T::parse(body, std::pmr::get_default_resource());
};

static_assert(!HasCookiesAccessor<ruvia::HttpRequest>);
static_assert(!HasQueryListAccessor<ruvia::HttpRequest>);
static_assert(!HasQueriesVectorAccessor<ruvia::HttpRequest>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonValue>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonObject>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::FormObject>);
static_assert(ParsesLvalueOwningString<ruvia::JsonValue>);
static_assert(ParsesLvalueOwningString<ruvia::JsonObject>);
static_assert(ParsesLvalueOwningString<ruvia::FormObject>);

struct AccessorSurfaceRequest final {
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(AccessorSurfaceRequest, message);
};

struct AccessorSurfaceResponse final {
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(AccessorSurfaceResponse, message);
};

struct NestedModelItem final {
    RUVIA_FIELD(id, ruvia::UInt32);
    RUVIA_OPTIONAL_FIELD(label, ruvia::String);
    RUVIA_MODEL(NestedModelItem, id, label);
};

struct NestedModelEnvelope final {
    RUVIA_FIELD(primary, NestedModelItem);
    RUVIA_FIELD(items, ruvia::Array<NestedModelItem>);
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(NestedModelEnvelope, primary, items, tags);
};

struct MaxFieldCountResponse final {
    RUVIA_OPTIONAL_FIELD(f01, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f02, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f03, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f04, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f05, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f06, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f07, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f08, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f09, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f10, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f11, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f12, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f13, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f14, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f15, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f16, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f17, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f18, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f19, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f20, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f21, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f22, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f23, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f24, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f25, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f26, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f27, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f28, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f29, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f30, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f31, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f32, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f33, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f34, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f35, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f36, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f37, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f38, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f39, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f40, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f41, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f42, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f43, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f44, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f45, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f46, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f47, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f48, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f49, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f50, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f51, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f52, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f53, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f54, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f55, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f56, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f57, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f58, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f59, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f60, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f61, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f62, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(f63, ruvia::Bool); RUVIA_OPTIONAL_FIELD(f64, ruvia::Bool);
    RUVIA_MODEL(MaxFieldCountResponse,
        f01, f02, f03, f04, f05, f06, f07, f08,
        f09, f10, f11, f12, f13, f14, f15, f16,
        f17, f18, f19, f20, f21, f22, f23, f24,
        f25, f26, f27, f28, f29, f30, f31, f32,
        f33, f34, f35, f36, f37, f38, f39, f40,
        f41, f42, f43, f44, f45, f46, f47, f48,
        f49, f50, f51, f52, f53, f54, f55, f56,
        f57, f58, f59, f60, f61, f62, f63, f64);
};

static_assert(ruvia::detail::isResponseModel<MaxFieldCountResponse>);

template <typename T>
concept ExposesAnyRvalueGeneratedMessageMember =
    requires { std::declval<const T&&>().message(); } ||
    requires { std::declval<T&&>().messageEnsure(); } ||
    requires { std::declval<T&&>().message(std::string_view{}); };

static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<AccessorSurfaceRequest&>().message())>,
    std::optional<ruvia::String>>);
static_assert(std::same_as<
    std::remove_cvref_t<decltype(std::declval<const AccessorSurfaceRequest&>().message())>,
    std::optional<ruvia::String>>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceRequest>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceResponse>);


}  // namespace

// --- Semicolon parameters: quoted-string awareness -----------------------

// --- Multipart boundary must be a full delimiter line, not a prefix ------

// --- zstd request-body decode: truncation must be rejected ---------------

// --- Accept quality parsing shares the quote-aware parameter scanner ------

// --- Chunk extension quoted-pair follows RFC quoted-string grammar -------
