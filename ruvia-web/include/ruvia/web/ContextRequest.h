#pragma once

// Request-side public API is owned independently from Context response/state
// construction. Context.h remains the convenient umbrella, while request-only
// consumers can depend on this narrower contract. Non-template facade methods
// are provided by ruvia::web; only model-dependent templates remain inline.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/MultipartReader.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/RequestFields.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"

namespace ruvia {

class Context;
class ContextRequest;
template <typename T>
class ValidatedJson;

namespace detail {
struct RequestFormFieldAccess;
struct RequestFormDataAccess;
template <typename T>
[[nodiscard]] RequestBindingHandle<T> bindValidatedModel(Context& context, const T& model);

template <typename T>
[[nodiscard]] RequestBindingHandle<T> bindValidatedJsonModel(Context& context, const T& model, std::string_view rawJson);

template <typename T>
    requires(!std::is_lvalue_reference_v<T>)
RequestBindingHandle<std::remove_cvref_t<T>> bindValidatedModel(Context&, T&&) = delete;

[[noreturn]] void throwInvalidJsonContentType();
[[noreturn]] void throwInvalidJsonBody();
[[noreturn]] void throwInvalidFormContentType();
[[noreturn]] void throwInvalidFormBody();
[[noreturn]] void throwTooManyFormFields();
[[noreturn]] void throwInvalidQuery();
[[noreturn]] void throwInvalidParam();
[[noreturn]] void throwInvalidHeader();
[[noreturn]] void throwInvalidCookie();
}  // namespace detail

struct SignedCookieLookupOptions final {
    BorrowedText name;
    BorrowedText secret;
};

class ContextRequest final {
public:
    enum class RepeatedScalarPolicy {
        kLastValue,
        kRetainAll,
    };

    enum class DottedNamePolicy {
        kLiteral,
        kExpandPath,
    };

    struct ParseBodyOptions final {
        RepeatedScalarPolicy repeatedScalars{RepeatedScalarPolicy::kLastValue};
        DottedNamePolicy dottedNames{DottedNamePolicy::kLiteral};
        // Upper bound on parsed form fields / multipart parts. A body of only
        // delimiters (e.g. 16 MiB of "x[]=&") would otherwise build millions of
        // heavy field objects from one request; rejecting past this many caps the
        // amplification. Matches the PHP max_input_vars / Django
        // DATA_UPLOAD_MAX_NUMBER_FIELDS default; raise it for forms that
        // legitimately carry more fields.
        std::size_t maxFields{1000};
    };

    struct RequestFormField;
    class RequestFormData;

    class RequestBlob final {
    public:
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return bytes_;
        }

        [[nodiscard]] std::string_view text() const noexcept {
            return std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
        }

        [[nodiscard]] std::string_view contentType() const noexcept {
            return contentType_;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return bytes_.empty();
        }

    private:
        friend class ContextRequest;
        friend struct RequestFormField;

        constexpr RequestBlob(std::span<const std::byte> bytes, std::string_view contentType) noexcept
            : bytes_(bytes),
              contentType_(contentType) {}

        std::span<const std::byte> bytes_;
        std::string_view contentType_;
    };

    struct RequestFormField final {
        RequestFormField(const RequestFormField&) = delete;
        RequestFormField& operator=(const RequestFormField&) = delete;
        RequestFormField(RequestFormField&&) noexcept = default;
        RequestFormField& operator=(RequestFormField&&) = delete;

        [[nodiscard]] std::string_view name() const& noexcept {
            return name_;
        }
        [[nodiscard]] std::string_view name() const&& = delete;

        [[nodiscard]] std::string_view value() const& noexcept {
            return value_;
        }
        [[nodiscard]] std::string_view value() const&& = delete;

        [[nodiscard]] std::string_view filename() const& noexcept {
            return filename_;
        }
        [[nodiscard]] std::string_view filename() const&& = delete;

        [[nodiscard]] std::string_view contentType() const& noexcept {
            return contentType_;
        }
        [[nodiscard]] std::string_view contentType() const&& = delete;

        [[nodiscard]] std::span<const std::pmr::string> path() const& noexcept {
            return path_;
        }
        [[nodiscard]] std::span<const std::pmr::string> path() const&& = delete;

        [[nodiscard]] bool isFile() const noexcept {
            return file_;
        }

        [[nodiscard]] bool hasArrayName() const noexcept {
            return array_;
        }

