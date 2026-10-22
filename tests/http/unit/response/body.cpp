#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"

#include "failing_memory_resource.h"
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

template <typename prepare_type, typename inspect_type>
void check_response_body_moves(ruvia::testing::test_context& ruvia_ctx,
    prepare_type&& prepare, inspect_type&& inspect) {
    for (const bool assign : {false, true}) {
        failing_memory_resource source_resource;
        failing_memory_resource target_resource;
        {
            http_response source(&source_resource);
            prepare(source);
            source.header("X-Source", "retained");
            std::optional<http_response> target;
            if (assign) {
                target.emplace(&target_resource);
                target->body(std::string(256, 't'));
            }
            source_resource.fail_after(0);
            target_resource.fail_after(0);
            if (assign) {
                *target = std::move(source);
            } else {
                target.emplace(std::move(source));
            }
            source_resource.allow_allocations();
            target_resource.allow_allocations();
            RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
            RUVIA_CHECK(target->memory_resource() == &source_resource);
            RUVIA_CHECK_EQ(target->header("X-Source").value_or("missing"), std::string_view("retained"));
            inspect(*target);
            RUVIA_CHECK(source.headers().empty());
            RUVIA_CHECK(source.body_bytes().empty());
            RUVIA_CHECK(!source.file_body().has_value());
            RUVIA_CHECK(!source.has_multipart_file_body());
            RUVIA_CHECK_EQ(source.body_segment_count(), std::size_t{0});
            RUVIA_CHECK_EQ(ruvia::plan_buffered_http_response_write(
                               ruvia::http_known_method::get, source)
                               .content_length(),
                std::uint64_t{0});
            source.body("source reused");
            source.header("X-Source", "replacement");
            RUVIA_CHECK_EQ(source.body_bytes(), std::string_view("source reused"));
            RUVIA_CHECK_EQ(source.header("X-Source").value_or("missing"), std::string_view("replacement"));
            RUVIA_CHECK_EQ(target->header("X-Source").value_or("missing"), std::string_view("retained"));
            inspect(*target);
        }
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
    }
}

}  // namespace

RUVIA_TEST(response_public_body_owns_its_source) {
    http_response response(std::pmr::new_delete_resource());
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
    http_response response(std::pmr::new_delete_resource());
    const auto path = std::filesystem::path("public-response.bin");
    const auto unchecked = ruvia::http_response_file_identity::unchecked();

    response.body("preserved");
    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(ruvia::http_native_path_view_type{}, 10, 0, 10, unchecked);
    }));
    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, unchecked);
    }));
    RUVIA_CHECK(!response.file_body().has_value());
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("preserved"));

    response.file_body(path.native(), 16, 3, 7, unchecked);
    auto file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->to_path() == path);
        RUVIA_CHECK(file->identity() == unchecked);
        RUVIA_CHECK(!file->identity().requires_validation());
    }

    const auto checked = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    response.file_body(std::filesystem::path("checked-response.bin").native(), 16, 3, 7, checked);
    file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->identity() == checked);
        RUVIA_CHECK(file->identity().requires_validation());
    }

    RUVIA_CHECK(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, checked);
    }));
    file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file) {
        RUVIA_CHECK(file->to_path() == std::filesystem::path("checked-response.bin"));
        RUVIA_CHECK(file->identity() == checked);
    }
}

namespace {

std::size_t released_response_file_handles = 0;
ruvia::http_response_file_handle::native_type last_released_response_file_handle = 0;

void release_counted_response_file_handle(ruvia::http_response_file_handle::native_type value) noexcept {
    ++released_response_file_handles;
    last_released_response_file_handle = value;
}

}  // namespace

RUVIA_TEST(response_file_body_owns_its_opened_handle_until_replacement) {
    released_response_file_handles = 0;
    const auto path = std::filesystem::path("opened-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    {
        http_response response(std::pmr::new_delete_resource());
        response.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_handle(7, &release_counted_response_file_handle));
        const auto file = response.file_body();
        RUVIA_CHECK(file.has_value());
        if (file) {
            RUVIA_CHECK(file->opened_handle() != nullptr);
            if (file->opened_handle() != nullptr) {
                RUVIA_CHECK_EQ(file->opened_handle()->native(), ruvia::http_response_file_handle::native_type{7});
            }
        }

        // Moving the response moves the handle; nothing is released.
        http_response target(std::pmr::new_delete_resource());
        target = std::move(response);
        RUVIA_CHECK_EQ(released_response_file_handles, std::size_t{0});
        RUVIA_CHECK(!response.file_body().has_value());

        // Replacing the body releases the handle exactly once.
        target.body("replaced");
        RUVIA_CHECK_EQ(released_response_file_handles, std::size_t{1});
        RUVIA_CHECK_EQ(last_released_response_file_handle, ruvia::http_response_file_handle::native_type{7});

        // A rejected setter releases the handle it was given and keeps the body.
        RUVIA_CHECK(throws_invalid_argument([&] {
            target.file_body(path.native(), 10, 8, 3, identity,
                ruvia::http_response_file_handle(8, &release_counted_response_file_handle));
        }));
        RUVIA_CHECK_EQ(released_response_file_handles, std::size_t{2});
        RUVIA_CHECK_EQ(target.body_bytes(), std::string_view("replaced"));

        // A path-only file body exposes no opened handle.
        target.file_body(path.native(), 16, 0, 16, identity);
        const auto unopened = target.file_body();
        RUVIA_CHECK(unopened.has_value() && unopened->opened_handle() == nullptr);

        target.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_handle(9, &release_counted_response_file_handle));
    }
    // Destroying the response releases the last handle.
    RUVIA_CHECK_EQ(released_response_file_handles, std::size_t{3});
    RUVIA_CHECK_EQ(last_released_response_file_handle, ruvia::http_response_file_handle::native_type{9});
}

