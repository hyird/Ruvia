#pragma once

#include <cstdint>
#include <type_traits>
#include <variant>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"

namespace ruvia::detail {

enum class Http1FinalResponseControlPlanError : std::uint8_t {
    kInvalidStatus,
    kInvalidConnectionField,
    kInvalidUpgradeField,
    kUpgradeRequired,
};

enum class Http2FinalResponseControlPlanError : std::uint8_t {
    kInvalidStatus,
    kUpgradeUnavailable,
    kConnectionSpecificFieldForbidden
};

class Http1FinalResponseControl;
class Http2FinalResponseControl;
class Http1FinalResponseCommitFailure;
class Http1FinalResponseControlPlanFailure;
class Http2FinalResponseControlPlanFailure;

template <typename Control, typename Failure>
class HttpFinalResponseControlPlanResult;

using Http1FinalResponseControlPlanResult =
    HttpFinalResponseControlPlanResult<
        Http1FinalResponseControl,
        Http1FinalResponseControlPlanFailure>;
using Http2FinalResponseControlPlanResult =
    HttpFinalResponseControlPlanResult<
        Http2FinalResponseControl,
        Http2FinalResponseControlPlanFailure>;

[[nodiscard]] Http1FinalResponseControlPlanResult
http1FinalResponseControlPlan(const HttpResponse& response) noexcept;

[[nodiscard]] Http2FinalResponseControlPlanResult
http2FinalResponseControlPlan(const HttpResponse& response) noexcept;

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
    friend Http1FinalResponseControlPlanResult
    http1FinalResponseControlPlan(const HttpResponse&) noexcept;

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
    friend Http2FinalResponseControlPlanResult
    http2FinalResponseControlPlan(const HttpResponse&) noexcept;

    constexpr Http2FinalResponseControl() noexcept = default;
};

class Http1FinalResponseControlPlanFailure final {
public:
    [[nodiscard]] constexpr Http1FinalResponseControlPlanError
    error() const noexcept {
        return error_;
    }

private:
    friend class Http1FinalResponseCommitFailure;
    template <typename Control, typename Failure>
    friend class HttpFinalResponseControlPlanResult;
    friend Http1FinalResponseControlPlanResult
    http1FinalResponseControlPlan(const HttpResponse&) noexcept;

    explicit constexpr Http1FinalResponseControlPlanFailure(
        Http1FinalResponseControlPlanError error) noexcept
        : error_(error) {}

    Http1FinalResponseControlPlanError error_;
};

class Http2FinalResponseControlPlanFailure final {
public:
    [[nodiscard]] constexpr Http2FinalResponseControlPlanError
    error() const noexcept {
        return error_;
    }

private:
    template <typename Control, typename Failure>
    friend class HttpFinalResponseControlPlanResult;
    friend Http2FinalResponseControlPlanResult
    http2FinalResponseControlPlan(const HttpResponse&) noexcept;

    explicit constexpr Http2FinalResponseControlPlanFailure(
        Http2FinalResponseControlPlanError error) noexcept
        : error_(error) {}

    Http2FinalResponseControlPlanError error_;
};

// Each protocol-specific entry point returns only its validated control token or
// one typed failure. The caller already owns the protocol, so the result does not
// repeat that discriminator or admit the other protocol's impossible branch.
template <typename Control, typename Failure>
class HttpFinalResponseControlPlanResult final {
public:
    [[nodiscard]] const Control* control() const & noexcept {
        return std::get_if<Control>(&value_);
    }
    [[nodiscard]] const Control* control() const && = delete;

    [[nodiscard]] const Failure*
    failure() const & noexcept {
        return std::get_if<Failure>(&value_);
    }
    [[nodiscard]] const Failure*
    failure() const && = delete;

private:
    friend Http1FinalResponseControlPlanResult
    http1FinalResponseControlPlan(const HttpResponse&) noexcept;
    friend Http2FinalResponseControlPlanResult
    http2FinalResponseControlPlan(const HttpResponse&) noexcept;

    using Value = std::variant<Control, Failure>;

    template <typename Alternative>
    explicit HttpFinalResponseControlPlanResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    Value value_;
};

static_assert(std::is_trivially_copyable_v<
    Http1FinalResponseControlPlanResult>);
static_assert(sizeof(Http1FinalResponseControlPlanResult) <= 8);
static_assert(std::is_trivially_copyable_v<
    Http2FinalResponseControlPlanResult>);
static_assert(sizeof(Http2FinalResponseControlPlanResult) <= 2);

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields.
[[nodiscard]] inline Http1FinalResponseControlPlanResult
http1FinalResponseControlPlan(const HttpResponse& response) noexcept {
    const auto statusCode = response.status();
    if (!httpFinalStatusCodeValid(statusCode)) {
        return Http1FinalResponseControlPlanResult(
            Http1FinalResponseControlPlanFailure(
                Http1FinalResponseControlPlanError::kInvalidStatus));
    }

    HttpConnectionOptions connectionOptions;
    HttpUpgradeProtocols upgradeProtocols;
    for (const auto& header : response.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            if (connectionOptions.parseField(
                    header.value(),
                    HttpFieldListRole::kSender,
                    [](std::string_view option) noexcept {
                        return !httpConnectionOptionConflictsWithManagedField(
                            option);
                    }) !=
                HttpFieldListParseStatus::kOk) {
                return Http1FinalResponseControlPlanResult(
                    Http1FinalResponseControlPlanFailure(
                        Http1FinalResponseControlPlanError::
                            kInvalidConnectionField));
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
                return Http1FinalResponseControlPlanResult(
                    Http1FinalResponseControlPlanFailure(
                        Http1FinalResponseControlPlanError::
                            kInvalidUpgradeField));
            }
        }
    }
    if (statusCode == 426 && !upgradeProtocols.hasProtocol()) {
        return Http1FinalResponseControlPlanResult(
            Http1FinalResponseControlPlanFailure(
                Http1FinalResponseControlPlanError::kUpgradeRequired));
    }
    return Http1FinalResponseControlPlanResult(
        Http1FinalResponseControl(connectionOptions, upgradeProtocols));
}

// Validate HTTP/2 control semantics before HPACK or stream state is mutated.
// The success token proves that no connection-specific field exists; RFC 9113
// section 8.2.2 requires rejection rather than silent filtering.
[[nodiscard]] inline Http2FinalResponseControlPlanResult
http2FinalResponseControlPlan(const HttpResponse& response) noexcept {
    const auto statusCode = response.status();
    if (!httpFinalStatusCodeValid(statusCode)) {
        return Http2FinalResponseControlPlanResult(
            Http2FinalResponseControlPlanFailure(
                Http2FinalResponseControlPlanError::kInvalidStatus));
    }
    if (statusCode == 426) {
        return Http2FinalResponseControlPlanResult(
            Http2FinalResponseControlPlanFailure(
                Http2FinalResponseControlPlanError::kUpgradeUnavailable));
    }
    for (const auto& header : response.headers()) {
        if (http2IsForbiddenResponseConnectionField(header.name())) {
            return Http2FinalResponseControlPlanResult(
                Http2FinalResponseControlPlanFailure(
                    Http2FinalResponseControlPlanError::
                        kConnectionSpecificFieldForbidden));
        }
    }
    return Http2FinalResponseControlPlanResult(Http2FinalResponseControl{});
}

}  // namespace ruvia::detail
