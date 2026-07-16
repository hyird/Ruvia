#include "test_harness.h"

#include <concepts>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http2/Http2PeerSettings.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FrameTypes.h"

namespace {

using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2PeerInitialWindowChange;
using ruvia::detail::Http2PeerSettingApplied;
using ruvia::detail::Http2PeerSettingApplyResult;
using ruvia::detail::Http2PeerSettingError;
using ruvia::detail::Http2PeerSettingFailure;
using ruvia::detail::Http2PeerSettings;
using ruvia::detail::Http2Role;
using ruvia::detail::Http2SettingId;
using ruvia::detail::http2PeerSettingErrorCode;
using ruvia::detail::http2PeerSettingErrorMessage;
using ruvia::detail::http2ReadSettingEntry;
using ruvia::detail::http2SettingsPayloadSizeValid;
using ruvia::detail::http2WriteSettingsEntry;
using ruvia::detail::kHttp2DefaultInitialWindowSize;
using ruvia::detail::kHttp2DefaultMaxFrameSize;
using ruvia::detail::kHttp2MaxFrameSizeLimit;

template <typename T>
concept HasPeerSettingStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasPeerSettingChangedField = requires(const T& result) {
    result.initialWindowChanged;
};

template <typename T>
concept HasPeerSettingDeltaField = requires(const T& result) {
    result.initialWindowDelta;
};

template <typename T>
concept HasPeerSettingDeltaAccessor = requires(const T& result) {
    { result.delta() } -> std::same_as<std::int64_t>;
};

template <typename T>
concept HasPeerSettingErrorAccessor = requires(const T& result) {
    { result.error() } -> std::same_as<Http2PeerSettingError>;
};

template <typename T>
concept HasAnyRvaluePeerSettingAccessor =
    requires(T&& result) { std::move(result).applied(); } ||
    requires(T&& result) { std::move(result).initialWindowChange(); } ||
    requires(T&& result) { std::move(result).failure(); };

static_assert(!std::default_initializable<Http2PeerSettingApplyResult>);
static_assert(std::same_as<
    decltype(std::declval<const Http2PeerSettingApplyResult&>().applied()),
    const Http2PeerSettingApplied*>);
static_assert(std::same_as<
    decltype(std::declval<const Http2PeerSettingApplyResult&>().initialWindowChange()),
    const Http2PeerInitialWindowChange*>);
static_assert(std::same_as<
    decltype(std::declval<const Http2PeerSettingApplyResult&>().failure()),
    const Http2PeerSettingFailure*>);
static_assert(!HasPeerSettingStatusField<Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingChangedField<Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaField<Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingErrorAccessor<Http2PeerSettingApplyResult>);
static_assert(!HasAnyRvaluePeerSettingAccessor<
    Http2PeerSettingApplyResult>);
static_assert(!HasPeerSettingDeltaAccessor<Http2PeerSettingApplied>);
static_assert(!HasPeerSettingErrorAccessor<Http2PeerSettingApplied>);
static_assert(HasPeerSettingDeltaAccessor<Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingErrorAccessor<Http2PeerInitialWindowChange>);
static_assert(!HasPeerSettingDeltaAccessor<Http2PeerSettingFailure>);
static_assert(HasPeerSettingErrorAccessor<Http2PeerSettingFailure>);
static_assert(!std::default_initializable<Http2PeerSettingApplied>);
static_assert(!std::constructible_from<Http2PeerInitialWindowChange, std::int64_t>);
static_assert(!std::constructible_from<Http2PeerSettingFailure, Http2PeerSettingError>);

constexpr std::uint32_t kInt32Max =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

}  // namespace

RUVIA_TEST(peer_settings_payload_size_validity) {
    // A SETTINGS payload is a sequence of 6-byte entries (RFC 9113 Section 6.5).
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("", 0)));
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("aaaaaa", 6)));
    RUVIA_CHECK(http2SettingsPayloadSizeValid(std::string_view("aaaaaaaaaaaa", 12)));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string_view("aaaaa", 5)));
    RUVIA_CHECK(!http2SettingsPayloadSizeValid(std::string_view("aaaaaaa", 7)));
}

RUVIA_TEST(peer_settings_read_entry_at_offset) {
    char buf[12];
    http2WriteSettingsEntry(buf, Http2SettingId::kMaxFrameSize, 20000);
    http2WriteSettingsEntry(buf + 6, Http2SettingId::kInitialWindowSize, 12345);
    const std::string_view payload(buf, 12);

    const auto first = http2ReadSettingEntry(payload, 0);
    RUVIA_CHECK(first.id == Http2SettingId::kMaxFrameSize);
    RUVIA_CHECK_EQ(first.value, std::uint32_t{20000});

    const auto second = http2ReadSettingEntry(payload, 6);
    RUVIA_CHECK(second.id == Http2SettingId::kInitialWindowSize);
    RUVIA_CHECK_EQ(second.value, std::uint32_t{12345});
}

