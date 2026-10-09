#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

using ruvia::http_response;
using ruvia::detail::apply_response_content_encoding;
using ruvia::detail::replace_response_body_with_content_encoding;
using ruvia::detail::response_body;

http_response make_response() {
    return http_response({.resource_ = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(apply_content_encoding_sets_coding_drops_identity_length_and_weakens_etag) {
    auto response = make_response();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    apply_response_content_encoding(response, "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(response_body(response).bytes(), std::string_view("identity"));
}

RUVIA_TEST(apply_content_encoding_leaves_weak_malformed_and_absent_etags) {
    auto weak = make_response();
    weak.header("ETag", "W/\"v1\"");
    apply_response_content_encoding(weak, "br");
    RUVIA_CHECK_EQ(weak.header("ETag"), std::string_view("W/\"v1\""));

    auto malformed = make_response();
    malformed.header("ETag", "v1");
    apply_response_content_encoding(malformed, "zstd");
    RUVIA_CHECK_EQ(malformed.header("ETag"), std::string_view("v1"));

    auto absent = make_response();
    apply_response_content_encoding(absent, "gzip");
    RUVIA_CHECK(!absent.header("ETag").has_value());
    RUVIA_CHECK_EQ(absent.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(encoded_representation_update_owns_aliased_coding_and_collapses_appended_fields) {
    for (bool buffered : {false, true}) {
        auto response = make_response();
        const std::string coding = "custom-" + std::string(48, 'c');
        response.header("Content-Encoding", coding, {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Content-Encoding", "gzip", {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Content-Length", "8");
        response.header("ETag", "\"v1\"");
        response.body("identity");
        const auto aliased = response.header("Content-Encoding").value();
        if (buffered) {
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            response.replace_body_with_content_encoding(std::move(encoded), aliased);
        } else {
            response.apply_content_encoding(aliased);
        }
        RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view(coding));
        std::size_t encoding_fields = 0;
        for (const auto& field : response.headers()) {
            if (field.name() == "Content-Encoding") {
                ++encoding_fields;
            }
        }
        RUVIA_CHECK_EQ(encoding_fields, std::size_t{1});
        RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
        if (buffered) {
            RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
        } else {
            RUVIA_CHECK(!response.header("Content-Length"));
        }
    }
}

RUVIA_TEST(apply_content_encoding_rejects_empty_coding) {
    auto response = make_response();
    response.header("ETag", "\"v1\"");
    bool rejected = false;
    try {
        apply_response_content_encoding(response, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(encoded_representation_update_rejects_invalid_coding_without_mutation) {
    const std::array invalid_codings{std::string_view{}, std::string_view("gzip;level=9"),
        std::string_view("bad coding"), std::string_view("gzip/deflate"),
        std::string_view(",gzip"), std::string_view("gzip,"), std::string_view("gzip,,br"),
        std::string_view("gzip\r\nX-Injected: yes"), std::string_view("gzip\0br", 7)};
    for (const bool buffered : {false, true}) {
        for (const auto coding : invalid_codings) {
            auto response = make_response();
            response.body("identity");
            response.header("Content-Length", "8");
            response.header("ETag", "\"v1\"");
            response.header("Content-Encoding", "identity");
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            bool rejected = false;
            try {
                if (buffered) {
                    response.replace_body_with_content_encoding(std::move(encoded), coding);
                } else {
                    response.apply_content_encoding(coding);
                }
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("identity"));
            RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("8"));
            RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
            RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("identity"));
            RUVIA_CHECK_EQ(encoded, std::string_view("compressed"));
        }
    }
}

RUVIA_TEST(encoded_representation_update_accepts_coding_lists_and_extension_codings) {
    for (const bool buffered : {false, true}) {
        for (const std::string_view coding : {"gzip, br", "custom-code", "gzip,\tbr"}) {
            auto response = make_response();
            response.body("identity");
            response.header("Content-Length", "8");
            response.header("ETag", "\"v1\"");
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            if (buffered) {
                response.replace_body_with_content_encoding(std::move(encoded), coding);
            } else {
                response.apply_content_encoding(coding);
            }
            RUVIA_CHECK_EQ(response.header("Content-Encoding"), coding);
            RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
            if (buffered) {
                RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("compressed"));
                RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
            } else {
                RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("identity"));
                RUVIA_CHECK(!response.header("Content-Length"));
            }
        }
    }
}

RUVIA_TEST(encoded_representation_update_preserves_values_at_each_allocation_failure) {
    const std::string identity(192, 'i');
    const std::string etag = "\"" + std::string(100, 'v') + "\"";
    const std::string coding = "custom-" + std::string(48, 'c');
    const std::string weak_etag = "W/" + etag;
    for (bool buffered : {false, true}) {
        bool succeeded = false;
        std::size_t failures = 0;
        for (std::size_t allowance = 0; allowance != 32 && !succeeded; ++allowance) {
            failing_memory_resource memory;
            {
                http_response response({.resource_ = &memory});
                response.body(identity);
                response.header("Content-Length", "192");
                response.header("ETag", etag);
                // Adding the encoding must also grow the inline header block.
                for (unsigned field = 0; field != 6; ++field) {
                    response.header("X-Fill-" + std::to_string(field), "retained");
                }
                const auto initial_count = response.headers().size();
                std::pmr::string encoded(256, 'e', std::pmr::new_delete_resource());
                memory.fail_after(allowance);
                try {
                    if (buffered) {
                        response.replace_body_with_content_encoding(std::move(encoded), coding);
                    } else {
                        response.apply_content_encoding(coding);
                    }
                    succeeded = true;
                } catch (const std::bad_alloc&) {
                    ++failures;
                    RUVIA_CHECK_EQ(response_body(response).bytes(), std::string_view(identity));
                    RUVIA_CHECK_EQ(response.headers().size(), initial_count);
                    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view(etag));
                    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("192"));
                    RUVIA_CHECK(!response.header("Content-Encoding"));
                }
                memory.allow_allocations();
                for (unsigned field = 0; field != 6; ++field) {
                    RUVIA_CHECK_EQ(response.header("X-Fill-" + std::to_string(field)), std::string_view("retained"));
                }
                if (succeeded) {
                    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view(coding));
                    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view(weak_etag));
                    if (buffered) {
                        RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("256"));
                        RUVIA_CHECK_EQ(response_body(response).bytes().size(), std::size_t{256});
                    } else {
                        RUVIA_CHECK(!response.header("Content-Length"));
                        RUVIA_CHECK_EQ(response_body(response).bytes(), std::string_view(identity));
                    }
                }
            }
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        }
        RUVIA_CHECK(succeeded);
        RUVIA_CHECK(failures >= 2);
    }
}

RUVIA_TEST(replace_body_with_content_encoding_commits_representation_and_weakens_etag) {
    auto response = make_response();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
    replace_response_body_with_content_encoding(response, std::move(encoded), "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(response_body(response).bytes(), std::string_view("compressed"));
}