        [[nodiscard]] RequestBlob blob() const& noexcept {
            return RequestBlob(std::as_bytes(std::span(value_)), std::string_view(contentType_));
        }
        [[nodiscard]] RequestBlob blob() const&& = delete;

    private:
        friend struct detail::RequestFormFieldAccess;

        RequestFormField(std::pmr::memory_resource* resource, std::pmr::string&& fieldName, std::pmr::string&& fieldValue, std::pmr::string&& fieldFilename = {}, std::pmr::string&& fieldContentType = {}, bool fieldFile = false, bool fieldArray = false)
            : name_(std::move(fieldName)),
              value_(std::move(fieldValue)),
              filename_(std::move(fieldFilename)),
              contentType_(std::move(fieldContentType)),
              path_(resource),
              file_(fieldFile),
              array_(fieldArray) {}

        std::pmr::string name_;
        std::pmr::string value_;
        std::pmr::string filename_;
        std::pmr::string contentType_;
        std::pmr::vector<std::pmr::string> path_;
        bool file_{false};
        bool array_{false};
    };

    class RequestFormData final {
    public:
        class Object;

        // Same-name form field group; field() selects the last retained field,
        // while fields() exposes every retained member.
        class Group final {
        public:
            Group(const Group&) = delete;
            Group& operator=(const Group&) = delete;
            Group(Group&&) noexcept = default;
            Group& operator=(Group&&) = delete;

            [[nodiscard]] std::string_view name() const noexcept {
                return name_;
            }

            [[nodiscard]] const RequestFormField* field() const noexcept {
                if (fields_.empty()) {
                    return nullptr;
                }
                return fields_.back();
            }

            [[nodiscard]] std::span<const RequestFormField* const> fields() const& noexcept {
                return fields_;
            }
            [[nodiscard]] std::span<const RequestFormField* const> fields() const&& = delete;

            [[nodiscard]] std::size_t size() const noexcept {
                return fields_.size();
            }

            [[nodiscard]] bool empty() const noexcept {
                return size() == 0;
            }

            [[nodiscard]] bool hasArrayName() const noexcept {
                return array_;
            }

            [[nodiscard]] bool multiple() const noexcept {
                return fields_.size() > 1;
            }

            [[nodiscard]] std::optional<std::string_view> value() const noexcept {
                const auto* const selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return selected->value();
            }

        private:
            friend class RequestFormData;
            friend class Object;

            [[nodiscard]] static Group make(std::pmr::memory_resource* resource, std::string_view name, bool array) {
                return Group(resource, name, array);
            }

            Group(std::pmr::memory_resource* resource, std::string_view name, bool array)
                : name_(name),
                  fields_(resource),
                  array_(array) {}

            void add(const RequestFormField& field) {
                fields_.push_back(&field);
                array_ = array_ || field.hasArrayName();
            }

            std::string_view name_;
            std::pmr::vector<const RequestFormField*> fields_;
            bool array_{false};
        };

        class Value final {
        public:
            [[nodiscard]] explicit operator bool() const noexcept {
                return field() != nullptr;
            }

            [[nodiscard]] const RequestFormField* field() const noexcept {
                return group_ == nullptr ? nullptr : group_->field();
            }

            [[nodiscard]] std::span<const RequestFormField* const> fields() const noexcept {
                return group_ == nullptr ? std::span<const RequestFormField* const>{} : group_->fields();
            }

            [[nodiscard]] std::size_t size() const noexcept {
                return group_ == nullptr ? 0 : group_->size();
            }

            [[nodiscard]] bool empty() const noexcept {
                return size() == 0;
            }

            [[nodiscard]] bool multiple() const noexcept {
                return group_ != nullptr && group_->multiple();
            }

            [[nodiscard]] bool hasArrayName() const noexcept {
                return group_ != nullptr && group_->hasArrayName();
            }

            [[nodiscard]] std::optional<std::string_view> value() const noexcept {
                return group_ == nullptr ? std::nullopt : group_->value();
            }

            [[nodiscard]] std::optional<RequestBlob> blob() const noexcept {
                const auto* selected = field();
                if (selected == nullptr) {
                    return std::nullopt;
                }
                return selected->blob();
            }

        private:
            friend class RequestFormData;
            friend class Object;

            explicit Value(const Group* group) noexcept
                : group_(group) {}

            const Group* group_{nullptr};
        };

