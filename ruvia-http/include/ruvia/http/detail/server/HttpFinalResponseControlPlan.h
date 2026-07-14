#pragma once

#include <cstdint>
#include <variant>

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"

namespace ruvia::detail {

enum class HttpFinalResponseControlPlanError : std::uint8_t {
    kInvalidStatus,
    kInvalidConnectionField,
    kInvalidUpgradeField,
    kUpgradeRequired,
    kUpgradeUnavailable,
    kConnectionSpecificFieldForbidden
};

class HttpFinalResponseControlPlanResult;

class Http1FinalResponseControl final {
public:
    [[nodiscard]] HttpConnectionOptions
    connectionOptions() const noexcept {
        return connectionOptions_;
    }

    [[nodiscard]] HttpUpgradeProtocols
    upgradeProtocols() const noexcept {
        return upgradeProtocols_;
    }

private:
    friend class HttpFinalResponseControlPlanResult;

    Http1FinalResponseControl(
        HttpConnectionOptions connectionOptions,
        HttpUpgradeProtocols upgradeProtocols) noexcept
        : connectionOptions_(connectionOptions),
          upgradeProtocols_(upgradeProtocols) {}

    HttpConnectionOptions connectionOptions_;
    HttpUpgradeProtocols upgradeProtocols_;
};

class Http2FinalResponseControl final {
private:
    friend class HttpFinalResponseControlPlanResult;

    constexpr Http2FinalResponseControl() noexcept = default;
};

class HttpFinalResponseControlPlan final {
public:
    [[nodiscard]] const Http1FinalResponseControl* http1() const & noexcept {
        return std::get_if<Http1FinalResponseControl>(&protocol_);
    }
    [[nodiscard]] const Http1FinalResponseControl* http1() const && = delete;

    [[nodiscard]] const Http2FinalResponseControl* http2() const & noexcept {
        return std::get_if<Http2FinalResponseControl>(&protocol_);
    }
    [[nodiscard]] const Http2FinalResponseControl* http2() const && = delete;

private:
    friend class HttpFinalResponseControlPlanResult;

    using Protocol = std::variant<
        Http1FinalResponseControl,
        Http2FinalResponseControl>;

    template <typename Alternative>
    explicit HttpFinalResponseControlPlan(Alternative alternative) noexcept
        : protocol_(alternative) {}

    Protocol protocol_;
};

class HttpFinalResponseControlPlanFailure final {
public:
    [[nodiscard]] constexpr HttpFinalResponseControlPlanError
    error() const noexcept {
        return error_;
    }

private:
    friend class HttpFinalResponseControlPlanResult;

    explicit constexpr HttpFinalResponseControlPlanFailure(
        HttpFinalResponseControlPlanError error) noexcept
        : error_(error) {}

    HttpFinalResponseControlPlanError error_;
};

// A final response either has one complete protocol-specific control plan or
// one failure. A failure cannot expose default Connection/Upgrade state, and a
// valid HTTP/2 plan cannot be mistaken for the parsed HTTP/1 field contract.
class HttpFinalResponseControlPlanResult final {
public:
    [[nodiscard]] const HttpFinalResponseControlPlan* plan() const & noexcept {
        return std::get_if<HttpFinalResponseControlPlan>(&value_);
    }
    [[nodiscard]] const HttpFinalResponseControlPlan* plan() const && = delete;

    [[nodiscard]] const HttpFinalResponseControlPlanFailure*
    failure() const & noexcept {
        return std::get_if<HttpFinalResponseControlPlanFailure>(&value_);
    }
    [[nodiscard]] const HttpFinalResponseControlPlanFailure*
    failure() const && = delete;

private:
    friend HttpFinalResponseControlPlanResult httpFinalResponseControlPlan(
        const HttpResponse&, HttpProtocolVersion) noexcept;

    using Value = std::variant<
        HttpFinalResponseControlPlan,
        HttpFinalResponseControlPlanFailure>;

    template <typename Alternative>
    explicit HttpFinalResponseControlPlanResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static HttpFinalResponseControlPlanResult http1(
        HttpConnectionOptions connectionOptions,
        HttpUpgradeProtocols upgradeProtocols) noexcept {
        return HttpFinalResponseControlPlanResult(
            HttpFinalResponseControlPlan(
                Http1FinalResponseControl(
                    connectionOptions,
                    upgradeProtocols)));
    }

    [[nodiscard]] static HttpFinalResponseControlPlanResult http2() noexcept {
        return HttpFinalResponseControlPlanResult(
            HttpFinalResponseControlPlan(
                Http2FinalResponseControl{}));
    }

    [[nodiscard]] static HttpFinalResponseControlPlanResult failure(
        HttpFinalResponseControlPlanError error) noexcept {
        return HttpFinalResponseControlPlanResult(
            HttpFinalResponseControlPlanFailure(error));
    }

    Value value_;
};

// Validate all control semantics before a final response mutates Connection,
// HPACK, or stream state. HTTP/1 success owns the parsed repeated Connection and
// Upgrade fields. HTTP/2 success proves that no connection-specific response
// field exists; RFC 9113 section 8.2.2 requires endpoints to reject rather than
// silently filter such an application-generated message.
[[nodiscard]] inline HttpFinalResponseControlPlanResult
httpFinalResponseControlPlan(
    const HttpResponse& response,
    HttpProtocolVersion protocolVersion) noexcept {
    const auto statusCode = response.status();
    if (!httpFinalStatusCodeValid(statusCode)) {
        return HttpFinalResponseControlPlanResult::failure(
            HttpFinalResponseControlPlanError::kInvalidStatus);
    }

    if (protocolVersion == HttpProtocolVersion::kHttp2) {
        if (statusCode == 426) {
            return HttpFinalResponseControlPlanResult::failure(
                HttpFinalResponseControlPlanError::kUpgradeUnavailable);
        }
        for (const auto& header : response.headers()) {
            if (http2IsForbiddenResponseConnectionField(header.name())) {
                return HttpFinalResponseControlPlanResult::failure(
                    HttpFinalResponseControlPlanError::
                        kConnectionSpecificFieldForbidden);
            }
        }
        return HttpFinalResponseControlPlanResult::http2();
    }

    HttpConnectionOptions connectionOptions;
    HttpUpgradeProtocols upgradeProtocols;
    for (const auto& header : response.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            if (connectionOptions.parseField(
                    header.value(),
                    HttpFieldListRole::kSender) !=
                HttpFieldListParseStatus::kOk) {
                return HttpFinalResponseControlPlanResult::failure(
                    HttpFinalResponseControlPlanError::
                        kInvalidConnectionField);
            }
            continue;
        }
        if (httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            if (upgradeProtocols.parseField(
                    header.value(),
                    HttpFieldListRole::kSender,
                    [](const HttpUpgradeProtocol&) noexcept {
                        return true;
                    }) != HttpFieldListParseStatus::kOk) {
                return HttpFinalResponseControlPlanResult::failure(
                    HttpFinalResponseControlPlanError::
                        kInvalidUpgradeField);
            }
        }
    }
    if (statusCode == 426 && !upgradeProtocols.hasProtocol()) {
        return HttpFinalResponseControlPlanResult::failure(
            HttpFinalResponseControlPlanError::kUpgradeRequired);
    }
    return HttpFinalResponseControlPlanResult::http1(
        connectionOptions,
        upgradeProtocols);
}

}  // namespace ruvia::detail
