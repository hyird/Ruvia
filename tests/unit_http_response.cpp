#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpResponse;

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(response_default_status_text) {
    auto response = makeResponse();
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("OK"));
    response.status(404);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{404});
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Not Found"));
    // Unknown codes fall back by class.
    response.status(599);
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Internal Server Error"));
    response.status(299);
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Bad Request"));
}

RUVIA_TEST(response_custom_status_text_and_normalization) {
    auto response = makeResponse();
    response.status(404, "Totally Missing");
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Totally Missing"));
    // Explicitly passing the default phrase normalizes back to the default.
    response.status(404, "Not Found");
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Not Found"));
}

RUVIA_TEST(response_status_code_range_validated) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(99); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(1000); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.status(100); }));  // lower boundary
    RUVIA_CHECK(!throwsInvalid([&] { response.status(999); }));  // upper boundary
}

RUVIA_TEST(response_status_text_rejects_injection) {
    // A custom reason phrase must not carry response-splitting bytes.
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(200, std::string_view("a\r\nInjected: x", 14)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(200, std::string_view("a\nb", 3)); }));
}
