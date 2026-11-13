#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_file.h"

#include "test_exceptions.h"

namespace {

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
void check_response_body_moves(prepare_type&& prepare, inspect_type&& inspect) {
    for (const bool assign : {false, true}) {
        http_response source;
        prepare(source);
        source.header("X-Source", "retained");
        std::optional<http_response> target;
        if (assign) {
            target.emplace();
            target->body(std::string(256, 't'));
            *target = std::move(source);
        } else {
            target.emplace(std::move(source));
        }
        EXPECT_EQ(target->header("X-Source").value_or("missing"), std::string_view("retained"));
        inspect(*target);
        EXPECT_TRUE(source.headers().empty());
        EXPECT_TRUE(source.body_bytes().empty());
        EXPECT_TRUE(!source.file_body().has_value());
        EXPECT_TRUE(!source.has_multipart_file_body());
        EXPECT_EQ(source.body_segment_count(), std::size_t{0});
        EXPECT_EQ(ruvia::plan_buffered_http_response_write(
                      ruvia::http_known_method::get, source)
                      .content_length(),
            std::uint64_t{0});
        source.body("source reused");
        EXPECT_EQ(source.body_bytes(), std::string_view("source reused"));
    }
}

}  // namespace

TEST(Ruvia, response_public_body_owns_its_source) {
    http_response response;
    std::string source_value = "owned copy";

    response.body(source_value);
    source_value[0] = 'X';

    EXPECT_EQ(response.body_bytes(), std::string_view("owned copy"));
}

TEST(Ruvia, public_response_file_view_exposes_read_only_descriptor) {
    const std::filesystem::path path("public-fixture.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    const ruvia::http_response_file_view file(path.c_str(), 16, 3, 7, identity);

    EXPECT_TRUE(file.to_path() == path);
    EXPECT_EQ(file.size(), std::uint64_t{16});
    EXPECT_EQ(file.offset(), std::uint64_t{3});
    EXPECT_EQ(file.length(), std::uint64_t{7});
    EXPECT_TRUE(file.identity().requires_validation());
    EXPECT_TRUE(file.identity() == identity);
}

TEST(Ruvia, response_file_setter_keeps_typed_identity_and_validates_before_replacement) {
    http_response response;
    const auto path = std::filesystem::path("public-response.bin");
    const auto unchecked = ruvia::http_response_file_identity::unchecked();

    response.body("preserved");
    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(ruvia::http_native_path_view_type{}, 10, 0, 10, unchecked);
    }));
    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, unchecked);
    }));
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), invalid, 0, 0, unchecked); }));
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), 10, invalid, 0, unchecked); }));
        EXPECT_TRUE(throws_invalid_argument([&] { response.file_body(path.native(), 10, 0, invalid, unchecked); }));
    }
    EXPECT_TRUE(!response.file_body().has_value());
    EXPECT_EQ(response.body_bytes(), std::string_view("preserved"));

    response.file_body(path.native(), 16, 3, 7, unchecked);
    auto file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == path);
        EXPECT_TRUE(file->identity() == unchecked);
        EXPECT_TRUE(!file->identity().requires_validation());
    }

    const auto checked = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    response.file_body(std::filesystem::path("checked-response.bin").native(), 16, 3, 7, checked);
    file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->identity() == checked);
        EXPECT_TRUE(file->identity().requires_validation());
    }

    EXPECT_TRUE(throws_invalid_argument([&] {
        response.file_body(std::filesystem::path("invalid-range.bin").native(), 10, 8, 3, checked);
    }));
    file = response.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == std::filesystem::path("checked-response.bin"));
        EXPECT_TRUE(file->identity() == checked);
    }
}

namespace {

std::size_t released_response_file_handles = 0;
ruvia::http_response_file_descriptor::native_type last_released_response_file_handle = 0;

void release_counted_response_file_handle(ruvia::http_response_file_descriptor::native_type value) noexcept {
    ++released_response_file_handles;
    last_released_response_file_handle = value;
}

struct reentrant_response_file_release final {
    std::size_t releases_{0};
    ruvia::http_response_file_descriptor* replacement_target_{nullptr};
    ruvia::http_response_file_descriptor* replacement_source_{nullptr};
    ruvia::http_response* rewritten_response_{nullptr};

