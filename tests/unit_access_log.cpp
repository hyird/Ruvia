#include "test_harness.h"

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using ruvia::AccessLogRecord;
using ruvia::HttpKnownMethod;
using ruvia::HttpProtocolVersion;
using ruvia::HttpRequest;
using ruvia::HttpServerOptions;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::recordHttpAccess;

template <typename Record>
concept HasLegacyHttp2Flag = requires(const Record& record) {
    { record.http2() } -> std::same_as<bool>;
};

using RecordHttpAccessFunction = void (*)(
    const HttpServerOptions::AccessLog&,
    const HttpRequest&,
    std::string_view,
    std::uint16_t,
    std::chrono::steady_clock::time_point) noexcept;

static_assert(std::is_same_v<
    decltype(std::declval<const AccessLogRecord&>().protocolVersion()),
    HttpProtocolVersion>);
static_assert(!HasLegacyHttp2Flag<AccessLogRecord>);
static_assert(std::is_same_v<
    decltype(&recordHttpAccess),
    RecordHttpAccessFunction>);
static_assert(!std::is_default_constructible_v<AccessLogRecord>);
static_assert(std::is_nothrow_copy_constructible_v<AccessLogRecord>);
static_assert(!std::is_copy_assignable_v<AccessLogRecord>);

struct AccessLogObservation final {
    std::size_t calls{0};
    std::string_view method;
    HttpKnownMethod knownMethod{HttpKnownMethod::kUnknown};
    std::string_view path;
    std::string_view remoteAddress;
    std::uint16_t status{0};
    std::uint64_t durationMicros{0};
    std::array<HttpProtocolVersion, 3> versions{};
};

void observeAccessLog(void* user, const AccessLogRecord& record) noexcept {
    auto& observation = *static_cast<AccessLogObservation*>(user);
    observation.method = record.method();
    observation.knownMethod = record.knownMethod();
    observation.path = record.path();
    observation.remoteAddress = record.remoteAddress();
    observation.status = record.status();
    observation.durationMicros = record.durationMicros();
    if (observation.calls < observation.versions.size()) {
        observation.versions[observation.calls] = record.protocolVersion();
    }
    ++observation.calls;
}

[[nodiscard]] HttpRequest makeRequest(
    std::string_view method,
    std::string_view path,
    HttpProtocolVersion protocolVersion) noexcept {
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, method);
    HttpRequestAccess::setTarget(request, path);
    HttpRequestAccess::setPath(request, path);
    HttpRequestAccess::setProtocolVersion(request, protocolVersion);
    return request;
}

}  // namespace

RUVIA_TEST(access_log_record_borrows_one_typed_request) {
    auto request = makeRequest(
        "PROPFIND",
        "/collection",
        HttpProtocolVersion::kHttp10);
    AccessLogObservation observation;
    HttpServerOptions::AccessLog accessLog;
    accessLog.callback = &observeAccessLog;
    accessLog.user = &observation;

    recordHttpAccess(
        accessLog,
        request,
        "192.0.2.80",
        207,
        std::chrono::steady_clock::now());

    RUVIA_CHECK_EQ(observation.calls, std::size_t{1});
    RUVIA_CHECK_EQ(observation.method, std::string_view("PROPFIND"));
    RUVIA_CHECK(observation.knownMethod == HttpKnownMethod::kUnknown);
    RUVIA_CHECK_EQ(observation.path, std::string_view("/collection"));
    RUVIA_CHECK_EQ(
        observation.remoteAddress,
        std::string_view("192.0.2.80"));
    RUVIA_CHECK_EQ(observation.status, std::uint16_t{207});
    RUVIA_CHECK(
        observation.versions[0] == HttpProtocolVersion::kHttp10);
}

RUVIA_TEST(access_log_preserves_all_protocol_versions_without_transport_bool) {
    AccessLogObservation observation;
    HttpServerOptions::AccessLog accessLog;
    accessLog.callback = &observeAccessLog;
    accessLog.user = &observation;
    constexpr std::array versions{
        HttpProtocolVersion::kHttp10,
        HttpProtocolVersion::kHttp11,
        HttpProtocolVersion::kHttp2};

    for (const auto version : versions) {
        auto request = makeRequest("GET", "/version", version);
        recordHttpAccess(
            accessLog,
            request,
            "198.51.100.81",
            200,
            std::chrono::steady_clock::now());
    }

    RUVIA_CHECK_EQ(observation.calls, versions.size());
    for (std::size_t i = 0; i < versions.size(); ++i) {
        RUVIA_CHECK(observation.versions[i] == versions[i]);
    }
}
