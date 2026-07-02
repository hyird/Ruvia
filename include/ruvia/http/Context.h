#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/MultipartReader.h"
#include "ruvia/http/Streaming.h"
#include "ruvia/http/ValidationTypes.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/http/detail/ContextValues.h"
#include "ruvia/http/detail/ValidatedValues.h"
#include "ruvia/memory/MemoryPool.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/redis/Redis.h"
#endif

#ifdef RUVIA_ENABLE_HTTP_CLIENT
#include "ruvia/http/HttpClient.h"
#endif

namespace ruvia {

class Context;
class ContextRequest;
class Env;
class StaticRoot;

#ifdef RUVIA_ENABLE_MARIADB
class DbHandle;
#endif
#ifdef RUVIA_ENABLE_REDIS
class RedisHandle;
#endif
namespace detail {
class DbRegistry;
class RedisRegistry;
class HttpClientRegistry;
class HttpClientPool;
class RateLimiter;
class RequestBodyLoader;
struct ContextAccess;
class ContextServices;
struct RouteRateLimitOptions;
struct RouteRateLimitResult;
RouteRateLimitResult checkRouteRateLimit(Context& context, const RouteRateLimitOptions& options) noexcept;
struct SessionAccess;
template <typename T>
void setValidatedBody(Context& context, ValidationTarget target, T&& body);
[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();
[[noreturn]] void throwInvalidQuery();
[[noreturn]] void throwInvalidParam();
[[noreturn]] void throwInvalidHeader();
[[noreturn]] void throwInvalidCookie();

// Assign `src` into `dst`, forcing storage in the backing memory resource rather
// than the small-string optimization's inline buffer. The Context's per-request
// arena outlives the Context, but a string object's inline SSO bytes do not — so
// without this, a short c.session()/c.req().text() value handed to c.text() (a borrowed
// view) would dangle once the Context is destroyed before the response is written.
// 32 clears every mainstream SSO threshold (libstdc++/MSVC 15, libc++ 22).
inline void assignStableString(std::pmr::string& dst, std::string_view src) {
    dst.clear();
    if (src.size() < 32) {
        dst.reserve(32);
    }
    dst.assign(src.data(), src.size());
}
}

class ContextRequest final {
public:
    struct ParseBodyOptions final {
        bool all{false};
        bool dot{false};
    };

    enum class MatchedRouteKind {
        kMiddleware,
        kHandler
    };

    struct MatchedRoute final {
        std::string_view method;
        std::string_view path;
        MatchedRouteKind kind{MatchedRouteKind::kHandler};
    };

    class RequestBlob final {
    public:
        constexpr RequestBlob(std::span<const std::byte> bytes, std::string_view type) noexcept
            : bytes_(bytes), type_(type) {}

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::span<const std::byte> arrayBuffer() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::string_view text() const noexcept {
            return std::string_view(
                reinterpret_cast<const char*>(bytes_.data()),
                bytes_.size());
        }

        [[nodiscard]] std::string_view type() const noexcept {
            return type_;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return bytes_.empty();
        }

    private:
        std::span<const std::byte> bytes_;
        std::string_view type_;
    };

    struct RequestFormField;
    class RequestFormData;

    class RawRequestClone final {
    public:
        class Header final {
        public:
            Header(
                std::pmr::memory_resource* resource,
                std::string_view name,
                std::string_view value)
                : name_(name, resource),
                  value_(value, resource) {}

            Header(const Header&) = delete;
            Header& operator=(const Header&) = delete;
            Header(Header&&) noexcept = default;
            Header& operator=(Header&&) noexcept = default;

            [[nodiscard]] std::string_view name() const noexcept {
                return std::string_view(name_.data(), name_.size());
            }

            [[nodiscard]] std::string_view value() const noexcept {
                return std::string_view(value_.data(), value_.size());
            }

        private:
            std::pmr::string name_;
            std::pmr::string value_;
        };

        explicit RawRequestClone(std::pmr::memory_resource* resource)
            : target_(resource),
              url_(resource),
              path_(resource),
              queryString_(resource),
              httpVersion_(resource),
              headers_(resource),
              body_(resource),
              remoteAddress_(resource),
              clientCertificate_(resource) {}

        RawRequestClone(const RawRequestClone&) = delete;
        RawRequestClone& operator=(const RawRequestClone&) = delete;
        RawRequestClone(RawRequestClone&&) noexcept = default;
        RawRequestClone& operator=(RawRequestClone&&) noexcept = default;

        [[nodiscard]] std::string_view method() const noexcept {
            return methodName(method_);
        }

        [[nodiscard]] HttpMethod methodEnum() const noexcept {
            return method_;
        }

        [[nodiscard]] std::string_view target() const noexcept {
            return std::string_view(target_.data(), target_.size());
        }

        [[nodiscard]] std::string_view url() const noexcept {
            return std::string_view(url_.data(), url_.size());
        }

        [[nodiscard]] std::string_view path() const noexcept {
            return std::string_view(path_.data(), path_.size());
        }

        [[nodiscard]] std::string_view queryString() const noexcept {
            return std::string_view(queryString_.data(), queryString_.size());
        }

        [[nodiscard]] std::string_view httpVersion() const noexcept {
            return std::string_view(httpVersion_.data(), httpVersion_.size());
        }

        [[nodiscard]] std::span<const Header> headers() const noexcept {
            return headers_;
        }

        [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

        [[nodiscard]] std::string_view body() const noexcept {
            return std::string_view(body_.data(), body_.size());
        }

        [[nodiscard]] std::string_view text() const noexcept {
            return body();
        }

        [[nodiscard]] std::span<const std::byte> arrayBuffer() const noexcept {
            return std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(body_.data()),
                body_.size());
        }

        [[nodiscard]] RequestBlob blob() const noexcept {
            return RequestBlob(arrayBuffer(), header("Content-Type"));
        }

        [[nodiscard]] RequestFormData parseBody(ParseBodyOptions options = {}) const;

        [[nodiscard]] RequestFormData formData() const;

        template <typename T>
        [[nodiscard]] T json() const {
            static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_MODEL");
            auto parsed = JsonBody<T>::parse(body(), body_.get_allocator().resource());
            if (!parsed) {
                detail::throwInvalidJsonBody();
            }
            return std::move(*parsed);
        }

        [[nodiscard]] std::string_view remoteAddress() const noexcept {
            return std::string_view(remoteAddress_.data(), remoteAddress_.size());
        }

        [[nodiscard]] std::string_view clientCertificate() const noexcept {
            return std::string_view(clientCertificate_.data(), clientCertificate_.size());
        }

        [[nodiscard]] bool isSecure() const noexcept {
            return secure_;
        }

    private:
        friend class ContextRequest;

        HttpMethod method_{HttpMethod::kUnknown};
        std::pmr::string target_;
        std::pmr::string url_;
        std::pmr::string path_;
        std::pmr::string queryString_;
        std::pmr::string httpVersion_;
        std::pmr::vector<Header> headers_;
        std::pmr::string body_;
        std::pmr::string remoteAddress_;
        std::pmr::string clientCertificate_;
        bool secure_{false};
    };

    struct RequestFormField final {
        RequestFormField(
            std::pmr::memory_resource* resource,
            std::pmr::string&& fieldName,
            std::pmr::string&& fieldValue,
            std::pmr::string&& fieldFilename = {},
            std::pmr::string&& fieldContentType = {},
            bool fieldFile = false,
            bool fieldArray = false)
            : name(std::move(fieldName)),
              value(std::move(fieldValue)),
              filename(std::move(fieldFilename)),
              contentType(std::move(fieldContentType)),
              path(resource),
              file(fieldFile),
              array(fieldArray) {}

        std::pmr::string name;
        std::pmr::string value;
        std::pmr::string filename;
        std::pmr::string contentType;
        std::pmr::vector<std::pmr::string> path;
        bool file{false};
        bool array{false};

        [[nodiscard]] bool isFile() const noexcept {
            return file;
        }

        [[nodiscard]] bool isArray() const noexcept {
            return array;
        }