    ruvia::http_response_file_descriptor opened() & noexcept {
        return {reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(this), release};
    }

    static void release(ruvia::http_response_file_descriptor::native_type value) noexcept {
        auto& record = *reinterpret_cast<reentrant_response_file_release*>(value);
        ++record.releases_;
        if (record.replacement_target_ != nullptr) {
            *record.replacement_target_ = std::move(*record.replacement_source_);
        }
        if (record.rewritten_response_ != nullptr) {
            record.rewritten_response_->static_body("callback");
        }
    }
};

}  // namespace

TEST(Ruvia, response_file_body_owns_its_opened_handle_until_replacement) {
    released_response_file_handles = 0;
    const auto path = std::filesystem::path("opened-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({1, 2, 3, 4});
    {
        http_response response;
        response.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_descriptor(7, &release_counted_response_file_handle));
        const auto file = response.file_body();
        EXPECT_TRUE(file.has_value());
        if (file) {
            EXPECT_TRUE(file->opened_file() != nullptr);
            if (file->opened_file() != nullptr) {
                EXPECT_EQ(file->opened_file()->native(), ruvia::http_response_file_descriptor::native_type{7});
            }
        }

        // Moving the response moves the handle; nothing is released.
        http_response target;
        target = std::move(response);
        EXPECT_EQ(released_response_file_handles, std::size_t{0});
        EXPECT_TRUE(!response.file_body().has_value());

        // Replacing the body releases the handle exactly once.
        target.body("replaced");
        EXPECT_EQ(released_response_file_handles, std::size_t{1});
        EXPECT_EQ(last_released_response_file_handle, ruvia::http_response_file_descriptor::native_type{7});

        // A rejected setter releases the handle it was given and keeps the body.
        EXPECT_TRUE(throws_invalid_argument([&] {
            target.file_body(path.native(), 10, 8, 3, identity,
                ruvia::http_response_file_descriptor(8, &release_counted_response_file_handle));
        }));
        EXPECT_EQ(released_response_file_handles, std::size_t{2});
        EXPECT_EQ(target.body_bytes(), std::string_view("replaced"));

        // A path-only file body exposes no opened handle.
        target.file_body(path.native(), 16, 0, 16, identity);
        const auto unopened = target.file_body();
        EXPECT_TRUE(unopened.has_value() && unopened->opened_file() == nullptr);

        target.file_body(path.native(), 16, 0, 16, identity,
            ruvia::http_response_file_descriptor(9, &release_counted_response_file_handle));
    }
    // Destroying the response releases the last handle.
    EXPECT_EQ(released_response_file_handles, std::size_t{3});
    EXPECT_EQ(last_released_response_file_handle, ruvia::http_response_file_descriptor::native_type{9});
}

TEST(Ruvia, response_file_handle_move_assignment_preserves_reentrant_replacement) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    reentrant_response_file_release replacement;
    {
        auto target = previous.opened();
        auto source = incoming.opened();
        auto next = replacement.opened();
        previous.replacement_target_ = &target;
        previous.replacement_source_ = &next;

        target = std::move(source);

        EXPECT_FALSE(source);
        EXPECT_FALSE(next);
        EXPECT_EQ(target.native(), reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(&replacement));
        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 1U);
        EXPECT_EQ(replacement.releases_, 0U);
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
    EXPECT_EQ(replacement.releases_, 1U);
}

TEST(Ruvia, response_file_handle_move_assignment_detaches_source_before_retirement) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    {
        auto target = previous.opened();
        auto source = incoming.opened();
        ruvia::http_response_file_descriptor empty;
        previous.replacement_target_ = &source;
        previous.replacement_source_ = &empty;

        target = std::move(source);

        EXPECT_FALSE(source);
        EXPECT_EQ(target.native(), reinterpret_cast<ruvia::http_response_file_descriptor::native_type>(&incoming));
        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 0U);
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
}

