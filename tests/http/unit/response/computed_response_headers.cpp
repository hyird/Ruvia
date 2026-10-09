#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

using ruvia::http_known_method;
using ruvia::http_response;

constexpr std::uint32_t method_bit(http_known_method method) {
    return std::uint32_t{1} << static_cast<std::uint32_t>(method);
}

http_response make_response() {
    return http_response({.resource_ = std::pmr::new_delete_resource()});
}

}  // namespace

// Response fields Ruvia formats itself: Content-Range and Allow.

RUVIA_TEST(allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = make_response();
    many.allow_methods(method_bit(http_known_method::get) |
                       method_bit(http_known_method::post) |
                       method_bit(http_known_method::head));
    RUVIA_CHECK_EQ(many.header("Allow").value_or(""), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = make_response();
    one.allow_methods(method_bit(http_known_method::delete_value));
    RUVIA_CHECK_EQ(one.header("Allow").value_or(""), std::string_view("DELETE"));

    std::string extension = "PROPFIND";
    const std::string_view extension_methods[]{extension};
    one.allow_methods(method_bit(http_known_method::get), extension_methods);
    extension.assign("CHANGED!");
    RUVIA_CHECK_EQ(one.header("Allow").value_or(""), std::string_view("GET, PROPFIND"));
}

RUVIA_TEST(allow_header_rejects_invalid_extension_methods_without_mutation) {
    const std::array invalid_methods{std::string_view{}, std::string_view("BAD METHOD"),
        std::string_view("GET, POST"), std::string_view("BAD\r\nX-Injected: yes"),
        std::string_view("BAD\0METHOD", 10), std::string_view("BAD\xff", 4)};
    for (const auto method : invalid_methods) {
        auto response = make_response();
        response.header("Allow", "GET");
        response.header("X-Keep", "unchanged");
        const std::array extension_methods{std::string_view("PROPFIND"), method};
        bool rejected = false;
        try {
            response.allow_methods(method_bit(http_known_method::post), extension_methods);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("GET"));
        RUVIA_CHECK_EQ(response.header("X-Keep").value_or(""), std::string_view("unchanged"));
    }
}

RUVIA_TEST(allow_header_reorders_methods_borrowed_from_current_field) {
    auto response = make_response();
    response.header("Allow", "FOO, BAR");
    const auto current = response.header("Allow").value();
    const std::array extension_methods{current.substr(5), current.substr(0, 3)};
    response.allow_methods(0, extension_methods);
    RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("BAR, FOO"));
}

RUVIA_TEST(allow_header_lists_connect_with_other_supported_methods) {
    auto response = make_response();
    response.allow_methods(method_bit(http_known_method::connect));
    RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("CONNECT"));
    const std::array extension_methods{std::string_view("PROPFIND")};
    response.allow_methods(method_bit(http_known_method::get) | method_bit(http_known_method::options) |
                               method_bit(http_known_method::connect),
        extension_methods);
    RUVIA_CHECK_EQ(response.header("Allow").value_or(""),
        std::string_view("GET, OPTIONS, CONNECT, PROPFIND"));
}

RUVIA_TEST(allow_header_accepts_extension_tokens_and_empty_method_set) {
    auto response = make_response();
    const std::array extension_methods{std::string_view("custom"), std::string_view("!#$%&'*+-.^_`|~")};
    response.allow_methods(method_bit(http_known_method::get), extension_methods);
    RUVIA_CHECK_EQ(response.header("Allow").value_or(""),
        std::string_view("GET, custom, !#$%&'*+-.^_`|~"));
    response.allow_methods(0);
    RUVIA_CHECK_EQ(response.header("Allow"), std::optional(std::string_view{}));
}

RUVIA_TEST(allow_header_preserves_borrowed_methods_when_replacement_allocation_fails) {
    failing_memory_resource resource;
    {
        http_response response({.resource_ = &resource});
        response.header("Allow", "FOO, BAR");
        const auto current = response.header("Allow").value();
        const std::array extension_methods{current.substr(5), current.substr(0, 3)};
        const auto live_allocations = resource.live_allocations();
        resource.fail_after(0);
        bool failed = false;
        try {
            response.allow_methods(method_bit(http_known_method::get), extension_methods);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("FOO, BAR"));
        RUVIA_CHECK_EQ(current, std::string_view("FOO, BAR"));
        RUVIA_CHECK_EQ(resource.live_allocations(), live_allocations);
        response.allow_methods(method_bit(http_known_method::get), extension_methods);
        RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("GET, BAR, FOO"));

        const std::array external_methods{std::string_view("FOO"), std::string_view("BAR")};
        resource.fail_after(0);
        response.allow_methods(method_bit(http_known_method::get), external_methods);
        resource.allow_allocations();
        RUVIA_CHECK_EQ(response.header("Allow").value_or(""), std::string_view("GET, FOO, BAR"));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
