#include <array>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::applyResponseContentEncoding;
using ruvia::detail::replaceResponseBodyWithContentEncoding;
using ruvia::detail::responseBody;

HttpResponse makeResponse() {
    return HttpResponse({.resource = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(apply_content_encoding_sets_coding_drops_identity_length_and_weakens_etag) {
    auto response = makeResponse();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    applyResponseContentEncoding(response, "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("identity"));
}

RUVIA_TEST(apply_content_encoding_leaves_weak_malformed_and_absent_etags) {
    auto weak = makeResponse();
    weak.header("ETag", "W/\"v1\"");
    applyResponseContentEncoding(weak, "br");
    RUVIA_CHECK_EQ(weak.header("ETag"), std::string_view("W/\"v1\""));

    auto malformed = makeResponse();
    malformed.header("ETag", "v1");
    applyResponseContentEncoding(malformed, "zstd");
    RUVIA_CHECK_EQ(malformed.header("ETag"), std::string_view("v1"));

    auto absent = makeResponse();
    applyResponseContentEncoding(absent, "gzip");
    RUVIA_CHECK(!absent.header("ETag").has_value());
    RUVIA_CHECK_EQ(absent.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(encoded_representation_update_owns_aliased_coding_and_collapses_appended_fields) {
    for (bool buffered : {false, true}) {
        auto response = makeResponse();
        const std::string coding = "custom-" + std::string(48, 'c');
        response.header("Content-Encoding", coding, {.mode = ruvia::HttpResponseHeaderMode::kAppend});
        response.header("Content-Encoding", "gzip", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
        response.header("Content-Length", "8");
        response.header("ETag", "\"v1\"");
        response.body("identity");
        const auto aliased = response.header("Content-Encoding").value();
        if (buffered) {
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            response.replaceBodyWithContentEncoding(std::move(encoded), aliased);
        } else {
            response.applyContentEncoding(aliased);
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
    auto response = makeResponse();
    response.header("ETag", "\"v1\"");
    bool rejected = false;
    try {
        applyResponseContentEncoding(response, {});
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
            auto response = makeResponse();
            response.body("identity");
            response.header("Content-Length", "8");
            response.header("ETag", "\"v1\"");
            response.header("Content-Encoding", "identity");
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            bool rejected = false;
            try {
                if (buffered) {
                    response.replaceBodyWithContentEncoding(std::move(encoded), coding);
                } else {
                    response.applyContentEncoding(coding);
                }
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("identity"));
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
            auto response = makeResponse();
            response.body("identity");
            response.header("Content-Length", "8");
            response.header("ETag", "\"v1\"");
            std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
            if (buffered) {
                response.replaceBodyWithContentEncoding(std::move(encoded), coding);
            } else {
                response.applyContentEncoding(coding);
            }
            RUVIA_CHECK_EQ(response.header("Content-Encoding"), coding);
            RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
            if (buffered) {
                RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("compressed"));
                RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
            } else {
                RUVIA_CHECK_EQ(response.bodyBytes(), std::string_view("identity"));
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
                HttpResponse response({.resource = &memory});
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
                        response.replaceBodyWithContentEncoding(std::move(encoded), coding);
                    } else {
                        response.applyContentEncoding(coding);
                    }
                    succeeded = true;
                } catch (const std::bad_alloc&) {
                    ++failures;
                    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view(identity));
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
                        RUVIA_CHECK_EQ(responseBody(response).bytes().size(), std::size_t{256});
                    } else {
                        RUVIA_CHECK(!response.header("Content-Length"));
                        RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view(identity));
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
    auto response = makeResponse();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
    replaceResponseBodyWithContentEncoding(response, std::move(encoded), "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("compressed"));
}