TEST(Ruvia, response_file_move_assignment_commits_before_old_body_reuses_source) {
    reentrant_response_file_release previous;
    reentrant_response_file_release incoming;
    const auto path = std::filesystem::path("response-retirement.bin");
    const auto identity = ruvia::http_response_file_identity::unchecked();
    {
        ruvia::http_response source;
        ruvia::http_response target;
        source.file_body(path.native(), 1, 0, 1, identity, incoming.opened());
        source.header("X-Source", "retained");
        target.file_body(path.native(), 1, 0, 1, identity, previous.opened());
        previous.rewritten_response_ = &source;

        target = std::move(source);

        EXPECT_EQ(previous.releases_, 1U);
        EXPECT_EQ(incoming.releases_, 0U);
        EXPECT_TRUE(target.file_body().has_value());
        EXPECT_EQ(target.header("X-Source").value_or(""), "retained");
        EXPECT_EQ(source.body_bytes(), "callback");
        EXPECT_FALSE(source.header("X-Source").has_value());
    }
    EXPECT_EQ(previous.releases_, 1U);
    EXPECT_EQ(incoming.releases_, 1U);
}

TEST(Ruvia, response_file_body_move_preserves_owned_path_and_identity) {
    http_response source_value;
    const auto path = std::filesystem::path("moved-response.bin");
    const auto identity = ruvia::http_response_file_identity::checked({9, 8, 7, 6});
    source_value.file_body(path.native(), 23, 4, 9, identity);

    http_response target;
    target = std::move(source_value);
    const auto file = target.file_body();
    EXPECT_TRUE(file.has_value());
    if (file) {
        EXPECT_TRUE(file->to_path() == path);
        EXPECT_EQ(file->size(), std::uint64_t{23});
        EXPECT_EQ(file->offset(), std::uint64_t{4});
        EXPECT_EQ(file->length(), std::uint64_t{9});
        EXPECT_TRUE(file->identity() == identity);
    }
}

TEST(Ruvia, response_file_body_moves_leave_sources_empty_and_reusable) {
    const std::filesystem::path path("retained-response-file.bin");
    const auto identity = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    check_response_body_moves([&](http_response& source) { source.file_body(path.native(), 23, 4, 9, identity); }, [&](const http_response& target) {
        const auto file = target.file_body();
        EXPECT_TRUE(file.has_value());
        if (file) {
            EXPECT_TRUE(file->to_path() == path);
            EXPECT_EQ(file->size(), std::uint64_t{23});
            EXPECT_EQ(file->offset(), std::uint64_t{4});
            EXPECT_EQ(file->length(), std::uint64_t{9});
            EXPECT_TRUE(file->identity() == identity);
        }
        EXPECT_TRUE(target.body_bytes().empty());
        EXPECT_EQ(target.body_segment_count(), std::size_t{1}); });
}

TEST(Ruvia, empty_file_ranges_remain_segments_after_response_moves) {
    const std::filesystem::path path("empty-response-file.bin");
    const auto identity = ruvia::http_response_file_identity::checked({11, 22, 33, 44});
    for (const std::uint64_t size : {0, 23}) {
        check_response_body_moves([&](http_response& source) { source.file_body(path.native(), size, size, 0, identity); }, [&](const http_response& target) {
            EXPECT_EQ(target.body_segment_count(), std::size_t{1});
            const auto segment = target.body_segment(0);
            EXPECT_TRUE(segment.bytes_.empty());
            EXPECT_TRUE(segment.file_.has_value());
            if (segment.file_) {
                EXPECT_TRUE(segment.file_->to_path() == path);
                EXPECT_EQ(segment.file_->size(), size);
                EXPECT_EQ(segment.file_->offset(), size);
                EXPECT_EQ(segment.file_->length(), std::uint64_t{0});
                EXPECT_TRUE(segment.file_->identity() == identity);
            }
            EXPECT_EQ(ruvia::plan_buffered_http_response_write(
                               ruvia::http_known_method::get, target).content_length(), std::uint64_t{0}); });
    }
}
