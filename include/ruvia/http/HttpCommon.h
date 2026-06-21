#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

enum class HttpMethod {
    kGet,
    kPost,
    kPut,
    kDelete,
    kPatch,
    kHead,
    kOptions,
    kConnect,
    kUnknown
};

inline constexpr HttpMethod Get = HttpMethod::kGet;
inline constexpr HttpMethod Post = HttpMethod::kPost;
inline constexpr HttpMethod Put = HttpMethod::kPut;
inline constexpr HttpMethod Delete = HttpMethod::kDelete;
inline constexpr HttpMethod Patch = HttpMethod::kPatch;
inline constexpr HttpMethod Head = HttpMethod::kHead;
inline constexpr HttpMethod Options = HttpMethod::kOptions;
inline constexpr HttpMethod Connect = HttpMethod::kConnect;

enum class RequestBodyMode {
    kBuffered,
    kStream
};

enum class ResponseBodyMode {
    kBuffered,
    kStream,
    kSse,
    kWebSocket
};

inline constexpr std::size_t kMaxRequestHeaders = 64;

inline constexpr std::size_t kMaxRouteParams = 16;

struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

struct RouteParamView {
    std::string_view name;
    std::string_view value;
};

struct MultipartPart {
    std::pmr::string name;
    std::pmr::string filename;
    std::pmr::string contentType;
    std::string_view body;

    explicit MultipartPart(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
};

class RequestValue final {
public:
    enum class DecodeMode : std::uint8_t {
        kNone,
        kPercent,
        kForm
    };

    RequestValue() = default;
    RequestValue(
        std::optional<std::string_view> value,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        DecodeMode decodeMode = DecodeMode::kNone) noexcept
        : value_(value),
          resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
          decodeMode_(decodeMode) {}

    [[nodiscard]] bool exists() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] std::optional<std::string_view> toStringView() const noexcept {
        return value_;
    }

    [[nodiscard]] std::optional<std::pmr::string> toString() const;
    [[nodiscard]] std::optional<bool> toBool() const noexcept;
    [[nodiscard]] std::optional<int> toInt() const noexcept;
    [[nodiscard]] std::optional<unsigned int> toUInt() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> toInt32() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> toUInt32() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> toInt64() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> toUInt64() const noexcept;

private:
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_ == nullptr ? std::pmr::get_default_resource() : resource_;
    }

    std::optional<std::string_view> value_;
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    DecodeMode decodeMode_{DecodeMode::kNone};
};

using QueryValue = RequestValue;
using ParamValue = RequestValue;

HttpMethod parseMethod(std::string_view method);
std::string_view methodName(HttpMethod method);
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