        [[nodiscard]] std::string_view text() const noexcept {
            return std::string_view(value.data(), value.size());
        }

        [[nodiscard]] std::span<const std::byte> arrayBuffer() const noexcept {
            return std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(value.data()),
                value.size());
        }

        [[nodiscard]] RequestBlob blob() const noexcept {
            return RequestBlob(arrayBuffer(), mediaType());
        }

        [[nodiscard]] std::string_view fileName() const noexcept {
            return std::string_view(filename.data(), filename.size());
        }

        [[nodiscard]] std::string_view mediaType() const noexcept {
            return std::string_view(contentType.data(), contentType.size());
        }
    };

    class RequestFormData final {
    public:
        enum class SingleValueSelection : unsigned char {
            kFirst,
            kLast
        };

        class Entry final {
        public:
            Entry(
                std::pmr::memory_resource* resource,
                std::string_view name,
                bool array,
                SingleValueSelection singleValueSelection = SingleValueSelection::kLast)
                : name_(name),
                  fields_(resource),
                  singleValueSelection_(singleValueSelection),
                  array_(array) {}

            [[nodiscard]] std::string_view name() const noexcept {
                return name_;
            }

            [[nodiscard]] const RequestFormField* field() const noexcept {
                if (fields_.empty()) {
                    return nullptr;
                }
                return singleValueSelection_ == SingleValueSelection::kFirst
                    ? fields_.front()
                    : fields_.back();
            }

            [[nodiscard]] std::span<const RequestFormField* const> fields() const noexcept {
                return std::span<const RequestFormField* const>(fields_.data(), fields_.size());
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return fields_.size();
            }

            [[nodiscard]] bool empty() const noexcept {
                return fields_.empty();
            }

            [[nodiscard]] bool array() const noexcept {
                return array_ || multiple();
            }

            [[nodiscard]] bool multiple() const noexcept {
                return fields_.size() > 1;
            }

            [[nodiscard]] std::optional<std::string_view> value() const noexcept {
                const auto* const selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return std::string_view(selected->value.data(), selected->value.size());
            }

            [[nodiscard]] std::pmr::vector<std::string_view> values() const {
                std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
                result.reserve(fields_.size());
                for (const auto* field : fields_) {
                    result.emplace_back(field->value.data(), field->value.size());
                }
                return result;
            }

        private:
            friend class RequestFormData;

            void add(const RequestFormField& field) {
                fields_.push_back(&field);
                array_ = array_ || field.array;
            }

            std::string_view name_;
            std::pmr::vector<const RequestFormField*> fields_;
            SingleValueSelection singleValueSelection_{SingleValueSelection::kLast};
            bool array_{false};
        };

        class Value final {
        public:
            Value(std::pmr::memory_resource* resource, const Entry* entry) noexcept
                : resource_(resource),
                  entry_(entry) {}

            [[nodiscard]] explicit operator bool() const noexcept {
                return exists();
            }

            [[nodiscard]] bool exists() const noexcept {
                return field() != nullptr;
            }

            [[nodiscard]] const RequestFormField* field() const noexcept {
                return entry_ == nullptr ? nullptr : entry_->field();
            }

            [[nodiscard]] const RequestFormField* operator->() const noexcept {
                return field();
            }

            [[nodiscard]] std::span<const RequestFormField* const> fields() const noexcept {
                return entry_ == nullptr ? std::span<const RequestFormField* const>{} : entry_->fields();
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return entry_ == nullptr ? 0 : entry_->size();
            }

            [[nodiscard]] bool empty() const noexcept {
                return size() == 0;
            }

            [[nodiscard]] bool multiple() const noexcept {
                return entry_ != nullptr && entry_->multiple();
            }

            [[nodiscard]] bool isArray() const noexcept {
                return entry_ != nullptr && entry_->array();
            }

            [[nodiscard]] bool isFile() const noexcept {
                const auto* selected = field();
                return selected != nullptr && selected->isFile();
            }

            [[nodiscard]] std::optional<std::string_view> text() const noexcept {
                return entry_ == nullptr ? std::nullopt : entry_->value();
            }

            [[nodiscard]] std::optional<std::string_view> value() const noexcept {
                return text();
            }

            [[nodiscard]] std::string_view value_or(std::string_view fallback) const noexcept {
                if (auto result = value()) {
                    return *result;
                }
                return fallback;
            }

            [[nodiscard]] std::pmr::vector<std::string_view> texts() const {
                if (entry_ == nullptr) {
                    return std::pmr::vector<std::string_view>(resource());
                }
                return entry_->values();
            }

            [[nodiscard]] std::pmr::vector<std::string_view> values() const {
                return texts();
            }

            [[nodiscard]] std::optional<RequestBlob> blob() const noexcept {
                const auto* selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return selected->blob();
            }

            [[nodiscard]] std::optional<std::string_view> fileName() const noexcept {
                const auto* selected = field();
                if (selected == nullptr || !selected->isFile()) {
                    return std::nullopt;
                }
                return selected->fileName();
            }

            [[nodiscard]] std::optional<std::string_view> mediaType() const noexcept {
                const auto* selected = field();
                if (selected == nullptr || !selected->isFile()) {
                    return std::nullopt;
                }
                return selected->mediaType();
            }

        private:
            [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
                return resource_ == nullptr ? std::pmr::get_default_resource() : resource_;
            }

            std::pmr::memory_resource* resource_{nullptr};
            const Entry* entry_{nullptr};
        };

        class PathValue final {
        public:
            explicit PathValue(
                std::pmr::vector<const RequestFormField*>&& fields,
                SingleValueSelection singleValueSelection = SingleValueSelection::kLast)
                : fields_(std::move(fields)),
                  singleValueSelection_(singleValueSelection) {
                for (const auto* field : fields_) {
                    array_ = array_ || (field != nullptr && field->array);
                }
            }

            [[nodiscard]] explicit operator bool() const noexcept {
                return exists();
            }

            [[nodiscard]] bool exists() const noexcept {
                return field() != nullptr;
            }

            [[nodiscard]] const RequestFormField* field() const noexcept {
                if (fields_.empty()) {
                    return nullptr;
                }
                return singleValueSelection_ == SingleValueSelection::kFirst
                    ? fields_.front()
                    : fields_.back();
            }

            [[nodiscard]] const RequestFormField* operator->() const noexcept {
                return field();
            }

            [[nodiscard]] std::span<const RequestFormField* const> fields() const noexcept {
                return std::span<const RequestFormField* const>(fields_.data(), fields_.size());
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return fields_.size();
            }

            [[nodiscard]] bool empty() const noexcept {
                return fields_.empty();
            }

            [[nodiscard]] bool multiple() const noexcept {
                return fields_.size() > 1;
            }

            [[nodiscard]] bool isArray() const noexcept {
                return array_ || multiple();
            }

            [[nodiscard]] bool isFile() const noexcept {
                const auto* selected = field();
                return selected != nullptr && selected->isFile();
            }

            [[nodiscard]] std::optional<std::string_view> text() const noexcept {
                const auto* selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return std::string_view(selected->value.data(), selected->value.size());
            }

            [[nodiscard]] std::optional<std::string_view> value() const noexcept {
                return text();
            }

            [[nodiscard]] std::string_view value_or(std::string_view fallback) const noexcept {
                if (auto result = value()) {
                    return *result;
                }
                return fallback;
            }

            [[nodiscard]] std::pmr::vector<std::string_view> texts() const {
                std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
                result.reserve(fields_.size());
                for (const auto* field : fields_) {
                    if (field != nullptr) {
                        result.emplace_back(field->value.data(), field->value.size());
                    }
                }
                return result;
            }

            [[nodiscard]] std::pmr::vector<std::string_view> values() const {
                return texts();
            }

            [[nodiscard]] std::optional<RequestBlob> blob() const noexcept {
                const auto* selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return selected->blob();
            }