        class Object final {
        public:
            Object(const Object&) = delete;
            Object& operator=(const Object&) = delete;
            Object(Object&&) noexcept = default;
            Object& operator=(Object&&) = delete;

            [[nodiscard]] Value get(std::string_view name) const noexcept {
                return Value(form_->pathEntryChild(path(), name));
            }

            [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
                if (hasNestedName(name)) {
                    return form_->countAtChild(path(), name);
                }
                const auto* formEntry = findEntry(name);
                return formEntry == nullptr ? 0 : formEntry->size();
            }

            [[nodiscard]] std::span<const Group> groups() const& noexcept {
                return entries_;
            }
            [[nodiscard]] std::span<const Group> groups() const&& = delete;

            [[nodiscard]] Object object(std::string_view name) const;

        private:
            friend class RequestFormData;

            Object(const RequestFormData& form, std::string_view dotPath);

            [[nodiscard]] static std::pmr::memory_resource* resourceFor(const RequestFormData& form) noexcept {
                return form.fields_.get_allocator().resource();
            }

            [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
                return entries_.get_allocator().resource();
            }

            [[nodiscard]] std::string_view path() const noexcept {
                return dotPath_;
            }

            [[nodiscard]] static bool hasNestedName(std::string_view name) noexcept {
                return name.find('.') != std::string_view::npos;
            }