RUVIA_TEST(peer_settings_defaults) {
    Http2PeerSettings settings(Http2Role::kServer);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2DefaultMaxFrameSize);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), kHttp2DefaultInitialWindowSize);
    RUVIA_CHECK_EQ(settings.maxConcurrentStreams(), (std::numeric_limits<std::uint32_t>::max)());
    RUVIA_CHECK(!settings.enableConnectProtocol());
}

RUVIA_TEST(peer_setting_apply_result_is_discriminated) {
    Http2PeerSettings settings(Http2Role::kServer);

    const auto applied = settings.apply(Http2SettingId::kHeaderTableSize, 8192);
    RUVIA_CHECK(applied.applied() != nullptr);
    RUVIA_CHECK(applied.initialWindowChange() == nullptr);
    RUVIA_CHECK(applied.failure() == nullptr);

    // Re-advertising the same value still requires stream propagation, with delta zero.
    const auto changed = settings.apply(
        Http2SettingId::kInitialWindowSize,
        static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
    RUVIA_CHECK(changed.applied() == nullptr);
    RUVIA_CHECK(changed.initialWindowChange() != nullptr);
    RUVIA_CHECK_EQ(changed.initialWindowChange()->delta(), std::int64_t{0});
    RUVIA_CHECK(changed.failure() == nullptr);

    const auto failed = settings.apply(Http2SettingId::kMaxFrameSize, 0);
    RUVIA_CHECK(failed.applied() == nullptr);
    RUVIA_CHECK(failed.initialWindowChange() == nullptr);
    RUVIA_CHECK(failed.failure() != nullptr);
    RUVIA_CHECK(failed.failure()->error() == Http2PeerSettingError::kInvalidMaxFrameSize);
}

RUVIA_TEST(peer_settings_enable_push_is_directional) {
    Http2PeerSettings server(Http2Role::kServer);
    const auto serverDisabled = server.apply(Http2SettingId::kEnablePush, 0);
    RUVIA_CHECK(serverDisabled.applied() != nullptr);
    const auto serverEnabled = server.apply(Http2SettingId::kEnablePush, 1);
    RUVIA_CHECK(serverEnabled.applied() != nullptr);
    const auto invalidServerValue = server.apply(Http2SettingId::kEnablePush, 2);
    RUVIA_CHECK(invalidServerValue.failure() != nullptr);
    RUVIA_CHECK(invalidServerValue.failure()->error() ==
                Http2PeerSettingError::kInvalidEnablePush);

    Http2PeerSettings client(Http2Role::kClient);
    const auto clientDisabled = client.apply(Http2SettingId::kEnablePush, 0);
    RUVIA_CHECK(clientDisabled.applied() != nullptr);
    const auto invalidFromServer = client.apply(Http2SettingId::kEnablePush, 1);
    RUVIA_CHECK(invalidFromServer.failure() != nullptr);
    RUVIA_CHECK(invalidFromServer.failure()->error() ==
                Http2PeerSettingError::kInvalidEnablePush);
}

RUVIA_TEST(peer_settings_initial_window_size_and_delta) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto result = settings.apply(Http2SettingId::kInitialWindowSize, 100000);
    RUVIA_CHECK(result.initialWindowChange() != nullptr);
    RUVIA_CHECK_EQ(result.initialWindowChange()->delta(),
                   std::int64_t{100000} - kHttp2DefaultInitialWindowSize);
    RUVIA_CHECK_EQ(settings.initialWindowSize(), std::int32_t{100000});

    // Exactly 2^31-1 is allowed and reports the signed difference.
    Http2PeerSettings atMax(Http2Role::kServer);
    const auto max = atMax.apply(Http2SettingId::kInitialWindowSize, kInt32Max);
    RUVIA_CHECK(max.initialWindowChange() != nullptr);
    RUVIA_CHECK_EQ(max.initialWindowChange()->delta(),
                   static_cast<std::int64_t>(kInt32Max) - kHttp2DefaultInitialWindowSize);
    // One above is a flow-control error (RFC 9113 Section 6.5.2).
    Http2PeerSettings tooBig(Http2Role::kServer);
    const auto invalid = tooBig.apply(Http2SettingId::kInitialWindowSize, kInt32Max + 1);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == Http2PeerSettingError::kInvalidInitialWindow);
}

