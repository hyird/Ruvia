#pragma once

#include "ruvia/memory/PmrResource.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

class HttpHeaderView final {
public:
    constexpr HttpHeaderView() noexcept = default;

    constexpr HttpHeaderView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

namespace detail {
struct MultipartPartAccess;
struct RequestNameValueViewAccess;
}  // namespace detail

class MultipartPart final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] std::string_view contentType() const noexcept {
        return contentType_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::MultipartPartAccess;

    constexpr MultipartPart(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body) noexcept
        : name_(name),
          filename_(filename),
          contentType_(contentType),
          body_(body) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view contentType_;
    std::string_view body_;
};

namespace detail {

struct MultipartPartAccess final {
    [[nodiscard]] static constexpr MultipartPart make(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body) noexcept {
        return MultipartPart(name, filename, contentType, body);
    }
};

}  // namespace detail

class RequestNameValueView final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    friend struct detail::RequestNameValueViewAccess;

    constexpr RequestNameValueView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    std::string_view name_;
    std::string_view value_;
};

namespace detail {

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }
};

}  // namespace detail

class RequestNameValueList final {
    struct Token final {
        explicit Token() = default;
    };

public:
    using value_type = RequestNameValueView;
    using iterator = std::pmr::vector<RequestNameValueView>::iterator;
    using const_iterator = std::pmr::vector<RequestNameValueView>::const_iterator;

    explicit RequestNameValueList(Token, std::pmr::memory_resource* resource = nullptr)
        : items_(detail::pmrResourceOrDefault(resource)) {}

    RequestNameValueList(const RequestNameValueList&) = delete;
    RequestNameValueList& operator=(const RequestNameValueList&) = delete;
    RequestNameValueList(RequestNameValueList&&) noexcept = default;
    RequestNameValueList& operator=(RequestNameValueList&&) noexcept = default;

    [[nodiscard]] const_iterator begin() const noexcept {
        return items_.begin();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return items_.cbegin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return items_.end();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return items_.cend();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] const RequestNameValueView* data() const noexcept {
        return items_.data();
    }

    [[nodiscard]] const RequestNameValueView& operator[](std::size_t index) const noexcept {
        return items_[index];
    }

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->name() == name) {
                return it->value();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        std::size_t result = 0;
        for (const auto& item : items_) {
            if (item.name() == name) {
                ++result;
            }
        }
        return result;
    }

    [[nodiscard]] std::span<const RequestNameValueView> entries() const noexcept {
        return std::span<const RequestNameValueView>(items_.data(), items_.size());
    }

private:
    friend class Context;

    void reserve(std::size_t count) {
        items_.reserve(count);
    }

    void push_back(RequestNameValueView value) {
        items_.push_back(value);
    }

    template <typename... Args>
    RequestNameValueView& emplace_back(Args&&... args) {
        return items_.emplace_back(std::forward<Args>(args)...);
    }

    std::pmr::vector<RequestNameValueView> items_;
};

class RequestValueGroup final {
    struct Token final {
        explicit Token() = default;
    };

public:
    RequestValueGroup(Token, std::pmr::memory_resource* resource, std::string_view name)
        : name_(name),
          values_(detail::pmrResourceOrDefault(resource)) {}

    RequestValueGroup(const RequestValueGroup&) = delete;
    RequestValueGroup& operator=(const RequestValueGroup&) = delete;
    RequestValueGroup(RequestValueGroup&&) noexcept = default;
    RequestValueGroup& operator=(RequestValueGroup&&) noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::span<const std::string_view> values() const noexcept {
        return std::span<const std::string_view>(values_.data(), values_.size());
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

private:
    friend class Context;

    void add(std::string_view value) {
        values_.push_back(value);
    }

    std::string_view name_;
    std::pmr::vector<std::string_view> values_;
};

class RequestValueGroupList final {
    struct Token final {
        explicit Token() = default;
    };

public:
    using value_type = RequestValueGroup;
    using iterator = std::pmr::vector<RequestValueGroup>::iterator;
    using const_iterator = std::pmr::vector<RequestValueGroup>::const_iterator;

    explicit RequestValueGroupList(Token, std::pmr::memory_resource* resource = nullptr)
        : groups_(detail::pmrResourceOrDefault(resource)) {}

    RequestValueGroupList(const RequestValueGroupList&) = delete;
    RequestValueGroupList& operator=(const RequestValueGroupList&) = delete;
    RequestValueGroupList(RequestValueGroupList&&) noexcept = default;
    RequestValueGroupList& operator=(RequestValueGroupList&&) noexcept = default;

    [[nodiscard]] const_iterator begin() const noexcept {
        return groups_.begin();
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return groups_.cbegin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return groups_.end();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return groups_.cend();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return groups_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return groups_.empty();
    }

    [[nodiscard]] const RequestValueGroup* data() const noexcept {
        return groups_.data();
    }

    [[nodiscard]] const RequestValueGroup& operator[](std::size_t index) const noexcept {
        return groups_[index];
    }

    [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        return requestGroup == nullptr ? 0 : requestGroup->size();
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        const auto* requestGroup = group(name);
        if (requestGroup == nullptr) {
            return {};
        }
        return requestGroup->values();
    }

    [[nodiscard]] std::span<const RequestValueGroup> entries() const noexcept {
        return std::span<const RequestValueGroup>(groups_.data(), groups_.size());
    }

private:
    friend class Context;

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    void push_back(RequestValueGroup value) {
        groups_.push_back(std::move(value));
    }

    template <typename... Args>
    RequestValueGroup& emplace_back(Args&&... args) {
        return groups_.emplace_back(std::forward<Args>(args)...);
    }

    [[nodiscard]] const RequestValueGroup* group(std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return &*it;
            }
        }
        return nullptr;
    }

    std::pmr::vector<RequestValueGroup> groups_;
};

HttpMethod parseMethod(std::string_view method);
std::string_view methodName(HttpMethod method);
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpStatusText(std::string_view value) noexcept;

}  // namespace ruvia