RUVIA_TEST(response_file_body_move_preserves_owned_path_and_identity) {
    http_response source_value(std::pmr::new_delete_resource());
    const auto path = std::filesystem::path("moved-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({9, 8, 7, 6});
    source_value.file_body(path.native(), 23, 4, 9, identity);

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
    http_response response(&response_resource);
    response.body("preserved");
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "allocation_test", {}, std::pmr::new_delete_resource());

    response_resource.fail_ = true;
    bool threw = false;
    try {
        response.multipart_file_body(std::filesystem::path("multipart-response-fixture.bin").native(), 10,
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
    http_response source_value(&source_resource);
    http_response target(&target_resource);
    source_value.body("move-owned");
    target.body("replaced");

    target = std::move(source_value);
    RUVIA_CHECK(target.headers().empty());
    RUVIA_CHECK(target.status() == ruvia::http_status::ok);
    RUVIA_CHECK(!target.header("missing").has_value());
    RUVIA_CHECK(target.headers().size() == 0);
    RUVIA_CHECK_EQ(target.body_bytes(), std::string_view("move-owned"));
}

RUVIA_TEST(response_byte_body_moves_leave_sources_empty_and_reusable) {
    enum class body_kind { copied,
        owned,
        static_value,
        empty };
    for (const auto kind : {body_kind::copied, body_kind::owned, body_kind::static_value, body_kind::empty}) {
        constexpr std::string_view payload = "moved response payload";
        const auto expected = kind == body_kind::empty ? std::string_view{} : payload;
        check_response_body_moves(ruvia_ctx, [kind, payload](http_response& source) {
            switch (kind) {
                case body_kind::copied:
                    source.body(payload);
                    break;
                case body_kind::owned:
                    source.owned_body(std::pmr::string(payload, source.memory_resource()));
                    break;
                case body_kind::static_value:
                    source.static_body(payload);
                    break;
                case body_kind::empty:
                    break;
            } }, [&](const http_response& target) {
            RUVIA_CHECK_EQ(target.body_bytes(), expected);
            RUVIA_CHECK(!target.file_body().has_value());
            RUVIA_CHECK_EQ(target.body_segment_count(), expected.empty() ? std::size_t{0} : std::size_t{1}); });
    }
}

RUVIA_TEST(response_file_body_moves_leave_sources_empty_and_reusable) {
    const std::filesystem::path path("retained-response-file.bin");
    const auto identity = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    check_response_body_moves(ruvia_ctx, [&](http_response& source) { source.file_body(path.native(), 23, 4, 9, identity); }, [&](const http_response& target) {
        const auto file = target.file_body();
        RUVIA_CHECK(file.has_value());
        if (file) {
            RUVIA_CHECK(file->to_path() == path);
            RUVIA_CHECK_EQ(file->size(), std::uint64_t{23});
            RUVIA_CHECK_EQ(file->offset(), std::uint64_t{4});
            RUVIA_CHECK_EQ(file->length(), std::uint64_t{9});
            RUVIA_CHECK(file->identity() == identity);
        }
        RUVIA_CHECK(target.body_bytes().empty());
        RUVIA_CHECK_EQ(target.body_segment_count(), std::size_t{1}); });
}

RUVIA_TEST(response_multipart_body_moves_leave_sources_empty_and_reusable) {
    const std::filesystem::path path("retained-multipart-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({55, 66, 77, 88});
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    constexpr std::string_view expected =
        "--moved_ranges\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-1/10\r\n\r\n01\r\n"
        "--moved_ranges\r\nContent-Type: text/plain\r\nContent-Range: bytes 8-9/10\r\n\r\n89\r\n"
        "--moved_ranges--\r\n";
    check_response_body_moves(ruvia_ctx, [&](http_response& source) {
        auto plan = ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain", "moved_ranges", {}, source.memory_resource());
        source.multipart_file_body(path.native(), 10, identity, std::move(plan)); }, [&](const http_response& target) {
        RUVIA_CHECK(target.has_multipart_file_body());
        std::string materialized;
        constexpr std::string_view file_bytes = "0123456789";
        for (std::size_t index = 0; index < target.body_segment_count(); ++index) {
            const auto segment = target.body_segment(index);
            if (segment.file_) {
                RUVIA_CHECK(segment.file_->to_path() == path);
                RUVIA_CHECK(segment.file_->identity() == identity);
                materialized.append(file_bytes.substr(static_cast<std::size_t>(segment.file_->offset()),
                    static_cast<std::size_t>(segment.file_->length())));
            } else {
                materialized.append(segment.bytes_);
            }
        }
        RUVIA_CHECK_EQ(materialized, expected);
        RUVIA_CHECK_EQ(ruvia::plan_buffered_http_response_write(
                           ruvia::http_known_method::get, target).content_length(),
            static_cast<std::uint64_t>(expected.size())); });
}