RUVIA_TEST(peer_settings_max_frame_size_bounds) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto minimum = settings.apply(
        Http2SettingId::kMaxFrameSize,
        kHttp2DefaultMaxFrameSize);
    RUVIA_CHECK(minimum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2DefaultMaxFrameSize);
    const auto maximum = settings.apply(
        Http2SettingId::kMaxFrameSize,
        kHttp2MaxFrameSizeLimit);
    RUVIA_CHECK(maximum.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.maxFrameSize(), kHttp2MaxFrameSizeLimit);
    // Below the 2^14 minimum and above the 2^24-1 maximum are rejected.
    const auto below = settings.apply(Http2SettingId::kMaxFrameSize, kHttp2DefaultMaxFrameSize - 1);
    RUVIA_CHECK(below.failure() != nullptr);
    RUVIA_CHECK(below.failure()->error() == Http2PeerSettingError::kInvalidMaxFrameSize);
    const auto above = settings.apply(Http2SettingId::kMaxFrameSize, kHttp2MaxFrameSizeLimit + 1);
    RUVIA_CHECK(above.failure() != nullptr);
    RUVIA_CHECK(above.failure()->error() == Http2PeerSettingError::kInvalidMaxFrameSize);
}

RUVIA_TEST(peer_settings_enable_connect_protocol_cannot_be_disabled) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto enabled =
        settings.apply(Http2SettingId::kEnableConnectProtocol, 1);
    RUVIA_CHECK(enabled.applied() != nullptr);
    RUVIA_CHECK(settings.enableConnectProtocol());
    // Once enabled it must never be turned off (RFC 8441).
    const auto disabled = settings.apply(Http2SettingId::kEnableConnectProtocol, 0);
    RUVIA_CHECK(disabled.failure() != nullptr);
    RUVIA_CHECK(disabled.failure()->error() ==
                Http2PeerSettingError::kInvalidEnableConnectProtocolTransition);
    // A non-boolean value is invalid.
    const auto invalid = settings.apply(Http2SettingId::kEnableConnectProtocol, 5);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() ==
                Http2PeerSettingError::kInvalidEnableConnectProtocol);

    // Setting 0 while already disabled is fine.
    Http2PeerSettings fresh(Http2Role::kServer);
    const auto remainsDisabled =
        fresh.apply(Http2SettingId::kEnableConnectProtocol, 0);
    RUVIA_CHECK(remainsDisabled.applied() != nullptr);
    RUVIA_CHECK(!fresh.enableConnectProtocol());
}

RUVIA_TEST(peer_settings_stored_values_and_unknown_ignored) {
    Http2PeerSettings settings(Http2Role::kServer);
    const auto maxConcurrent =
        settings.apply(Http2SettingId::kMaxConcurrentStreams, 250);
    RUVIA_CHECK(maxConcurrent.applied() != nullptr);
    RUVIA_CHECK_EQ(settings.maxConcurrentStreams(), std::uint32_t{250});
    const auto headerTable =
        settings.apply(Http2SettingId::kHeaderTableSize, 8192);
    RUVIA_CHECK(headerTable.applied() != nullptr);
    const auto headerList =
        settings.apply(Http2SettingId::kMaxHeaderListSize, 1000);
    RUVIA_CHECK(headerList.applied() != nullptr);
    // An unregistered setting id is ignored (RFC 9113 Section 6.5.2).
    const auto unknown =
        settings.apply(static_cast<Http2SettingId>(0x63), 999);
    RUVIA_CHECK(unknown.applied() != nullptr);
}

RUVIA_TEST(peer_settings_error_code_and_message_mapping) {
    // Only an invalid initial window is a flow-control error; the rest are protocol errors.
    RUVIA_CHECK(http2PeerSettingErrorCode(Http2PeerSettingError::kInvalidInitialWindow) ==
                Http2ErrorCode::kFlowControlError);
    RUVIA_CHECK(http2PeerSettingErrorCode(Http2PeerSettingError::kInvalidEnablePush) ==
                Http2ErrorCode::kProtocolError);
    RUVIA_CHECK(http2PeerSettingErrorCode(Http2PeerSettingError::kInvalidMaxFrameSize) ==
                Http2ErrorCode::kProtocolError);

    RUVIA_CHECK_EQ(http2PeerSettingErrorMessage(Http2PeerSettingError::kInvalidEnablePush),
                   std::string_view("invalid ENABLE_PUSH"));
    RUVIA_CHECK_EQ(http2PeerSettingErrorMessage(Http2PeerSettingError::kInvalidInitialWindow),
                   std::string_view("invalid initial window"));
    RUVIA_CHECK_EQ(http2PeerSettingErrorMessage(Http2PeerSettingError::kInvalidMaxFrameSize),
                   std::string_view("invalid max frame size"));
    RUVIA_CHECK_EQ(
        http2PeerSettingErrorMessage(Http2PeerSettingError::kInvalidEnableConnectProtocol),
        std::string_view("invalid ENABLE_CONNECT_PROTOCOL"));
    RUVIA_CHECK_EQ(
        http2PeerSettingErrorMessage(
            Http2PeerSettingError::kInvalidEnableConnectProtocolTransition),
        std::string_view("invalid ENABLE_CONNECT_PROTOCOL transition"));
}
