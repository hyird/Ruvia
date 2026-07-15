#include "test_harness.h"

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/HttpResponseBody.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/HttpResponseFileBody.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using ruvia::HttpResponse;
using ruvia::detail::HttpResponseBody;
using ruvia::detail::materializeResponseBody;
using ruvia::detail::responseBody;
using ruvia::detail::setResponseBodyBorrowedView;
using ruvia::detail::setResponseBodyOwned;
using ruvia::detail::setResponseBodyStaticView;
using ruvia::detail::setResponseBorrowedFileBody;
using ruvia::detail::setResponseFileBody;

static_assert(std::is_same_v<
    decltype(responseBody(std::declval<const HttpResponse&>())),
    const HttpResponseBody&>);
static_assert(!std::is_copy_constructible_v<HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<HttpResponseBody>);
static_assert(!std::is_move_assignable_v<HttpResponseBody>);
static_assert(std::is_nothrow_move_constructible_v<HttpResponse>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::HttpBorrowedResponseBytes>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::HttpStaticResponseBytes>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::HttpOwnedResponseFile>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::HttpBorrowedResponseFile>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ResponseFileBody>);

template <typename T>
concept ExposesAnyRvalueResponseBodyBorrow =
    requires(T&& value) { std::move(value).empty(); } ||
    requires(T&& value) { std::move(value).borrowedBytes(); } ||
    requires(T&& value) { std::move(value).staticBytes(); } ||
    requires(T&& value) { std::move(value).ownedBytes(); } ||
    requires(T&& value) { std::move(value).ownedFile(); } ||
    requires(T&& value) { std::move(value).borrowedFile(); } ||
    requires(T&& value) { std::move(value).bytes(); } ||
    requires(T&& value) { std::move(value).file(); } ||
    requires(T&& value) { std::move(value).nativePathCStr(); };

template <typename T>
concept ExposesRvalueResponseBodyAccess =
    requires(T&& response) { responseBody(std::move(response)); } ||
    requires(T&& response) {
        ruvia::detail::HttpResponseBodyAccess::body(std::move(response));
    };

static_assert(!ExposesAnyRvalueResponseBodyBorrow<HttpResponseBody>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<
    ruvia::detail::HttpOwnedResponseBytes>);
static_assert(!ExposesAnyRvalueResponseBodyBorrow<
    ruvia::detail::HttpOwnedResponseFile>);
static_assert(!ExposesRvalueResponseBodyAccess<HttpResponse>);

[[nodiscard]] std::size_t activeAlternativeCount(
    const HttpResponseBody& body) noexcept {
    return static_cast<std::size_t>(body.empty() != nullptr) +
        static_cast<std::size_t>(body.borrowedBytes() != nullptr) +
        static_cast<std::size_t>(body.staticBytes() != nullptr) +
        static_cast<std::size_t>(body.ownedBytes() != nullptr) +
        static_cast<std::size_t>(body.ownedFile() != nullptr) +
        static_cast<std::size_t>(body.borrowedFile() != nullptr);
}

template <typename Function>
[[nodiscard]] bool throwsInvalidArgument(Function&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(response_body_has_one_storage_alternative) {
    HttpResponse response(std::pmr::new_delete_resource());

    RUVIA_CHECK(responseBody(response).empty() != nullptr);
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    std::string borrowedStorage = "borrowed";
    setResponseBodyBorrowedView(response, borrowedStorage);
    RUVIA_CHECK(responseBody(response).borrowedBytes() != nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("borrowed"));
    RUVIA_CHECK(!responseBody(response).file().has_value());
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    setResponseBodyStaticView(response, "static");
    RUVIA_CHECK(responseBody(response).staticBytes() != nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("static"));
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    std::pmr::string owned("owned", std::pmr::new_delete_resource());
    setResponseBodyOwned(response, std::move(owned));
    RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("owned"));
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    response.body({});
    RUVIA_CHECK(responseBody(response).empty() != nullptr);
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});
}

RUVIA_TEST(response_public_body_owns_its_source) {
    HttpResponse response(std::pmr::new_delete_resource());
    std::string source = "owned copy";

    response.body(source);
    source[0] = 'X';

    RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
    RUVIA_CHECK(responseBody(response).borrowedBytes() == nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("owned copy"));
}

