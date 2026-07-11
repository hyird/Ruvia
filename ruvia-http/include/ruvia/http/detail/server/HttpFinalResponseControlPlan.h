#pragma once

#include <cstdint>

#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/HttpConnectionFields.h"

namespace ruvia::detail {

enum class HttpFinalResponseControlStatus : std::uint8_t {
    kOk,
    kInvalidStatus,
    kInvalidUpgradeField,
    kUpgradeRequired,
    kUpgradeUnavailable
};

class HttpFinalResponseControlPlan final {
public:
    [[nodiscard]] HttpFinalResponseControlStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == HttpFinalResponseControlStatus::kOk;
    }

    [[nodiscard]] const HttpUpgradeProtocols& upgradeProtocols() const noexcept {
        return upgradeProtocols_;
    }

private:
    friend HttpFinalResponseControlPlan httpFinalResponseControlPlan(
        const HttpResponse&, HttpProtocolVersion) noexcept;

    HttpFinalResponseControlPlan(
        HttpFinalResponseControlStatus status,
        HttpUpgradeProtocols upgradeProtocols = {}) noexcept
        : status_(status), upgradeProtocols_(upgradeProtocols) {}

    HttpFinalResponseControlStatus status_{HttpFinalResponseControlStatus::kInvalidStatus};
    HttpUpgradeProtocols upgradeProtocols_;
};

// Validate the control semantics before a final response mutates connection or
// HPACK state. HTTP/1 retains a parsed Upgrade plan for the connection finalizer.
// HTTP/2 cannot carry Upgrade at all, so 426 cannot be represented there: RFC
// 9110 requires Upgrade on 426 while RFC 9113 forbids that connection-specific
// field in an HTTP/2 message.
[[nodiscard]] inline HttpFinalResponseControlPlan httpFinalResponseControlPlan(
    const HttpResponse& response,
    HttpProtocolVersion protocolVersion) noexcept {
    const auto statusCode = response.status();
    if (!httpFinalStatusCodeValid(statusCode)) {
        return {HttpFinalResponseControlStatus::kInvalidStatus};
    }
    if (protocolVersion == HttpProtocolVersion::kHttp2 && statusCode == 426) {
        return {HttpFinalResponseControlStatus::kUpgradeUnavailable};
    }

    HttpUpgradeProtocols upgradeProtocols;
    if (protocolVersion != HttpProtocolVersion::kHttp2) {
        for (const auto& header : response.headers()) {
            if (!httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
                continue;
            }
            if (upgradeProtocols.parseField(
                    header.value(),
                    HttpFieldListRole::kSender,
                    [](const HttpUpgradeProtocol&) noexcept {
                        return true;
                    }) != HttpFieldListParseStatus::kOk) {
                return {HttpFinalResponseControlStatus::kInvalidUpgradeField};
            }
        }
    }
    if (statusCode == 426 && !upgradeProtocols.hasProtocol()) {
        return {HttpFinalResponseControlStatus::kUpgradeRequired, upgradeProtocols};
    }
    return {HttpFinalResponseControlStatus::kOk, upgradeProtocols};
}

}  // namespace ruvia::detail