            [[nodiscard]] const Group* findEntry(std::string_view name) const noexcept {
                for (const auto& formEntry : entries_) {
                    if (formEntry.name() == name) {
                        return &formEntry;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] static std::string_view directChildName(const RequestFormField& field, std::string_view dotPath) noexcept;

            void rebuildEntries();

            const RequestFormData* form_{nullptr};
            std::pmr::string dotPath_;
            std::pmr::vector<Group> entries_;
        };

        RequestFormData(const RequestFormData&) = delete;
        RequestFormData& operator=(const RequestFormData&) = delete;
        RequestFormData(RequestFormData&&) noexcept = default;
        RequestFormData& operator=(RequestFormData&&) = delete;

        [[nodiscard]] std::span<const RequestFormField> fields() const& noexcept {
            return fields_;
        }
        [[nodiscard]] std::span<const RequestFormField> fields() const&& = delete;

        [[nodiscard]] std::span<const Group> groups() const& noexcept {
            return entries_;
        }
        [[nodiscard]] std::span<const Group> groups() const&& = delete;

        [[nodiscard]] Value get(std::string_view name) const& noexcept {
            return Value(findEntry(name));
        }
        [[nodiscard]] Value get(std::string_view) const&& = delete;

        [[nodiscard]] Object object(std::string_view dotPath) const& {
            return Object(*this, dotPath);
        }
        [[nodiscard]] Object object(std::string_view) const&& = delete;

        [[nodiscard]] std::size_t count(std::string_view name) const noexcept {
            const auto* formEntry = findEntry(name);
            return formEntry == nullptr ? 0 : formEntry->size();
        }

    private:
        friend struct detail::RequestFormDataAccess;

        explicit RequestFormData(std::pmr::memory_resource* resource)
            : fields_(resource),
              entries_(resource),
              pathEntries_(resource) {}

        explicit RequestFormData(std::pmr::vector<RequestFormField>&& fields);

        [[nodiscard]] const Group* findEntry(std::string_view name) const noexcept {
            if (isPathName(name)) {
                if (const auto* formEntry = pathEntry(name)) {
                    return formEntry;
                }
            }
            for (const auto& formEntry : entries_) {
                if (formEntry.name() == name) {
                    return &formEntry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static bool isPathName(std::string_view name) noexcept {
            return name.find('.') != std::string_view::npos;
        }

        [[nodiscard]] static bool consumePath(const RequestFormField& field, std::size_t& index, std::string_view dotPath) noexcept;

        [[nodiscard]] static bool pathMatches(const RequestFormField& field, std::string_view dotPath) noexcept {
            const auto path = field.path();
            if (path.empty() || dotPath.empty()) {
                return false;
            }

            std::size_t index = 0;
            return consumePath(field, index, dotPath) && index == path.size();
        }

        [[nodiscard]] static bool pathMatchesChild(const RequestFormField& field, std::string_view dotPath, std::string_view name) noexcept {
            const auto path = field.path();
            if (path.empty() || name.empty()) {
                return false;
            }

            std::size_t index = 0;
            return consumePath(field, index, dotPath) && consumePath(field, index, name) && index == path.size();
        }

        [[nodiscard]] const Group* pathEntry(std::string_view dotPath) const noexcept {
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

        [[nodiscard]] const Group* pathEntryChild(std::string_view dotPath, std::string_view name) const noexcept {
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

        [[nodiscard]] static bool pathNameMatchesChild(std::string_view pathName, std::string_view dotPath, std::string_view name) noexcept {
            if (name.empty()) {
                return false;
            }
            if (dotPath.empty()) {
                return pathName == name;
            }
            return pathName.size() == dotPath.size() + 1 + name.size() && pathName.starts_with(dotPath) && pathName[dotPath.size()] == '.' && pathName.ends_with(name);
        }

        [[nodiscard]] std::size_t countAtChild(std::string_view dotPath, std::string_view name) const noexcept {
            const auto* formEntry = pathEntryChild(dotPath, name);
            return formEntry == nullptr ? 0 : formEntry->size();
        }

        void rebuildEntries();

        [[nodiscard]] static std::string_view entryName(const RequestFormField& field) noexcept {
            const auto path = field.path();
            if (!path.empty()) {
                const auto& name = path.front();
                return name;
            }
            return field.name();
        }

        void rebuildPathEntries(std::pmr::memory_resource* resource);

        [[nodiscard]] static std::string_view pathEntryName(const RequestFormField& field) noexcept {
            return field.name();
        }

        std::pmr::vector<RequestFormField> fields_;
        std::pmr::vector<Group> entries_;
        std::pmr::vector<Group> pathEntries_;
    };

    [[nodiscard]] std::string_view method() const noexcept;
    [[nodiscard]] HttpKnownMethod knownMethod() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;
    [[nodiscard]] std::string_view routePath() const noexcept;
    // Malformed request fields and selected-body syntax errors throw HttpError
    // with a 400 status. A selected body with the wrong media type throws
    // HttpError with 415; application/programming exceptions are never
    // represented by these request-validation signals.
    // Which Accept-* field a negotiation reads. The weight grammar is shared,
    // but how a member matches an offered value is not: Accept carries media
    // ranges with wildcards and parameters, Accept-Language does RFC 4647 basic
    // filtering ("en" matches "en-US"), and the other two match tokens exactly
    // or by "*".
    enum class Negotiable : std::uint8_t {
        kMediaType,
        kLanguage,
        kEncoding,
        kCharset,
    };

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

    // Whether the client would accept this one representation. The cheap probe,
    // and the right call when there is only one thing to offer.
    [[nodiscard]] bool accepts(std::string_view mediaType) const noexcept;

    // Picks the representation to send when several could be. Returns the
    // element of `supported` the client prefers most, or nullopt when it
    // accepts none of them -- which is a 406, and the reason this cannot just
    // return the first entry as a fallback.
    //
    // The client's weights decide; `supported` order breaks its ties, so it
    // reads as the server's own preference, best first. An absent field means
    // no preference and selects supported.front(). The returned view is one of
    // the caller's own entries, borrowed, not a copy out of the request.
    //
    // accepts() answers "would this do?" for one type; this answers "which of
    // mine is best?" -- looping accepts() in server order cannot, because it
    // returns the server's first acceptable option rather than the client's
    // most preferred one.
    [[nodiscard]] std::optional<std::string_view> negotiate(Negotiable field, std::span<const std::string_view> supported) const noexcept;

    [[nodiscard]] std::optional<std::string_view> negotiate(Negotiable field, std::initializer_list<std::string_view> supported) const noexcept {
        return negotiate(field, std::span<const std::string_view>(supported.begin(), supported.size()));
    }
    [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const;
    [[nodiscard]] std::span<const std::string_view> queries(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const;

    // Every field of one kind, in the order the request presented it, for the
    // callers that must enumerate rather than look one name up: signing an
    // arbitrary header set, forwarding, or diagnostics. The named lookups above
    // stay the cheap path; these materialize the request's field list once and
    // cache it for the rest of the request. The list borrows request-owned
    // storage, so it never outlives this Context.
    //
    // "Fields" rather than a no-argument queries(): queries(name) already means
    // "every value for this one name", and one identifier must not mean both.
    //
    // Deliberately not const&&-qualified, unlike the accessors on the owning
    // types this list is made of. ContextRequest owns nothing -- it is a facade
    // over a Context pointer -- and req() returns one by value, so the natural
    // c.req().headerFields() is an rvalue call that must keep working. The
    // borrowed storage belongs to the Context and outlives the facade, exactly
    // as it does for header()/queries().
    //
    // headerFields() normalizes every field name to lower case, matching the
    // HTTP/2 wire form, while header(name) is an ASCII case-insensitive lookup
    // over the request as received. Compare names from this list against lower
    // case literals; "X-Trace" never matches, "x-trace" does.
    [[nodiscard]] const RequestNameValueList& headerFields() const;
    [[nodiscard]] const RequestNameValueList& queryFields() const;
    [[nodiscard]] const RequestNameValueList& cookieFields() const;
    [[nodiscard]] const RequestNameValueList& paramFields() const;
    // Verifies the "value.signature" format written by setSignedCookie; returns
    // the value view on a valid signature, nullopt when missing or tampered.
    [[nodiscard]] std::optional<std::string_view> signedCookie(SignedCookieLookupOptions options) const;
    [[nodiscard]] ScopedOperation<std::string_view> text() const;
    [[nodiscard]] ScopedOperation<std::span<const std::byte>> bytes() const;
    [[nodiscard]] ScopedOperation<RequestBlob> blob() const;
    ScopedOperation<void> discardBody() const;

    [[nodiscard]] ScopedOperation<JsonValue> jsonValue() const;

    template <typename T>
    [[nodiscard]] ScopedOperation<T> json() const;

    template <typename T>
    [[nodiscard]] ScopedOperation<T> form() const;

    // Optional format probes for endpoints that want to fall back when the
    // Content-Type is not the consumed media type. Once the media type selects
    // JSON or form, a malformed body still throws the same 400 as json<T>(),
    // jsonValue(), or form<T>(); nullopt never means "the selected format was
    // invalid". Transport and
    // protocol failures (unreadable body, unsupported Content-Encoding, decoded
    // size over the limit) also throw because they describe the request stream.
    [[nodiscard]] ScopedOperation<std::optional<JsonValue>> jsonValueIf() const;

    template <typename T>
    [[nodiscard]] ScopedOperation<std::optional<T>> jsonIf() const;

    template <typename T>
    [[nodiscard]] ScopedOperation<std::optional<T>> formIf() const;

    template <typename T>
    [[nodiscard]] const T& validated() const;

    template <typename T>
    [[nodiscard]] ValidatedJson<T> validatedJson() const;

    [[nodiscard]] ScopedOperation<std::pmr::vector<MultipartPart>> multipart() const;

    [[nodiscard]] ScopedOperation<RequestFormData> parseBody() const {
        return parseBody(ParseBodyOptions{});
    }

    [[nodiscard]] ScopedOperation<RequestFormData> parseBody(ParseBodyOptions options) const;

    /// Streaming request-body reader for explicit stream routes. Each BodyReader::read()
    /// returns a view valid only until the next read() call (see BodyReader::read).
    [[nodiscard]] BodyReader& bodyReader() const;

    [[nodiscard]] MultipartReader multipartReader() const;

    [[nodiscard]] std::optional<std::string_view> param(std::string_view name) const;

private:
    friend class Context;

    explicit constexpr ContextRequest(const Context& context) noexcept
        : context_(&context) {}

    [[nodiscard]] bool contentTypeMatches(std::string_view expected) const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] const detail::RequestBindings& requestBindings() const noexcept;

    [[nodiscard]] static Task<std::span<const std::byte>> bytesTask(const Context* context);
    [[nodiscard]] static Task<RequestBlob> blobTask(const Context* context);
    [[nodiscard]] static Task<JsonValue> jsonValueTask(const Context* context);
    [[nodiscard]] static Task<std::optional<JsonValue>> jsonValueIfTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<T> jsonModelTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<std::optional<T>> jsonIfModelTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<T> formModelTask(const Context* context);
    template <typename T>
    [[nodiscard]] static Task<std::optional<T>> formIfModelTask(const Context* context);
    [[nodiscard]] static Task<std::string_view> contextTextTask(const Context* context);
    [[nodiscard]] static bool contextContentTypeMatches(const Context* context, std::string_view expected) noexcept;
    [[nodiscard]] static std::pmr::memory_resource* contextResource(const Context* context) noexcept;
    [[nodiscard]] static detail::ScopedOperationScope& contextOperationScope(const Context* context) noexcept;

    const Context* context_{nullptr};
};

}  // namespace ruvia

#include "ruvia/web/detail/http/context/ContextRequestModel.inl"
