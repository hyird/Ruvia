#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"

#include "test_harness.h"

namespace {

class switchable_failing_resource final : public std::pmr::memory_resource {
public:
    bool fail_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using ruvia::http_response;

template <typename function_type>
[[nodiscard]] bool throws_invalid_argument(function_type&& function) {
    try {
        function();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(response_public_body_owns_its_source) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    std::string source_value = "owned copy";

    response.body(source_value);
    source_value[0] = 'X';

    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("owned copy"));
}

RUVIA_TEST(public_response_file_view_exposes_read_only_descriptor) {
    const std::filesystem::path path("public-fixture.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    const ruvia::http_response_file_view file(path.c_str(), 16, 3, 7, identity);

    RUVIA_CHECK(file.to_path() == path);
    RUVIA_CHECK_EQ(file.size(), std::uint64_t{16});
    RUVIA_CHECK_EQ(file.offset(), std::uint64_t{3});
    RUVIA_CHECK_EQ(file.length(), std::uint64_t{7});
    RUVIA_CHECK(file.identity().requires_validation());
    RUVIA_CHECK(file.identity() == identity);
}

RUVIA_TEST(response_file_setter_keeps_typed_identity_and_validates_before_replacement) {
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    const auto path = std::filesystem::path("public-response.bin");
    const auto unchecked = ruvia::http_response_file_identity::unchecked();

    response.body("preserved");
    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path{}, 10, 0, 10, unchecked);
    }));
    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin"), 10, 8, 3, unchecked);
    }));
    RUVIA_CHECK(!response.file_body().has_value());
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("preserved"));

    response.file_body(path, 16, 3, 7, unchecked);
    auto file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->to_path() == path);
        RUVIA_CHECK(file->identity() == unchecked);
        RUVIA_CHECK(!file->identity().requires_validation());
    }

    const auto checked = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    response.file_body(std::filesystem::path("checked-response.bin"), 16, 3, 7, checked);
    file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->identity() == checked);
        RUVIA_CHECK(file->identity().requires_validation());
    }

    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin"), 10, 8, 3, checked);
    }));
    file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->to_path() == std::filesystem::path("checked-response.bin"));
        RUVIA_CHECK(file->identity() == checked);
    }
}

RUVIA_TEST(response_file_body_move_preserves_owned_path_and_identity) {
    http_response source_value({.resource_ = std::pmr::new_delete_resource()});
    const auto path = std::filesystem::path("moved-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({9, 8, 7, 6});
    source_value.file_body(path, 23, 4, 9, identity);

    http_response target;
    target = std::move(source_value);
    const auto file = target.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->to_path() == path);
        RUVIA_CHECK_EQ(file->size(), std::uint64_t{23});
        RUVIA_CHECK_EQ(file->offset(), std::uint64_t{4});
        RUVIA_CHECK_EQ(file->length(), std::uint64_t{9});
        RUVIA_CHECK(file->identity() == identity);
    }
}

RUVIA_TEST(multipart_body_allocation_failure_preserves_previous_body) {
    switchable_failing_resource response_resource;
    http_response response({.resource_ = &response_resource});
    response.body("preserved");
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "allocation_test", {}, std::pmr::new_delete_resource());

    response_resource.fail_ = true;
    bool threw = false;
    try {
        response.multipart_file_body("multipart-response-fixture.bin", 10,
            ruvia::http_response_file_identity::unchecked(), std::move(plan));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("preserved"));
    RUVIA_CHECK(!response.has_multipart_file_body());
}

RUVIA_TEST(response_body_move_preserves_content) {
    std::pmr::monotonic_buffer_resource source_resource;
    std::pmr::monotonic_buffer_resource target_resource;
    http_response source_value({.resource_ = &source_resource});
    http_response target({.resource_ = &target_resource});
    source_value.body("move-owned");
    target.body("replaced");

    target = std::move(source_value);
    RUVIA_CHECK(target.headers().empty());
    RUVIA_CHECK(target.status() == ruvia::http_status::ok);
    RUVIA_CHECK(!target.header("missing").has_value());
    RUVIA_CHECK(target.headers().size() == 0);
    RUVIA_CHECK_EQ(target.body_bytes(), std::string_view("move-owned"));
}