RUVIA_TEST(response_body_materializes_only_ephemeral_borrow) {
    HttpResponse response(std::pmr::new_delete_resource());
    std::string source = "ephemeral";
    setResponseBodyBorrowedView(response, source);

    materializeResponseBody(response);
    source[0] = 'X';
    RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
    RUVIA_CHECK(responseBody(response).borrowedBytes() == nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("ephemeral"));

    setResponseBodyStaticView(response, "process-lifetime");
    materializeResponseBody(response);
    RUVIA_CHECK(responseBody(response).staticBytes() != nullptr);
    RUVIA_CHECK(responseBody(response).ownedBytes() == nullptr);
    RUVIA_CHECK_EQ(
        responseBody(response).bytes(),
        std::string_view("process-lifetime"));
}

RUVIA_TEST(response_body_file_view_is_atomic_and_non_default) {
    HttpResponse response(std::pmr::new_delete_resource());
    const std::filesystem::path ownedPath("owned-fixture.bin");
    setResponseFileBody(response, ownedPath, 20, 5, 7);

    RUVIA_CHECK(responseBody(response).ownedFile() != nullptr);
    RUVIA_CHECK(responseBody(response).borrowedFile() == nullptr);
    RUVIA_CHECK(responseBody(response).bytes().empty());
    RUVIA_CHECK_EQ(responseBody(response).size(), std::size_t{7});
    const auto ownedFile = responseBody(response).file();
    RUVIA_CHECK(ownedFile.has_value());
    RUVIA_CHECK(ownedFile->toPath() == ownedPath);
    RUVIA_CHECK_EQ(ownedFile->size(), std::uint64_t{20});
    RUVIA_CHECK_EQ(ownedFile->offset(), std::uint64_t{5});
    RUVIA_CHECK_EQ(ownedFile->length(), std::uint64_t{7});
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    const std::filesystem::path borrowedPath("borrowed-fixture.bin");
    setResponseBorrowedFileBody(response, borrowedPath, 12, 2, 4);
    RUVIA_CHECK(responseBody(response).ownedFile() == nullptr);
    RUVIA_CHECK(responseBody(response).borrowedFile() != nullptr);
    const auto borrowedFile = responseBody(response).file();
    RUVIA_CHECK(borrowedFile.has_value());
    RUVIA_CHECK(borrowedFile->toPath() == borrowedPath);
    RUVIA_CHECK_EQ(borrowedFile->size(), std::uint64_t{12});
    RUVIA_CHECK_EQ(borrowedFile->offset(), std::uint64_t{2});
    RUVIA_CHECK_EQ(borrowedFile->length(), std::uint64_t{4});
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(response)), std::size_t{1});

    // A zero-length file remains a file alternative: opening/framing policy must
    // not silently collapse it into the distinct empty-body state.
    setResponseFileBody(response, ownedPath, 0);
    RUVIA_CHECK(responseBody(response).ownedFile() != nullptr);
    RUVIA_CHECK(responseBody(response).file().has_value());
    RUVIA_CHECK_EQ(responseBody(response).size(), std::size_t{0});
    RUVIA_CHECK(responseBody(response).empty() == nullptr);
}

RUVIA_TEST(response_body_file_transition_validates_before_replacement) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.body("preserved");

    RUVIA_CHECK(throwsInvalidArgument([&] {
        setResponseFileBody(response, std::filesystem::path{}, 10);
    }));
    RUVIA_CHECK(throwsInvalidArgument([&] {
        setResponseFileBody(
            response,
            std::filesystem::path("invalid-range.bin"),
            10,
            8,
            3);
    }));
    RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("preserved"));
}

RUVIA_TEST(response_body_move_preserves_active_alternative) {
    std::pmr::monotonic_buffer_resource sourceResource;
    std::pmr::monotonic_buffer_resource targetResource;
    HttpResponse source(&sourceResource);
    HttpResponse target(&targetResource);
    source.body("move-owned");
    setResponseBodyBorrowedView(target, "replaced");

    target = std::move(source);
    RUVIA_CHECK(target.headers().empty());
    RUVIA_CHECK(target.status() == 200);
    RUVIA_CHECK(!target.header("missing").has_value());
    RUVIA_CHECK(target.headers().size() == 0);
    RUVIA_CHECK(responseBody(target).ownedBytes() != nullptr);
    RUVIA_CHECK_EQ(responseBody(target).bytes(), std::string_view("move-owned"));
    RUVIA_CHECK_EQ(activeAlternativeCount(responseBody(target)), std::size_t{1});
}