            [[nodiscard]] std::optional<std::string_view> fileName() const noexcept {
                const auto* selected = field();
                if (selected == nullptr || !selected->isFile()) {
                    return std::nullopt;
                }
                return selected->fileName();
            }

            [[nodiscard]] std::optional<std::string_view> mediaType() const noexcept {
                const auto* selected = field();
                if (selected == nullptr || !selected->isFile()) {
                    return std::nullopt;
                }
                return selected->mediaType();
            }

        private:
            std::pmr::vector<const RequestFormField*> fields_;
            SingleValueSelection singleValueSelection_{SingleValueSelection::kLast};
            bool array_{false};
        };

        class Object final {
        public:
            Object(const RequestFormData* form, std::string_view dotPath)
                : form_(form),
                  dotPath_(dotPath, resourceFor(form)),
                  entries_(resourceFor(form)) {
                rebuildEntries();
            }

            [[nodiscard]] PathValue operator[](std::string_view name) const {
                return at(name);
            }

            [[nodiscard]] PathValue at(std::string_view name) const {
                if (hasNestedName(name)) {
                    if (form_ == nullptr) {
                        return emptyPathValue();
                    }
                    return PathValue(form_->getAllAtChild(path(), name), singleValueSelection());
                }
                return PathValue(fieldsForName(name), singleValueSelection());
            }

            [[nodiscard]] PathValue get(std::string_view name) const {
                return at(name);
            }

            [[nodiscard]] PathValue getAll(std::string_view name) const {
                return at(name);
            }

            [[nodiscard]] std::optional<std::string_view> value(std::string_view name) const noexcept {
                if (hasNestedName(name)) {
                    return form_ == nullptr ? std::nullopt : form_->valueAtChild(path(), name);
                }
                const auto* formEntry = entry(name);
                return formEntry == nullptr ? std::nullopt : formEntry->value();
            }

            [[nodiscard]] std::pmr::vector<std::string_view> values(std::string_view name) const {
                if (hasNestedName(name)) {
                    if (form_ == nullptr) {
                        return std::pmr::vector<std::string_view>(resource());
                    }
                    return form_->valuesAtChild(path(), name);
                }
                std::pmr::vector<std::string_view> result(resource());
                const auto* formEntry = entry(name);
                if (formEntry == nullptr) {
                    return result;
                }
                return formEntry->values();
            }

            [[nodiscard]] bool has(std::string_view name) const noexcept {
                return count(name) != 0;
            }

            [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
                if (hasNestedName(name)) {
                    return form_ == nullptr ? 0 : form_->countAtChild(path(), name);
                }
                const auto* formEntry = entry(name);
                return formEntry == nullptr ? 0 : formEntry->size();
            }

            [[nodiscard]] std::span<const Entry> entries() const noexcept {
                return std::span<const Entry>(entries_.data(), entries_.size());
            }

            [[nodiscard]] std::span<const Entry> groups() const noexcept {
                return entries();
            }

            [[nodiscard]] std::pmr::vector<std::string_view> keys() const {
                std::pmr::vector<std::string_view> result(resource());
                result.reserve(entries_.size());
                for (const auto& formEntry : entries_) {
                    result.push_back(formEntry.name());
                }
                return result;
            }

            [[nodiscard]] Object object(std::string_view name) const {
                if (path().empty()) {
                    return Object(form_, name);
                }

                std::pmr::string childPath(resource());
                childPath.reserve(path().size() + (name.empty() ? 0 : 1 + name.size()));
                childPath.append(path());
                if (!name.empty()) {
                    childPath.push_back('.');
                    childPath.append(name);
                }
                return Object(form_, std::string_view(childPath.data(), childPath.size()));
            }

        private:
            [[nodiscard]] static std::pmr::memory_resource* resourceFor(
                const RequestFormData* form) noexcept {
                if (form == nullptr) {
                    return std::pmr::get_default_resource();
                }
                return form->fields_.get_allocator().resource();
            }

            [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
                return entries_.get_allocator().resource();
            }

            [[nodiscard]] std::string_view path() const noexcept {
                return std::string_view(dotPath_.data(), dotPath_.size());
            }

            [[nodiscard]] static bool hasNestedName(std::string_view name) noexcept {
                return name.find('.') != std::string_view::npos;
            }

            [[nodiscard]] PathValue emptyPathValue() const {
                return PathValue(
                    std::pmr::vector<const RequestFormField*>(resource()),
                    singleValueSelection());
            }

            [[nodiscard]] SingleValueSelection singleValueSelection() const noexcept {
                return form_ == nullptr ? SingleValueSelection::kLast : form_->singleValueSelection_;
            }

            [[nodiscard]] const Entry* entry(std::string_view name) const noexcept {
                for (const auto& formEntry : entries_) {
                    if (formEntry.name() == name) {
                        return &formEntry;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] std::pmr::vector<const RequestFormField*> fieldsForName(std::string_view name) const {
                if (hasNestedName(name)) {
                    if (form_ == nullptr) {
                        return std::pmr::vector<const RequestFormField*>(resource());
                    }
                    return form_->getAllAtChild(path(), name);
                }

                std::pmr::vector<const RequestFormField*> result(resource());
                const auto* formEntry = entry(name);
                if (formEntry == nullptr) {
                    return result;
                }
                result.reserve(formEntry->size());
                for (const auto* field : formEntry->fields()) {
                    result.push_back(field);
                }
                return result;
            }

            [[nodiscard]] static std::string_view directChildName(
                const RequestFormField& field,
                std::string_view dotPath) noexcept {
                if (field.path.empty()) {
                    return {};
                }

                std::size_t index = 0;
                if (!consumePath(field, index, dotPath) || index >= field.path.size() ||
                    index + 1 != field.path.size()) {
                    return {};
                }

                const auto& child = field.path[index];
                return std::string_view(child.data(), child.size());
            }

            void rebuildEntries() {
                entries_.clear();
                if (form_ == nullptr) {
                    return;
                }

                auto* const currentResource = resource();
                std::pmr::vector<std::size_t> order(currentResource);
                order.reserve(form_->fields_.size());
                for (std::size_t i = 0; i < form_->fields_.size(); ++i) {
                    if (!directChildName(form_->fields_[i], path()).empty()) {
                        order.push_back(i);
                    }
                }
                if (order.empty()) {
                    return;
                }

                std::stable_sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) noexcept {
                    const auto leftName = directChildName(form_->fields_[left], path());
                    const auto rightName = directChildName(form_->fields_[right], path());
                    if (leftName == rightName) {
                        return left < right;
                    }
                    return leftName < rightName;
                });

                struct EntryBuild final {
                    std::size_t firstIndex;
                    std::size_t begin;
                    std::size_t end;
                };
                std::pmr::vector<EntryBuild> builds(currentResource);
                builds.reserve(order.size());
                for (std::size_t offset = 0; offset < order.size();) {
                    const auto begin = offset;
                    const auto firstIndex = order[offset];
                    const auto name = directChildName(form_->fields_[firstIndex], path());
                    do {
                        ++offset;
                    } while (offset < order.size() &&
                        directChildName(form_->fields_[order[offset]], path()) == name);
                    builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
                }
                std::stable_sort(builds.begin(), builds.end(), [](const EntryBuild& left, const EntryBuild& right) noexcept {
                    return left.firstIndex < right.firstIndex;
                });

                entries_.reserve(builds.size());
                for (const auto& build : builds) {
                    auto& formEntry = entries_.emplace_back(
                        currentResource,
                        directChildName(form_->fields_[build.firstIndex], path()),
                        false,
                        singleValueSelection());
                    for (std::size_t i = build.begin; i < build.end; ++i) {
                        formEntry.add(form_->fields_[order[i]]);
                    }
                }
            }

            const RequestFormData* form_{nullptr};
            std::pmr::string dotPath_;
            std::pmr::vector<Entry> entries_;
        };

        explicit RequestFormData(
            std::pmr::memory_resource* resource,
            SingleValueSelection singleValueSelection = SingleValueSelection::kLast)
            : singleValueSelection_(singleValueSelection),
              fields_(resource),
              entries_(resource),
              pathEntries_(resource) {}

        explicit RequestFormData(
            std::pmr::vector<RequestFormField>&& fields,
            SingleValueSelection singleValueSelection = SingleValueSelection::kLast)
            : singleValueSelection_(singleValueSelection),
              fields_(std::move(fields)),
              entries_(fields_.get_allocator().resource()),
              pathEntries_(fields_.get_allocator().resource()) {
            rebuildEntries();
        }

        [[nodiscard]] std::span<const RequestFormField> fields() const noexcept {
            return fields_;
        }

        [[nodiscard]] std::span<const RequestFormField> entries() const noexcept {
            return fields_;
        }

        [[nodiscard]] std::span<const Entry> groups() const noexcept {
            return entries_;
        }

        [[nodiscard]] std::pmr::vector<std::string_view> keys() const {
            std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
            result.reserve(entries_.size());
            for (const auto& entry : entries_) {
                result.push_back(entry.name());
            }
            return result;
        }

        [[nodiscard]] std::span<const RequestFormField> values() const noexcept {
            return fields_;
        }

        [[nodiscard]] const Entry* entry(std::string_view name) const noexcept {
            if (isPathName(name)) {
                if (const auto* formEntry = pathEntry(name)) {
                    return formEntry;
                }
            }
            for (const auto& entry : entries_) {
                if (entry.name() == name) {
                    return &entry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const RequestFormField* field(std::string_view name) const noexcept {
            const auto* formEntry = entry(name);
            if (formEntry == nullptr) {
                return nullptr;
            }
            return formEntry->field();
        }

        [[nodiscard]] std::pmr::vector<const RequestFormField*> fields(std::string_view name) const {
            std::pmr::vector<const RequestFormField*> result(fields_.get_allocator().resource());
            const auto* formEntry = entry(name);
            if (formEntry == nullptr) {
                return result;
            }
            result.reserve(formEntry->size());
            for (const auto* field : formEntry->fields()) {
                result.push_back(field);
            }
            return result;
        }

        [[nodiscard]] Value get(std::string_view name) const noexcept {
            return (*this)[name];
        }

        [[nodiscard]] Value getAll(std::string_view name) const noexcept {
            return (*this)[name];
        }

        [[nodiscard]] Value operator[](std::string_view name) const noexcept {
            return Value(fields_.get_allocator().resource(), entry(name));
        }

        [[nodiscard]] PathValue at(std::string_view dotPath) const {
            return PathValue(getAllAt(dotPath), singleValueSelection_);
        }

        [[nodiscard]] Object object(std::string_view dotPath) const {
            return Object(this, dotPath);
        }

        [[nodiscard]] bool has(std::string_view name) const noexcept {
            return entry(name) != nullptr;
        }

        [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
            const auto* formEntry = entry(name);
            return formEntry == nullptr ? 0 : formEntry->size();
        }

        [[nodiscard]] bool isArray(std::string_view name) const noexcept {
            const auto* formEntry = entry(name);
            return formEntry != nullptr && formEntry->array();
        }

        [[nodiscard]] std::optional<std::string_view> value(std::string_view name) const noexcept {
            const auto* formEntry = entry(name);
            if (formEntry == nullptr) {
                return std::nullopt;
            }
            return formEntry->value();
        }

        [[nodiscard]] std::pmr::vector<std::string_view> values(std::string_view name) const {
            std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
            const auto* formEntry = entry(name);
            if (formEntry == nullptr) {
                return result;
            }
            return formEntry->values();
        }

        [[nodiscard]] const RequestFormField* getAt(std::string_view dotPath) const noexcept {
            const auto* formEntry = pathEntry(dotPath);
            return formEntry == nullptr ? nullptr : formEntry->field();
        }

        [[nodiscard]] bool hasAt(std::string_view dotPath) const noexcept {
            return pathEntry(dotPath) != nullptr;
        }

        [[nodiscard]] std::size_t countAt(std::string_view dotPath) const noexcept {
            const auto* formEntry = pathEntry(dotPath);
            return formEntry == nullptr ? 0 : formEntry->size();
        }

        [[nodiscard]] bool isArrayAt(std::string_view dotPath) const noexcept {
            const auto* formEntry = pathEntry(dotPath);
            return formEntry != nullptr && formEntry->array();
        }

        [[nodiscard]] std::optional<std::string_view> valueAt(std::string_view dotPath) const noexcept {
            const auto* field = getAt(dotPath);
            if (field == nullptr) {
                return std::nullopt;
            }
            return std::string_view(field->value.data(), field->value.size());
        }

        [[nodiscard]] std::pmr::vector<std::string_view> valuesAt(std::string_view dotPath) const {
            std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
            const auto* formEntry = pathEntry(dotPath);
            if (formEntry == nullptr) {
                return result;
            }
            return formEntry->values();
        }

        [[nodiscard]] std::pmr::vector<const RequestFormField*> getAllAt(std::string_view dotPath) const {
            std::pmr::vector<const RequestFormField*> result(fields_.get_allocator().resource());
            const auto* formEntry = pathEntry(dotPath);
            if (formEntry == nullptr) {
                return result;
            }
            result.reserve(formEntry->size());
            for (const auto* field : formEntry->fields()) {
                result.push_back(field);
            }
            return result;
        }

    private:
        [[nodiscard]] static bool isPathName(std::string_view name) noexcept {
            return name.find('.') != std::string_view::npos;
        }

        [[nodiscard]] std::pmr::vector<const RequestFormField*> getAllAtChild(
            std::string_view dotPath,
            std::string_view name) const {
            std::pmr::vector<const RequestFormField*> result(fields_.get_allocator().resource());
            const auto* formEntry = pathEntryChild(dotPath, name);
            if (formEntry == nullptr) {
                return result;
            }
            result.reserve(formEntry->size());
            for (const auto* field : formEntry->fields()) {
                result.push_back(field);
            }
            return result;
        }

        [[nodiscard]] const RequestFormField* getAtChild(
            std::string_view dotPath,
            std::string_view name) const noexcept {
            const auto* formEntry = pathEntryChild(dotPath, name);
            return formEntry == nullptr ? nullptr : formEntry->field();
        }

        [[nodiscard]] std::optional<std::string_view> valueAtChild(
            std::string_view dotPath,
            std::string_view name) const noexcept {
            const auto* field = getAtChild(dotPath, name);
            if (field == nullptr) {
                return std::nullopt;
            }
            return std::string_view(field->value.data(), field->value.size());
        }

        [[nodiscard]] std::pmr::vector<std::string_view> valuesAtChild(
            std::string_view dotPath,
            std::string_view name) const {
            std::pmr::vector<std::string_view> result(fields_.get_allocator().resource());
            const auto* formEntry = pathEntryChild(dotPath, name);
            if (formEntry == nullptr) {
                return result;
            }
            return formEntry->values();
        }

        [[nodiscard]] static bool consumePath(
            const RequestFormField& field,
            std::size_t& index,
            std::string_view dotPath) noexcept {
            if (dotPath.empty()) {
                return true;
            }

            std::size_t offset = 0;
            while (true) {
                const auto dot = dotPath.find('.', offset);
                const auto segment = dot == std::string_view::npos
                    ? dotPath.substr(offset)
                    : dotPath.substr(offset, dot - offset);
                if (segment.empty() || index >= field.path.size()) {
                    return false;
                }
                const auto stored = std::string_view(field.path[index].data(), field.path[index].size());
                if (stored != segment) {
                    return false;
                }
                ++index;
                if (dot == std::string_view::npos) {
                    return true;
                }
                offset = dot + 1;
            }
        }

        [[nodiscard]] static bool pathMatches(
            const RequestFormField& field,
            std::string_view dotPath) noexcept {
            if (field.path.empty() || dotPath.empty()) {
                return false;
            }

            std::size_t index = 0;
            return consumePath(field, index, dotPath) && index == field.path.size();
        }

        [[nodiscard]] static bool pathMatchesChild(
            const RequestFormField& field,
            std::string_view dotPath,
            std::string_view name) noexcept {
            if (field.path.empty() || name.empty()) {
                return false;
            }

            std::size_t index = 0;
            return consumePath(field, index, dotPath) &&
                consumePath(field, index, name) &&
                index == field.path.size();
        }

        [[nodiscard]] const Entry* pathEntry(std::string_view dotPath) const noexcept {
            if (dotPath.empty()) {
                return nullptr;
            }
            for (const auto& formEntry : pathEntries_) {
                if (formEntry.name() == dotPath) {
                    return &formEntry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const Entry* pathEntryChild(
            std::string_view dotPath,
            std::string_view name) const noexcept {
            if (name.empty()) {
                return nullptr;
            }
            for (const auto& formEntry : pathEntries_) {
                if (pathNameMatchesChild(formEntry.name(), dotPath, name)) {
                    return &formEntry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static bool pathNameMatchesChild(
            std::string_view pathName,
            std::string_view dotPath,
            std::string_view name) noexcept {
            if (name.empty()) {
                return false;
            }
            if (dotPath.empty()) {
                return pathName == name;
            }
            return pathName.size() == dotPath.size() + 1 + name.size() &&
                pathName.substr(0, dotPath.size()) == dotPath &&
                pathName[dotPath.size()] == '.' &&
                pathName.substr(dotPath.size() + 1) == name;
        }

        [[nodiscard]] std::size_t countAtChild(
            std::string_view dotPath,
            std::string_view name) const noexcept {
            const auto* formEntry = pathEntryChild(dotPath, name);
            return formEntry == nullptr ? 0 : formEntry->size();
        }

        void rebuildEntries() {
            entries_.clear();
            pathEntries_.clear();
            if (fields_.empty()) {
                return;
            }

            auto* const resource = fields_.get_allocator().resource();
            std::pmr::vector<std::size_t> order(resource);
            order.reserve(fields_.size());
            for (std::size_t i = 0; i < fields_.size(); ++i) {
                order.push_back(i);
            }
            std::stable_sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) noexcept {
                const auto leftName = entryName(fields_[left]);
                const auto rightName = entryName(fields_[right]);
                if (leftName == rightName) {
                    return left < right;
                }
                return leftName < rightName;
            });

            struct EntryBuild final {
                std::size_t firstIndex;
                std::size_t begin;
                std::size_t end;
            };
            std::pmr::vector<EntryBuild> builds(resource);
            builds.reserve(order.size());
            for (std::size_t offset = 0; offset < order.size();) {
                const auto begin = offset;
                const auto firstIndex = order[offset];
                const auto name = entryName(fields_[firstIndex]);
                do {
                    ++offset;
                } while (offset < order.size() &&
                    entryName(fields_[order[offset]]) == name);
                builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
            }
            std::stable_sort(builds.begin(), builds.end(), [](const EntryBuild& left, const EntryBuild& right) noexcept {
                return left.firstIndex < right.firstIndex;
            });

            entries_.reserve(builds.size());
            for (const auto& build : builds) {
                auto& entry = entries_.emplace_back(
                    resource,
                    entryName(fields_[build.firstIndex]),
                    false,
                    singleValueSelection_);
                for (std::size_t i = build.begin; i < build.end; ++i) {
                    entry.add(fields_[order[i]]);
                }
            }

            rebuildPathEntries(resource);
        }

        [[nodiscard]] static std::string_view entryName(const RequestFormField& field) noexcept {
            if (!field.path.empty()) {
                const auto& name = field.path.front();
                return std::string_view(name.data(), name.size());
            }
            return std::string_view(field.name.data(), field.name.size());
        }

        void rebuildPathEntries(std::pmr::memory_resource* resource) {
            std::pmr::vector<std::size_t> order(resource);
            order.reserve(fields_.size());
            for (std::size_t i = 0; i < fields_.size(); ++i) {
                if (!fields_[i].path.empty()) {
                    order.push_back(i);
                }
            }
            if (order.empty()) {
                return;
            }

            std::stable_sort(order.begin(), order.end(), [this](std::size_t left, std::size_t right) noexcept {
                const auto leftName = pathEntryName(fields_[left]);
                const auto rightName = pathEntryName(fields_[right]);
                if (leftName == rightName) {
                    return left < right;
                }
                return leftName < rightName;
            });

            struct EntryBuild final {
                std::size_t firstIndex;
                std::size_t begin;
                std::size_t end;
            };
            std::pmr::vector<EntryBuild> builds(resource);
            builds.reserve(order.size());
            for (std::size_t offset = 0; offset < order.size();) {
                const auto begin = offset;
                const auto firstIndex = order[offset];
                const auto name = pathEntryName(fields_[firstIndex]);
                do {
                    ++offset;
                } while (offset < order.size() &&
                    pathEntryName(fields_[order[offset]]) == name);
                builds.push_back(EntryBuild{.firstIndex = firstIndex, .begin = begin, .end = offset});
            }
            std::stable_sort(builds.begin(), builds.end(), [](const EntryBuild& left, const EntryBuild& right) noexcept {
                return left.firstIndex < right.firstIndex;
            });

            pathEntries_.reserve(builds.size());
            for (const auto& build : builds) {
                auto& entry = pathEntries_.emplace_back(
                    resource,
                    pathEntryName(fields_[build.firstIndex]),
                    false,
                    singleValueSelection_);
                for (std::size_t i = build.begin; i < build.end; ++i) {
                    entry.add(fields_[order[i]]);
                }
            }
        }

        [[nodiscard]] static std::string_view pathEntryName(const RequestFormField& field) noexcept {
            return std::string_view(field.name.data(), field.name.size());
        }

        SingleValueSelection singleValueSelection_{SingleValueSelection::kLast};
        std::pmr::vector<RequestFormField> fields_;
        std::pmr::vector<Entry> entries_;
        std::pmr::vector<Entry> pathEntries_;
    };

    [[nodiscard]] const HttpRequest& raw() const noexcept;

    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] std::pmr::string url() const;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] const RequestNameValueList& header() const;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;
    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& query() const;
    [[nodiscard]] std::optional<std::span<const std::string_view>> queries(std::string_view name) const;
    [[nodiscard]] const RequestValueGroupList& queries() const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& cookie() const;
    // Verifies the "value.signature" format written by setSignedCookie; returns
    // the value view on a valid signature, nullopt when missing or tampered.
    [[nodiscard]] std::optional<std::string_view> signedCookie(
        std::string_view name,
        std::string_view secret) const;
    [[nodiscard]] Task<std::string_view> text() const;
    [[nodiscard]] Task<std::span<const std::byte>> bytes() const;
    [[nodiscard]] Task<std::span<const std::byte>> arrayBuffer() const;
    [[nodiscard]] Task<RequestBlob> blob() const;
    Task<void> discardBody() const;

    [[nodiscard]] Task<JsonValue> json() const;

    template <typename T>
    [[nodiscard]] Task<T> json() const;

    template <typename T>
    [[nodiscard]] Task<T> form() const;

    template <typename T>
    [[nodiscard]] const T& valid() const = delete;

    template <typename T>
    [[nodiscard]] const T& valid(ValidationTarget target) const;

    template <typename T>
    [[nodiscard]] const T& valid(std::string_view target) const;

    template <typename T>
    void addValidatedData(ValidationTarget target, T&& data) const;

    template <typename T>
    void addValidatedData(std::string_view target, T&& data) const;

    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> multipart() const;

    [[nodiscard]] Task<RequestFormData> parseBody() const {
        return parseBody(ParseBodyOptions{});
    }

    [[nodiscard]] Task<RequestFormData> parseBody(ParseBodyOptions options) const;

    [[nodiscard]] Task<RequestFormData> formData() const;

    [[nodiscard]] BodyReader& bodyReader() const;

    [[nodiscard]] MultipartReader multipartReader() const;

    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

    [[nodiscard]] const RequestNameValueList& param() const;

    [[nodiscard]] std::string_view routePath() const noexcept;

    [[nodiscard]] std::span<const MatchedRoute> matchedRoutes() const;

    [[nodiscard]] std::size_t routeIndex() const noexcept;

private:
    friend class Context;
    friend Task<RawRequestClone> cloneRawRequest(const ContextRequest& request);
    friend std::string_view routePath(const Context& context) noexcept;
    friend std::span<const MatchedRoute> matchedRoutes(const Context& context);

    explicit constexpr ContextRequest(const Context& context) noexcept
        : context_(&context) {}

    const Context* context_{nullptr};

    [[nodiscard]] Task<RawRequestClone> cloneRawRequest() const;
};

[[nodiscard]] inline Task<ContextRequest::RawRequestClone> cloneRawRequest(const ContextRequest& request) {
    return request.cloneRawRequest();
}

class Context final {
private:
    friend class ContextRequest;
    friend struct detail::ContextAccess;
    friend struct detail::SessionAccess;
    friend detail::RouteRateLimitResult detail::checkRouteRateLimit(
        Context& context,
        const detail::RouteRateLimitOptions& options) noexcept;
    template <typename T>
    friend void detail::setValidatedBody(Context& context, ValidationTarget target, T&& body);

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        detail::ContextServices services) noexcept;

    Context(
        RequestMemory& memory,
        const HttpRequest& request,
        std::string_view routePath,
        const std::string_view* paramNames,
        const std::string_view* paramValues,
        std::size_t paramCount,
        std::uintptr_t routeRateLimitScope,
        detail::ContextServices services,
        HttpMethod routeMethod = HttpMethod::kUnknown,
        std::size_t routeMiddlewareCount = 0) noexcept;

public:
    struct RenderOptions final {
        std::string_view head{};
        std::string_view title{};
    };

    using Renderer = Task<HttpResponse> (*)(
        Context& context,
        std::string_view body,
        RenderOptions options);

    using Layout = Task<HttpResponse> (*)(
        Context& context,
        std::string_view body,
        RenderOptions options);

    struct HeaderOptions final {
        bool append{false};
    };

    class ResponseHeaderInit final {
    public:
        constexpr ResponseHeaderInit() noexcept = default;

        constexpr ResponseHeaderInit(std::span<const HttpHeaderView> headers) noexcept
            : headers_(headers) {}

        template <std::size_t N>
        constexpr ResponseHeaderInit(const HttpHeaderView (&headers)[N]) noexcept
            : headers_(headers, N) {}

        constexpr ResponseHeaderInit(std::initializer_list<HttpHeaderView> headers) noexcept
            : headers_(headers.begin(), headers.size()) {}

        [[nodiscard]] constexpr operator std::span<const HttpHeaderView>() const noexcept {
            return headers_;
        }

    private:
        std::span<const HttpHeaderView> headers_{};
    };

    struct ResponseInit final {
        std::uint16_t status{0};
        std::string_view statusText{};
        ResponseHeaderInit headers{};
    };

    class Vars final {
    public:
        explicit constexpr Vars(Context& context) noexcept
            : context_(&context) {}

        template <typename T>
        [[nodiscard]] T* get(std::string_view name) const noexcept {
            return context_->template get<T>(name);
        }

        template <typename T>
        [[nodiscard]] bool has(std::string_view name) const noexcept {
            return get<T>(name) != nullptr;
        }

        template <typename T>
        [[nodiscard]] T* get(ContextKey<T> key) const noexcept {
            return context_->template get<T>(key);
        }

        template <typename T>
        [[nodiscard]] bool has(ContextKey<T> key) const noexcept {
            return get<T>(key) != nullptr;
        }

        template <typename T>
        [[nodiscard]] T& operator[](ContextKey<T> key) const {
            if (auto* value = get(key)) {
                return *value;
            }
            throw std::logic_error("context value is not available");
        }

    private:
        Context* context_;
    };

    class ConstVars final {
    public:
        explicit constexpr ConstVars(const Context& context) noexcept
            : context_(&context) {}

        template <typename T>
        [[nodiscard]] const T* get(std::string_view name) const noexcept {
            return context_->template get<T>(name);
        }

        template <typename T>
        [[nodiscard]] bool has(std::string_view name) const noexcept {
            return get<T>(name) != nullptr;
        }

        template <typename T>
        [[nodiscard]] const T* get(ContextKey<T> key) const noexcept {
            return context_->template get<T>(key);
        }

        template <typename T>
        [[nodiscard]] bool has(ContextKey<T> key) const noexcept {
            return get<T>(key) != nullptr;
        }

        template <typename T>
        [[nodiscard]] const T& operator[](ContextKey<T> key) const {
            if (const auto* value = get(key)) {
                return *value;
            }
            throw std::logic_error("context value is not available");
        }

    private:
        const Context* context_;
    };

    ~Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] ContextRequest req() const noexcept {
        return ContextRequest(*this);
    }

    [[nodiscard]] std::exception_ptr error() const noexcept {
        return error_;
    }

    // Server-side session blob (persisted by a SessionMiddleware via Redis; the
    // application owns the blob's format). setSession/clearSession mark it for
    // persistence on the way out.
    [[nodiscard]] std::string_view session() const noexcept {
        return sessionData_ == nullptr
            ? std::string_view{}
            : std::string_view(sessionData_->data(), sessionData_->size());
    }
    void setSession(std::string_view data) {
        detail::assignStableString(sessionDataStorage(), data);
        sessionDirty_ = true;
    }
    void clearSession() {
        if (sessionData_ != nullptr) {
            sessionData_->clear();
        }
        sessionDirty_ = true;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    [[nodiscard]] const Env& env() const noexcept;

#ifdef RUVIA_ENABLE_MARIADB
    [[nodiscard]] DbHandle db() const;
    [[nodiscard]] DbHandle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] RedisHandle redis() const;
    [[nodiscard]] RedisHandle redis(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_HTTP_CLIENT
    // path is an HTTP/1.1 origin-form target: empty maps to "/", otherwise use "/..." or "*".
    [[nodiscard]] Task<FetchResponse> fetch(
        std::string_view path,
        FetchOptions options = {}) {
        return fetch(detail::kDefaultHttpClientAlias, path, std::move(options));
    }

    // path is an HTTP/1.1 origin-form target: empty maps to "/", otherwise use "/..." or "*".
    [[nodiscard]] Task<FetchResponse> fetch(
        std::string_view alias,
        std::string_view path,
        FetchOptions options = {});
#endif

    [[nodiscard]] WebSocket& webSocket() const;

    [[nodiscard]] ResponseStreamWriter& stream() const;

    [[nodiscard]] ResponseStreamWriter& streamText();

    [[nodiscard]] SseWriter streamSSE() const;

    template <typename T = std::byte>
    [[nodiscard]] std::pmr::polymorphic_allocator<T> allocator() const noexcept {
        return std::pmr::polymorphic_allocator<T>(resource());
    }

    template <typename T>
    void set(std::string_view name, T&& value) {
        values().set(name, std::forward<T>(value));
    }

    template <typename T, typename ValueT>
    void set(ContextKey<T> key, ValueT&& value) {
        values().template setAs<T>(key.name(), std::forward<ValueT>(value));
    }

    template <typename T>
    [[nodiscard]] T* get(std::string_view name) noexcept {
        auto* store = valuesIf();
        return store == nullptr ? nullptr : store->template getIf<T>(name);
    }

    template <typename T>
    [[nodiscard]] const T* get(std::string_view name) const noexcept {
        const auto* store = valuesIf();
        return store == nullptr ? nullptr : store->template getIf<T>(name);
    }

    template <typename T>
    [[nodiscard]] T* get(ContextKey<T> key) noexcept {
        return get<T>(key.name());
    }

    template <typename T>
    [[nodiscard]] const T* get(ContextKey<T> key) const noexcept {
        return get<T>(key.name());
    }

    [[nodiscard]] Vars var() noexcept {
        return Vars(*this);
    }

    [[nodiscard]] ConstVars var() const noexcept {
        return ConstVars(*this);
    }

    void status(std::uint16_t statusCode);

    void header(std::string_view name, std::string_view value) {
        header(name, value, HeaderOptions{});
    }

    void header(std::string_view name, std::string_view value, HeaderOptions options);

    void header(std::string_view name, std::nullopt_t);

    void setCookie(std::string_view name, std::string_view value, const CookieOptions& options = {});
    void setSignedCookie(
        std::string_view name,
        std::string_view value,
        std::string_view secret,
        const CookieOptions& options = {});
    // Serialize a Set-Cookie header value without touching the response.
    [[nodiscard]] std::pmr::string generateCookie(
        std::string_view name,
        std::string_view value,
        const CookieOptions& options = {}) const;
    [[nodiscard]] std::pmr::string generateSignedCookie(
        std::string_view name,
        std::string_view value,
        std::string_view secret,
        const CookieOptions& options = {}) const;
    [[nodiscard]] std::optional<std::string_view> deleteCookie(std::string_view name, CookieOptions options = {});

    [[nodiscard]] HttpResponse& res();

    void res(HttpResponse&& response);

    [[nodiscard]] bool finalized() const noexcept {
        return responseFinalized_;
    }

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::string_view body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::nullptr_t,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(std::nullptr_t, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(
        std::span<const std::byte> body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse body(std::span<const std::byte> body, ResponseInit init) const;

    [[nodiscard]] HttpResponse newResponse(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse newResponse(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(
        std::string_view body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse newResponse(
        std::nullptr_t,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse newResponse(
        std::nullptr_t,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(
        std::nullptr_t,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(std::nullptr_t, ResponseInit init) const;

    [[nodiscard]] HttpResponse newResponse(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse newResponse(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse newResponse(
        std::span<const std::byte> body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse newResponse(
        std::span<const std::byte> body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(
        std::span<const std::byte> body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse newResponse(std::span<const std::byte> body, ResponseInit init) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse newResponse(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse newResponse(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse newResponse(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse newResponse(const char (&body)[N], ResponseInit init) const;

    [[nodiscard]] HttpResponse newResponse(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse newResponse(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse newResponse(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse newResponse(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse newResponse(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse body(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse body(const char (&body)[N], ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::string_view body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse text(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse text(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse text(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse text(const char (&body)[N], ResponseInit init) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(
        const T& value,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    template <typename T>
    [[nodiscard]] HttpResponse json(const T& value, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::string_view body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(std::string_view body, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse html(std::pmr::string& body, ResponseInit init) const;

    [[nodiscard]] HttpResponse html(
        const std::pmr::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::pmr::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        const std::string& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    [[nodiscard]] HttpResponse html(
        std::string&& body,
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const = delete;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::uint16_t statusCode = 0,
        std::string_view statusText = {}) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::span<const HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(
        const char (&body)[N],
        std::uint16_t statusCode,
        std::initializer_list<HttpHeaderView> headers) const;

    template <std::size_t N>
    [[nodiscard]] HttpResponse html(const char (&body)[N], ResponseInit init) const;

    void setRenderer(Renderer renderer) noexcept;

    [[nodiscard]] Layout setLayout(Layout layout) noexcept;

    [[nodiscard]] Layout getLayout() const noexcept;

    [[nodiscard]] Task<HttpResponse> render(std::string_view body);

    [[nodiscard]] Task<HttpResponse> render(std::string_view body, std::string_view head);

    [[nodiscard]] Task<HttpResponse> render(std::string_view body, RenderOptions options);

    [[nodiscard]] HttpResponse redirect(
        std::string_view location,
        std::uint16_t statusCode = 302,
        std::string_view statusText = {}) const;

    [[nodiscard]] HttpResponse file(
        const std::filesystem::path& path,
        std::string_view contentType = {}) const;

    [[nodiscard]] HttpResponse staticFile(
        const StaticRoot& root,
        std::string_view relativePath,
        std::string_view contentType = {}) const;

    [[nodiscard]] HttpResponse error(
        std::uint16_t statusCode,
        std::string_view code,
        std::string_view message,
        std::string_view statusText = {}) const;

    [[nodiscard]] Task<HttpResponse> notFound();

private:
    [[nodiscard]] HttpResponse streamingHead(std::string_view contentType = {}) const;

    [[nodiscard]] Task<std::string_view> requestBody() const;
    Task<void> requestDiscardBody() const;
    [[nodiscard]] Task<std::pmr::vector<MultipartPart>> requestMultipart() const;
    [[nodiscard]] Task<ContextRequest::RequestFormData> parseRequestBody(
        ContextRequest::ParseBodyOptions options,
        ContextRequest::RequestFormData::SingleValueSelection singleValueSelection =
            ContextRequest::RequestFormData::SingleValueSelection::kLast) const;
    [[nodiscard]] BodyReader& requestBodyReader() const;
    [[nodiscard]] MultipartReader requestMultipartReader() const;
    [[nodiscard]] std::optional<std::string_view> routeParam(std::string_view name) const;
    [[nodiscard]] bool requestAccepts(std::string_view mediaType) const noexcept;
    void ensureRequestQuery() const;
    [[nodiscard]] const RequestNameValueList& requestQuery() const;
    [[nodiscard]] const RequestValueGroupList& requestQueries() const;
    [[nodiscard]] const RequestNameValueList& requestCookies() const;
    [[nodiscard]] const std::pmr::vector<ContextRequest::MatchedRoute>& requestMatchedRoutes() const;

    [[nodiscard]] std::string_view multipartBoundary() const;

    [[nodiscard]] bool requestContentTypeMatches(std::string_view expected) const noexcept;

    Context& setStableResponseHeader(std::string_view name, std::string_view value);
    Context& removeResponseHeader(std::string_view name);
    void rebuildResponseHeaderIndexes() noexcept;

    [[nodiscard]] HttpResponseHeader* findResponseHeaderForUpdate(
        std::string_view name,
        std::uint32_t knownBit) noexcept;

    void recordResponseKnownHeaderIndex(std::uint32_t knownBit, std::size_t index) noexcept;

    void applyResponseState(
        HttpResponse& response,
        std::uint16_t statusCode,
        std::string_view statusText,
        std::span<const HttpHeaderView> headers = {}) const;

    void applyExplicitResponseHeaders(
        HttpResponse& response,
        std::span<const HttpHeaderView> headers) const;

    [[nodiscard]] HttpResponse textStaticView(
        std::string_view body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    [[nodiscard]] HttpResponse jsonSerialized(
        std::pmr::string& body,
        std::uint16_t statusCode,
        std::string_view statusText) const;

    [[nodiscard]] const RequestNameValueList& requestHeaders() const;
    [[nodiscard]] std::optional<std::string_view> requestHeader(std::string_view name) const;
    [[nodiscard]] const RequestNameValueList& routeParams() const;
    [[nodiscard]] std::pmr::string& decodedBody() const;
    [[nodiscard]] std::string_view sessionId() const noexcept {
        return sessionId_ == nullptr
            ? std::string_view{}
            : std::string_view(sessionId_->data(), sessionId_->size());
    }
    [[nodiscard]] std::pmr::string& sessionIdStorage();
    [[nodiscard]] std::pmr::string& sessionDataStorage();
    [[nodiscard]] detail::ContextValueStore& values();
    [[nodiscard]] HttpResponse& responseStorage();
    void storeResponse(HttpResponse&& response);
    void storeAssignedResponse(HttpResponse&& response);
    void storeError(std::exception_ptr exception) noexcept {
        error_ = std::move(exception);
    }
    [[nodiscard]] bool hasResponse() const noexcept {
        return responseFinalized_;
    }
    [[nodiscard]] HttpResponse takeResponse();
    [[nodiscard]] detail::ContextValueStore* valuesIf() noexcept {
        return values_;
    }
    [[nodiscard]] const detail::ContextValueStore* valuesIf() const noexcept {
        return values_;
    }

    static constexpr std::size_t kResponseIndexSlots = 22;

    RequestMemory& memory_;
    const HttpRequest& request_;
    std::string_view routePath_;
    HttpMethod routeMethod_{HttpMethod::kUnknown};
    const std::string_view* paramNames_{nullptr};
    const std::string_view* paramValues_{nullptr};
    std::size_t paramCount_{0};
    std::size_t routeMiddlewareCount_{0};
    [[maybe_unused]] detail::DbRegistry* db_{nullptr};
    [[maybe_unused]] detail::RedisRegistry* redis_{nullptr};
    [[maybe_unused]] detail::HttpClientRegistry* httpClients_{nullptr};
    detail::RateLimiter* rateLimiter_{nullptr};
    HttpErrorHandler errorHandler_{nullptr};
    HttpNotFoundHandler notFoundHandler_{nullptr};
    std::uintptr_t routeRateLimitScope_{0};
    BodyReader* bodyReader_{nullptr};
    detail::RequestBodyLoader* bodyLoader_{nullptr};
    WebSocket* webSocket_{nullptr};
    ResponseStreamWriter* responseStream_{nullptr};
    Renderer renderer_{nullptr};
    Layout layout_{nullptr};
    std::uint16_t responseStatusCode_{200};
    HttpResponseHeaders responseHeaders_;
    // Holds the decoded request body when Content-Encoding was applied, so
    // body() can return a stable view; mutable because body() is const.
    mutable std::pmr::string* decodedBody_{nullptr};
    mutable RequestNameValueList* requestHeaders_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* requestQueryStorage_{nullptr};
    mutable RequestNameValueList* requestQuery_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* requestQueriesStorage_{nullptr};
    mutable RequestValueGroupList* requestQueries_{nullptr};
    mutable RequestNameValueList* requestCookies_{nullptr};
    mutable std::pmr::vector<std::pmr::string>* routeParamStorage_{nullptr};
    mutable RequestNameValueList* routeParams_{nullptr};
    mutable std::pmr::vector<ContextRequest::MatchedRoute>* matchedRoutes_{nullptr};
    std::pmr::string* sessionId_{nullptr};
    std::pmr::string* sessionData_{nullptr};
    detail::ContextValueStore* values_{nullptr};
    HttpResponse* response_{nullptr};
    std::exception_ptr error_;
    mutable bool bodyDecoded_ : 1 {false};
    bool sessionDirty_ : 1 {false};
    bool responseFinalized_ : 1 {false};
    std::array<std::int16_t, kResponseIndexSlots> responseHeaderIndexes_{};

    detail::ValidatedValueStore validatedValues_;
};

inline const HttpRequest& ContextRequest::raw() const noexcept {
    return context_->request_;
}

inline std::string_view ContextRequest::method() const noexcept {
    return methodName(raw().method());
}

inline std::pmr::string ContextRequest::url() const {
    const auto requestTarget = raw().target();
    std::pmr::string result(context_->resource());
    if (requestTarget.starts_with("http://") || requestTarget.starts_with("https://")) {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    const auto host = header("Host");
    if (!host || host->empty() || requestTarget.empty() || requestTarget.front() != '/') {
        result.assign(requestTarget.data(), requestTarget.size());
        return result;
    }

    result.append(raw().isSecure() ? "https://" : "http://");
    result.append(host->data(), host->size());
    result.append(requestTarget.data(), requestTarget.size());
    return result;
}

inline std::string_view ContextRequest::path() const noexcept {
    return raw().path();
}

inline std::string_view ContextRequest::routePath() const noexcept {
    return context_->routePath_;
}

inline std::span<const ContextRequest::MatchedRoute> ContextRequest::matchedRoutes() const {
    return context_->requestMatchedRoutes();
}

inline std::size_t ContextRequest::routeIndex() const noexcept {
    if (context_->routePath_.empty() || context_->routeMethod_ == HttpMethod::kUnknown) {
        return 0;
    }
    return context_->routeMiddlewareCount_;
}

inline std::string_view routePath(const Context& context) noexcept {
    return context.req().routePath();
}

inline std::span<const ContextRequest::MatchedRoute> matchedRoutes(const Context& context) {
    return context.req().matchedRoutes();
}

inline std::string_view routePath(const Context& context, std::ptrdiff_t index) {
    const auto routes = matchedRoutes(context);
    if (routes.empty()) {
        return {};
    }

    auto resolved = index;
    if (resolved < 0) {
        resolved += static_cast<std::ptrdiff_t>(routes.size());
    }
    if (resolved < 0 || static_cast<std::size_t>(resolved) >= routes.size()) {
        return {};
    }
    return routes[static_cast<std::size_t>(resolved)].path;
}

inline const RequestNameValueList& ContextRequest::header() const {
    return context_->requestHeaders();
}

inline std::optional<std::string_view> ContextRequest::header(std::string_view name) const {
    return context_->requestHeader(name);
}

inline bool ContextRequest::accepts(std::string_view mediaType) const noexcept {
    return context_->requestAccepts(mediaType);
}

inline std::optional<std::string_view> ContextRequest::query(std::string_view name) const {
    return context_->requestQuery().get(name);
}

inline const RequestNameValueList& ContextRequest::query() const {
    return context_->requestQuery();
}

inline std::optional<std::span<const std::string_view>> ContextRequest::queries(std::string_view name) const {
    return context_->requestQueries().get(name);
}

inline const RequestValueGroupList& ContextRequest::queries() const {
    return context_->requestQueries();
}

inline std::optional<std::string_view> ContextRequest::cookie(std::string_view name) const {
    return context_->requestCookies().get(name);
}

inline const RequestNameValueList& ContextRequest::cookie() const {
    return context_->requestCookies();
}


inline Task<std::string_view> ContextRequest::text() const {
    return context_->requestBody();
}

inline Task<std::span<const std::byte>> ContextRequest::bytes() const {
    return arrayBuffer();
}

inline Task<std::span<const std::byte>> ContextRequest::arrayBuffer() const {
    const auto body = co_await text();
    co_return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()),
        body.size());
}

inline Task<ContextRequest::RequestBlob> ContextRequest::blob() const {
    auto bytes = co_await arrayBuffer();
    co_return RequestBlob(bytes, header("Content-Type").value_or(std::string_view{}));
}

inline Task<void> ContextRequest::discardBody() const {
    return context_->requestDiscardBody();
}

inline Task<std::pmr::vector<MultipartPart>> ContextRequest::multipart() const {
    return context_->requestMultipart();
}

inline Task<ContextRequest::RequestFormData> ContextRequest::parseBody(ParseBodyOptions options) const {
    return context_->parseRequestBody(options);
}

inline Task<ContextRequest::RequestFormData> ContextRequest::formData() const {
    return context_->parseRequestBody(
        ParseBodyOptions{.all = true},
        RequestFormData::SingleValueSelection::kFirst);
}

inline BodyReader& ContextRequest::bodyReader() const {
    return context_->requestBodyReader();
}

inline MultipartReader ContextRequest::multipartReader() const {
    return context_->requestMultipartReader();
}

inline std::optional<std::string_view> ContextRequest::param(std::string_view name) const {
    return context_->routeParam(name);
}

inline const RequestNameValueList& ContextRequest::param() const {
    return context_->routeParams();
}

template <typename T>
inline const T& ContextRequest::valid(ValidationTarget target) const {
    return context_->validatedValues_.get<T>(target);
}

template <typename T>
inline const T& ContextRequest::valid(std::string_view target) const {
    return valid<T>(validationTargetFromName(target));
}

namespace detail {

template <typename T>
void setValidatedBody(Context& context, ValidationTarget target, T&& body) {
    context.validatedValues_.set(target, std::forward<T>(body), context.resource());
}

}  // namespace detail

template <typename T>
inline void ContextRequest::addValidatedData(ValidationTarget target, T&& data) const {
    detail::setValidatedBody(
        const_cast<Context&>(*context_),
        target,
        std::forward<T>(data));
}

template <typename T>
inline void ContextRequest::addValidatedData(std::string_view target, T&& data) const {
    addValidatedData(validationTargetFromName(target), std::forward<T>(data));
}

}  // namespace ruvia

#include "ruvia/http/Context.inl"
