#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_date.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"
#include "ruvia/web/static_files.h"

#include "context_request_fixture.h"
#include "test_harness.h"

namespace static_file_test {

[[nodiscard]] std::string read_file_response_body(const ruvia::http_response_file_view& file) {
    std::ifstream input(file.to_path(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("file response path could not be opened");
    }
    input.seekg(static_cast<std::streamoff>(file.offset()));
    std::string body;
    body.resize(static_cast<std::size_t>(file.length()));
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (static_cast<std::size_t>(input.gcount()) != body.size()) {
        throw std::runtime_error("file response path did not contain the selected slice");
    }
    return body;
}

class static_file_test_request final {
public:
    void set_method(std::string_view method) {
        method_ = method;
    }

    void set_target(std::string_view target) {
        target_.assign(target);
    }

    void add_header(ruvia::http_header_view header) {
        headers_.emplace_back(header.name(), header.value());
    }

    [[nodiscard]] ruvia::test_request request() const {
        auto request_value = ruvia::test_request::method(method_, target_);
        for (const auto& [name, value] : headers_) {
            request_value.header(name, value);
        }
        return request_value;
    }

private:
    std::string method_{"GET"};
    std::string target_{"/"};
    std::vector<std::pair<std::string, std::string>> headers_;
};

template <typename callback_type>
decltype(auto) with_static_context(const static_file_test_request& request, callback_type&& callback) {
    using result_type = std::invoke_result_t<callback_type, ruvia::context&>;
    std::optional<std::conditional_t<std::is_void_v<result_type>, bool, result_type>> result;
    std::exception_ptr error;
    static_cast<void>(context_request_test::with_context(
        request.request(), [&](ruvia::context& context_value) -> ruvia::task<void> {
            try {
                if constexpr (std::is_void_v<result_type>) {
                    callback(context_value);
                    result.emplace(true);
                } else {
                    result.emplace(callback(context_value));
                }
            } catch (...) {
                error = std::current_exception();
            }
            context_value.respond(context_value.text(""));
            co_return;
        }));
    if (error) {
        std::rethrow_exception(error);
    }
    if (!result) {
        throw std::runtime_error("static file request callback was not invoked");
    }
    if constexpr (!std::is_void_v<result_type>) {
        return result_type(std::move(*result));
    }
}

[[nodiscard]] ruvia::http_status_code static_file_status(
    const ruvia::static_root& root, std::string_view path) {
    static_file_test_request request;
    return with_static_context(request, [&](ruvia::context& ctx) {
        try {
            return ctx.static_file(root, {.relative_path_ = path}).status();
        } catch (const ruvia::http_error& error) {
            return error.info().status();
        }
    });
}

}  // namespace static_file_test

using static_file_test::read_file_response_body;
using static_file_test::static_file_status;
using static_file_test::static_file_test_request;
using static_file_test::with_static_context;

RUVIA_TEST(static_root_copies_public_mime_configuration_into_owned_storage) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_mime_resource_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.custom-resource") << "content";

    {
        ruvia::static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        options.mime_types_.push_back(ruvia::static_mime_type{
            .extension_ = ".custom-resource",
            .content_type_ = "application/x-custom-resource-type",
        });
        ruvia::static_root root(dir, options);
        options.mime_types_.front().content_type_ = "application/x-changed";
        static_file_test_request request;
        with_static_context(request, [&](ruvia::context& ctx) {
            const auto response = ctx.static_file(root, {.relative_path_ = "payload.custom-resource"});
            RUVIA_CHECK_EQ(response.header("Content-Type"),
                std::string_view("application/x-custom-resource-type"));
        });
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_serves_utf8_file_and_directory_urls) {
    namespace fs = std::filesystem;
    const auto temp_root = fs::canonical(fs::temp_directory_path());
    const auto dir = temp_root /
                     ("ruvia_static_utf8_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(dir)) {
        throw std::runtime_error("temporary static root already exists");
    }
    struct directory_cleanup final {
        fs::path parent_;
        fs::path path_;
        ruvia::testing::test_context& test_context_;
        ~directory_cleanup() {
            std::error_code error;
            const auto resolved = fs::weakly_canonical(path_, error);
            const bool owned = !error && resolved.parent_path() == parent_;
            ruvia::testing::report_check(test_context_, !owned, __FILE__, __LINE__, "owned temporary directory");
            if (owned) {
                fs::remove_all(path_, error);
                ruvia::testing::report_check(test_context_, static_cast<bool>(error), __FILE__, __LINE__, "temporary directory removed");
            }
        }
    } cleanup{temp_root, dir, ruvia_ctx};

    const auto filename = fs::path(u8"\u6587\u6863-\U0001F4C4.txt");
    const auto directory = fs::path(u8"\u76EE\u5F55-\U0001F4C1");
    fs::create_directory(dir / directory);
    for (const auto& [path, body] : std::array{
             std::pair{dir / filename, std::string_view("unicode-file-body")},
             std::pair{dir / directory / "index.html", std::string_view("unicode-index-body")}}) {
        std::ofstream output(path, std::ios::binary);
        output << body;
        if (!output) {
            throw std::runtime_error("failed to write Unicode static file");
        }
    }

    ruvia::static_root root(dir, {.index_file_ = "index.html"});
    struct case_value final {
        std::string_view relative_url_;
        std::string_view body_;
        std::string_view content_type_;
    };
    const case_value cases[]{
        {"%E6%96%87%E6%A1%A3-%F0%9F%93%84.txt", "unicode-file-body", "text/plain; charset=utf-8"},
        {"%E7%9B%AE%E5%BD%95-%F0%9F%93%81/", "unicode-index-body", "text/html; charset=utf-8"},
    };
    for (const auto& item : cases) {
        static_file_test_request request;
        const std::string target = "/" + std::string(item.relative_url_);
        request.set_target(target);
        with_static_context(request, [&](ruvia::context& context_value) {
            const auto response = context_value.static_file(root, {.relative_path_ = item.relative_url_});
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
            RUVIA_CHECK_EQ(response.header("Content-Type"), std::optional<std::string_view>{item.content_type_});
            const auto file = response.file_body();
            RUVIA_CHECK(file.has_value());
            if (file) {
                RUVIA_CHECK_EQ(file->length(), static_cast<std::uint64_t>(item.body_.size()));
                RUVIA_CHECK_EQ(read_file_response_body(*file), item.body_);
            }
        });
    }
}

RUVIA_TEST(file_response_preserves_normal_status_before_evaluating_request_conditions) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("ruvia_file_normal_status_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const auto path = dir / "payload.txt";
    {
        std::ofstream output(path, std::ios::binary);
        output << "0123456789";
    }
    struct case_value final {
        ruvia::http_status_code status_;
        ruvia::http_header_view condition_;
    };
    const case_value cases[]{
        {ruvia::http_status::not_found, {"If-None-Match", "*"}},
        {ruvia::http_status::temporary_redirect, {"If-Match", "\"different\""}},
        {ruvia::http_status::created, {"Range", "bytes=0-1"}},
    };
    ruvia::static_root root(dir,
        {.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all}});
    for (const auto& [indexed, item] : std::array{
             std::pair{false, cases[0]}, std::pair{true, cases[0]},
             std::pair{false, cases[1]}, std::pair{true, cases[1]},
             std::pair{false, cases[2]}, std::pair{true, cases[2]}}) {
        static_file_test_request request;
        request.add_header(item.condition_);
        with_static_context(request, [&](ruvia::context& context_value) {
            context_value.status(item.status_);
            context_value.header("X-Context", "preserved");
            std::optional<ruvia::http_response> response;
            try {
                response.emplace(indexed ? context_value.static_file(root, {.relative_path_ = "payload.txt"})
                                         : context_value.file({.path_ = path}));
            } catch (const ruvia::http_error&) {
            }
            RUVIA_CHECK(response.has_value());
            if (response) {
                RUVIA_CHECK_EQ(response->status(), item.status_);
                RUVIA_CHECK_EQ(response->header("X-Context"), std::optional<std::string_view>{"preserved"});
                RUVIA_CHECK(!response->header("Content-Range").has_value());
                const auto file = response->file_body();
                RUVIA_CHECK(file.has_value());
                if (file) {
                    RUVIA_CHECK_EQ(file->offset(), std::uint64_t{0});
                    RUVIA_CHECK_EQ(file->length(), std::uint64_t{10});
                }
            }
        });
    }
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_response_owns_path_after_handler_local_root_is_destroyed) {
    namespace fs = std::filesystem;

    const auto dir = fs::temp_directory_path() / "ruvia_static_local_root";
    fs::create_directories(dir);
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << "owned-static-path";
    }

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) {
        auto response = [&] {
            ruvia::static_root_options options;
            options.file_types_ =
                ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
            ruvia::static_root handler_local_root(dir, std::move(options));
            return context_value.static_file(
                handler_local_root, {.relative_path_ = "payload.txt", .content_type_ = "text/plain"});
        }();

        const auto file = response.file_body();
        RUVIA_CHECK(file.has_value());
        if (file.has_value()) {
            RUVIA_CHECK_EQ(read_file_response_body(*file), std::string("owned-static-path"));
        }

        fs::remove_all(dir);
    });
}

RUVIA_TEST(static_file_type_policy_has_closed_exact_alternatives) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_file_type_policy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "html";
    std::ofstream(dir / "asset.custom") << "custom";

    bool empty_only_threw = false;
    try {
        ruvia::static_root rejected_root(dir,
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only}});
    } catch (const std::invalid_argument&) {
        empty_only_threw = true;
    }
    RUVIA_CHECK(empty_only_threw);

    for (const std::string_view invalid : {"", ".", "..", "a/b", "a\\b"}) {
        bool invalid_type_threw = false;
        try {
            ruvia::static_root rejected_root(dir,
                {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only,
                     .extensions_ = {std::string(invalid)}}});
        } catch (const std::invalid_argument&) {
            invalid_type_threw = true;
        }
        RUVIA_CHECK(invalid_type_threw);
    }

    ruvia::static_root default_root(dir);
    RUVIA_CHECK(static_file_status(default_root, "index.html") == ruvia::http_status::ok);
    RUVIA_CHECK(static_file_status(default_root, "asset.custom") != ruvia::http_status::ok);

    ruvia::static_root_options all_options;
    all_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root all_root(dir, std::move(all_options));
    RUVIA_CHECK(static_file_status(all_root, "index.html") == ruvia::http_status::ok);
    RUVIA_CHECK(static_file_status(all_root, "asset.custom") == ruvia::http_status::ok);

    ruvia::static_root_options only_options;
    only_options.file_types_ = ruvia::static_file_type_policy{
        .kind_ = ruvia::static_file_type_policy::kind_type::only, .extensions_ = {".CUSTOM"}};
    ruvia::static_root only_root(dir, std::move(only_options));
    RUVIA_CHECK(static_file_status(only_root, "index.html") != ruvia::http_status::ok);
    RUVIA_CHECK(static_file_status(only_root, "asset.custom") == ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_range_serving_status_and_content_range) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_range_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "empty.txt", std::ios::binary | std::ios::trunc);
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    const auto serve_method = [&root](std::string_view method, std::string_view path,
                                  std::string_view range) {
        static_file_test_request request;
        request.set_method(method);
        if (!range.empty()) {
            request.add_header(http_header_view{"Range", range});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            const auto response =
                context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"});
            // Copy out before the request arena unwinds.
            return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Content-Range").value_or("")));
        });
    };
    const auto serve_file = [&serve_method](std::string_view path, std::string_view range) {
        return serve_method("GET", path, range);
    };
    const auto serve = [&serve_file](
                           std::string_view range) { return serve_file("data.txt", range); };

    // A valid single range -> 206 with the byte range echoed.
    const auto ok = serve("bytes=0-4");
    RUVIA_CHECK_EQ(ok.first, ruvia::http_status::partial_content);
    RUVIA_CHECK_EQ(ok.second, std::string("bytes 0-4/100"));

    // Multiple ranges produce a multipart representation without a top-level Content-Range.
    const auto multi = serve("bytes=0-9,20-29");
    RUVIA_CHECK_EQ(multi.first, ruvia::http_status::partial_content);
    RUVIA_CHECK(multi.second.empty());

    // A wholly unsatisfiable range -> 416 with "bytes */size".
    const auto bad = serve("bytes=1000-2000");
    RUVIA_CHECK_EQ(bad.first, ruvia::http_status::range_not_satisfiable);
    RUVIA_CHECK_EQ(bad.second, std::string("bytes */100"));

    // An unknown range unit MUST be ignored (RFC 9110 §14.2) -> full 200, not 416.
    const auto unknown_unit = serve("items=0-9");
    RUVIA_CHECK_EQ(unknown_unit.first, ruvia::http_status::ok);

    // A syntactically malformed byte range is likewise ignored -> full 200.
    const auto malformed = serve("bytes=abc");
    RUVIA_CHECK_EQ(malformed.first, ruvia::http_status::ok);

    // Range units are case-insensitive (RFC 9110 §14.1); this is still a 206.
    const auto case_insensitive_unit = serve("Bytes=5-9");
    RUVIA_CHECK_EQ(case_insensitive_unit.first, ruvia::http_status::partial_content);
    RUVIA_CHECK_EQ(case_insensitive_unit.second, std::string("bytes 5-9/100"));

    // RFC 9110 §14.2 defines Range handling only for GET. A HEAD request
    // carrying the same field must describe the full representation with 200,
    // not invent a partial 206 response with Content-Range metadata.
    const auto head = serve_method("HEAD", "data.txt", "bytes=0-4");
    RUVIA_CHECK_EQ(head.first, ruvia::http_status::ok);
    RUVIA_CHECK(head.second.empty());

    // This server uses RFC 9110 §14.2's permitted ignore policy for a selected
    // representation with no content, avoiding an invalid zero-length 206 range.
    const auto empty = serve_file("empty.txt", "bytes=0-0");
    RUVIA_CHECK_EQ(empty.first, ruvia::http_status::ok);
    RUVIA_CHECK(empty.second.empty());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_resolves_percent_encoded_name_and_stays_traversal_safe) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_pct_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "my report.txt", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view path) {
        static_file_test_request request;
        request.set_method("GET");
        return with_static_context(request, [&](ruvia::context& context_value) {
            ruvia::http_status_code status = ruvia::http_status::internal_server_error;
            try {
                status = context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"})
                             .status();
            } catch (const ruvia::http_error& error) {
                status = error.info().status();
            }
            return status;
        });
    };

    // "%20" resolves to the space in the real on-disk name (RFC 3986 percent
    // equivalence). Before decoding this 404'd: the raw bytes "my%20report.txt"
    // were compared against the decoded index key "my report.txt".
    RUVIA_CHECK_EQ(serve("my%20report.txt"), ruvia::http_status::ok);

    // Decoding must not open a traversal hole: "%2e%2e%2f" -> "../" is still
    // clamped at the root (403), and encoded separators plus dot-segments cannot
    // ascend past it either.
    RUVIA_CHECK_EQ(serve("%2e%2e%2fetc%2fpasswd"), ruvia::http_status::forbidden);
    RUVIA_CHECK_EQ(serve("sub%2f%2e%2e%2f%2e%2e%2fetc"), ruvia::http_status::forbidden);

    // A decoded NUL ("%00") cannot occur in a filename and is rejected outright.
    RUVIA_CHECK_EQ(serve("my%00report.txt"), ruvia::http_status::forbidden);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_declares_vary_accept_encoding_but_context_file_does_not) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_vary_dir";
    fs::create_directories(dir);
    const auto file_path = dir / "app.js";
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        const std::string content(50, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) {
        // No sidecar and no Accept-Encoding -> the identity file is served, but it must
        // STILL declare Vary: Accept-Encoding: the same URL would serve a compressed
        // variant to a capable client, so a shared cache keyed only on the URL must not
        // reuse this identity body for everyone (RFC 9110 12.5.5 / RFC 9111 4.1). The
        // identity body carries no Content-Encoding.
        const auto served =
            context_value.static_file(root, {.relative_path_ = "app.js", .content_type_ = "text/javascript"});
        RUVIA_CHECK_EQ(served.status(), ruvia::http_status::ok);
        RUVIA_CHECK(
            (served.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos));
        RUVIA_CHECK(!served.header("Content-Encoding").has_value());

        // context::file serves a single path with no encoding negotiation, so it must
        // NOT declare Vary: Accept-Encoding (which would needlessly fragment caches).
        const auto direct = context_value.file({.path_ = file_path});
        RUVIA_CHECK_EQ(direct.status(), ruvia::http_status::ok);
        RUVIA_CHECK(!direct.header("Vary").has_value());
        RUVIA_CHECK_EQ(direct.header("Content-Type").value_or(""),
            std::string_view("text/javascript; charset=utf-8"));
        RUVIA_CHECK(direct.file_body().has_value());

        fs::remove_all(dir);
    });
}

RUVIA_TEST(static_file_preserves_context_vary_when_adding_accept_encoding) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_vary_merge_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "app.js", std::ios::binary | std::ios::trunc);
        out << "console.log('ok');";
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    static_file_test_request request;
    request.set_method("GET");
    with_static_context(request, [&](ruvia::context& context_value) {
        context_value.header("Vary", "Origin");

        const auto response =
            context_value.static_file(root, {.relative_path_ = "app.js", .content_type_ = "text/javascript"});
        const auto vary = response.header("Vary").value_or("");
        // context response metadata is applied after the file's base headers. It
        // must not erase the negotiation dimension, or a shared cache can reuse an
        // identity/encoded representation for the wrong Accept-Encoding request.
        RUVIA_CHECK(vary.find("Origin") != std::string_view::npos);
        RUVIA_CHECK(vary.find("Accept-Encoding") != std::string_view::npos);

        fs::remove_all(dir);
    });
}

RUVIA_TEST(static_file_if_range_dates_do_not_authorize_partial_responses) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_if_range_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    // Serve with a Range plus an optional If-Range; returns (status, Last-Modified).
    const auto serve = [&root](std::optional<std::string_view> if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        return with_static_context(request, [&](ruvia::context& ctx) {
            const auto response =
                ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve(std::nullopt);
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::partial_content);
    RUVIA_CHECK(!base.second.empty());
    const auto last_modified = ruvia::parse_http_date(base.second);
    RUVIA_CHECK(last_modified.has_value());
    // Matching whole seconds do not establish a strong file validator.
    RUVIA_CHECK_EQ(serve(base.second).first, ruvia::http_status::ok);

    // A present empty If-Range is not an entity-tag or HTTP-date. Its condition
    // is therefore false, so it must suppress the Range rather than being
    // confused with an absent field and producing a partial response.
    RUVIA_CHECK_EQ(serve(std::string_view{}).first, ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(file_if_range_same_second_updates_require_an_etag) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    const auto dir = fs::temp_directory_path() / "ruvia_if_range_same_second";
    fs::create_directories(dir);
    const auto path = dir / "data.txt";
    const auto base_time = floor<seconds>(fs::file_time_type::clock::now()) - hours(24);
    const auto write_version = [&](std::string_view content, milliseconds fraction) {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        fs::last_write_time(path, base_time + fraction);
    };
    struct observation {
        ruvia::http_status_code status{ruvia::http_status::internal_server_error};
        std::string last_modified;
        std::string etag;
        std::uint64_t offset;
        std::uint64_t length;
    };
    for (const bool use_static : {false, true}) {
        std::optional<ruvia::static_root> root;
        const auto serve = [&](bool range, std::optional<std::string_view> if_range) {
            auto request = ruvia::test_request::get("/data.txt");
            if (range) {
                request.header("Range", "bytes=4-8");
            }
            if (if_range) {
                request.header("If-Range", *if_range);
            }
            observation result{};
            (void)context_request_test::with_context(request, [&](ruvia::context& context) -> ruvia::task<void> {
                const auto response = use_static
                                          ? context.static_file(*root, {.relative_path_ = "data.txt"})
                                          : context.file({.path_ = path});
                const auto file = response.file_body();
                result = observation{
                    response.status(),
                    std::string(response.header("Last-Modified").value_or("")),
                    std::string(response.header("ETag").value_or("")),
                    file ? file->offset() : 0,
                    file ? file->length() : 0,
                };
                co_return;
            });
            return result;
        };
        write_version("abcdefghij", milliseconds(100));
        if (use_static) {
            root.emplace(dir);
        }
        const auto before = serve(false, std::nullopt);
        root.reset();
        write_version("0123456789", milliseconds(500));
        if (use_static) {
            root.emplace(dir);
        }
        const auto after = serve(false, std::nullopt);
        RUVIA_CHECK_EQ(before.status, ruvia::http_status::ok);
        RUVIA_CHECK_EQ(after.status, ruvia::http_status::ok);
        RUVIA_CHECK_EQ(before.last_modified, after.last_modified);
        RUVIA_CHECK(before.etag != after.etag);
        const auto date_range = serve(true, before.last_modified);
        RUVIA_CHECK_EQ(date_range.status, ruvia::http_status::ok);
        RUVIA_CHECK_EQ(date_range.offset, std::uint64_t{0});
        RUVIA_CHECK_EQ(date_range.length, std::uint64_t{10});
        RUVIA_CHECK_EQ(serve(true, before.etag).status, ruvia::http_status::ok);
        const auto etag_range = serve(true, after.etag);
        RUVIA_CHECK_EQ(etag_range.status, ruvia::http_status::partial_content);
        RUVIA_CHECK_EQ(etag_range.offset, std::uint64_t{4});
        RUVIA_CHECK_EQ(etag_range.length, std::uint64_t{5});
    }
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_historical_last_modified_supports_date_preconditions) {
    namespace fs = std::filesystem;
    using namespace std::chrono;

    const auto dir = fs::temp_directory_path() / "ruvia_static_historical_mtime_dir";
    fs::create_directories(dir);
    const auto path = dir / "data.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "historical";
    }
    // Relate the two native clocks without depending on vendor-specific
    // file_clock::from_sys/from_utc APIs. Sub-second sampling is immaterial.
    const auto offset = seconds{-315619200} - duration_cast<seconds>(system_clock::now().time_since_epoch());
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() + offset, ec);
    RUVIA_CHECK(!ec);
    ruvia::static_root_options options;
    options.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    const auto serve = [&](bool snapshot, std::optional<std::string_view> since) {
        static_file_test_request request;
        request.set_method("GET");
        if (since) {
            request.add_header(ruvia::http_header_view{"If-Modified-Since", *since});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            const auto response = snapshot
                                      ? context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
                                      : context_value.file({.path_ = path, .content_type_ = "text/plain"});
            return std::pair(response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };
    for (const auto snapshot : {false, true}) {
        const auto base = serve(snapshot, std::nullopt);
        RUVIA_CHECK_EQ(base.first, ruvia::http_status::ok);
        const auto date = ruvia::parse_http_date(base.second);
        RUVIA_CHECK(date.has_value());
        if (date) {
            RUVIA_CHECK(*date >= -315619260 && *date <= -315619140);
        }
        RUVIA_CHECK_EQ(serve(snapshot, base.second).first, ruvia::http_status::not_modified);
        RUVIA_CHECK_EQ(serve(snapshot, "Thu, 01 Jan 1959 00:00:00 GMT").first, ruvia::http_status::ok);
    }
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_clamps_future_last_modified_and_rejects_it_for_if_range) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_future_mtime_dir";
    fs::create_directories(dir);
    const auto path = dir / "data.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "future";
    }
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::hours(24), ec);
    RUVIA_CHECK(!ec);

    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::optional<std::string_view> if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-1"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            const auto response =
                context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("Last-Modified").value_or("")));
        });
    };

    const auto before = std::time(nullptr);
    const auto base = serve(std::nullopt);
    const auto after = std::time(nullptr);
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::partial_content);
    const auto last_modified = ruvia::parse_http_date(base.second);
    RUVIA_CHECK(last_modified.has_value());
    RUVIA_CHECK(last_modified.value_or(after + 1) >= before);
    RUVIA_CHECK(last_modified.value_or(after + 1) <= after);

    // The clamped wire date is the response time, not the file's actual
    // validator. It is therefore weak and cannot authorize stitching a partial
    // response into the client's stored representation.
    RUVIA_CHECK_EQ(serve(base.second).first, ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_without_response_validators_still_enforces_preconditions) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_range_request_policy;
    using ruvia::static_response_validator_policy;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ifrange_novalidator_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    options.range_requests_ = static_range_request_policy::honor;
    options.response_validators_ =
        static_response_validator_policy::omit;  // no ETag / Last-Modified on responses
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view if_range) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (!if_range.empty()) {
            request.add_header(http_header_view{"If-Range", if_range});
        }
        return with_static_context(request, [&](ruvia::context& ctx) {
            return ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
                .status();
        });
    };

    // A plain range with no If-Range is still honored without validators -> 206.
    RUVIA_CHECK_EQ(serve(""), ruvia::http_status::partial_content);
    // A range WITH If-Range but no server validator cannot be confirmed, so the
    // Range MUST be ignored and the full representation served (RFC 9110 13.1.5) --
    // not a 206 stitched from bytes the client cannot verify it still holds.
    // (Gating the If-Range check on response validator emission skipped it and returned 206.)
    RUVIA_CHECK_EQ(serve("\"stale-etag\""), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(serve("Wed, 21 Oct 2015 07:28:00 GMT"), ruvia::http_status::ok);

    struct conditional_result final {
        ruvia::http_status_code status_;
        bool has_etag_;
        bool has_last_modified_;
    };
    const auto serve_conditional = [&root](std::string_view method,
                                       std::string_view name,
                                       std::string_view value) {
        static_file_test_request request;
        request.set_method(method);
        if (!name.empty()) {
            request.add_header(http_header_view{name, value});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            try {
                const auto response =
                    context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
                return conditional_result{response.status(), response.header("ETag").has_value(),
                    response.header("Last-Modified").has_value()};
            } catch (const ruvia::http_error& error) {
                return conditional_result{error.info().status(), false, false};
            }
        });
    };

    const auto plain = serve_conditional("GET", {}, {});
    RUVIA_CHECK_EQ(plain.status_, ruvia::http_status::ok);
    RUVIA_CHECK(!plain.has_etag_);
    RUVIA_CHECK(!plain.has_last_modified_);

    // Disabling response validator fields does not disable request
    // preconditions. Wildcard conditions test whether a current representation
    // exists and therefore require no ETag at all; date conditions use the
    // origin's file metadata even when Last-Modified is not emitted.
    const auto get_existing =
        serve_conditional("GET", "If-None-Match", "*");
    RUVIA_CHECK_EQ(get_existing.status_, ruvia::http_status::not_modified);
    RUVIA_CHECK(!get_existing.has_etag_);
    RUVIA_CHECK(!get_existing.has_last_modified_);
    RUVIA_CHECK_EQ(
        serve_conditional("POST", "If-None-Match", "*").status_,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(serve_conditional("POST", "If-Match", "*").status_,
        ruvia::http_status::ok);
    RUVIA_CHECK_EQ(
        serve_conditional("POST", "If-Match", "\"stale\"").status_,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(serve_conditional("POST", "If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT")
                       .status_,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(serve_conditional("GET", "If-Modified-Since", "Fri, 31 Dec 9999 23:59:59 GMT")
                       .status_,
        ruvia::http_status::not_modified);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_match_takes_precedence_over_if_unmodified_since) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_precedence_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    struct header {
        std::string_view name_;
        std::string_view value_;
    };
    const auto serve_method = [&root](
                                  std::string_view method, std::initializer_list<header> headers) {
        static_file_test_request request;
        request.set_method(method);
        for (const auto& header : headers) {
            request.add_header(http_header_view{header.name_, header.value_});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            ruvia::http_status_code status = ruvia::http_status::internal_server_error;
            std::string etag;
            try {
                const auto response =
                    context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
                status = response.status();
                etag.assign(response.header("ETag").value_or(""));
            } catch (const ruvia::http_error& error) {
                status = error.info().status();
            }
            return std::pair<ruvia::http_status_code, std::string>(status, std::move(etag));
        });
    };
    const auto serve = [&serve_method](std::initializer_list<header> headers) {
        return serve_method("GET", headers);
    };

    // Discover the current strong ETag with a bare request.
    const auto base = serve({});
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::ok);
    const std::string etag = base.second;
    RUVIA_CHECK(!etag.empty());
    // A date well before the file's mtime -> If-Unmodified-Since fails on its own.
    constexpr std::string_view old_date = "Thu, 01 Jan 1970 00:00:00 GMT";

    // If-Unmodified-Since alone (stale date) is a 412 precondition failure.
    RUVIA_CHECK_EQ(
        serve({{"If-Unmodified-Since", old_date}}).first,
        ruvia::http_status::precondition_failed);

    // With a matching If-Match present, RFC 9110 §13.2.2 requires If-Unmodified-Since
    // to be ignored -- the strong validator matched, so serve 200 rather than 412.
    RUVIA_CHECK_EQ(
        serve({{"If-Match", etag},
                  {"If-Unmodified-Since", old_date}})
            .first,
        ruvia::http_status::ok);

    // Presence is distinct from a non-empty field value. The empty #entity-tag
    // list matches no current representation, so an empty If-Match fails rather
    // than being treated as if the field were absent.
    RUVIA_CHECK_EQ(serve({{"If-Match", ""}}).first,
        ruvia::http_status::precondition_failed);

    constexpr std::string_view future_date = "Fri, 31 Dec 9999 23:59:59 GMT";
    RUVIA_CHECK_EQ(
        serve({{"If-Modified-Since", future_date}}).first,
        ruvia::http_status::not_modified);
    // Even an empty If-None-Match is present and therefore takes precedence over
    // If-Modified-Since. Its empty list does not match, so the response is 200.
    RUVIA_CHECK_EQ(
        serve({{"If-None-Match", ""},
                  {"If-Modified-Since", future_date}})
            .first,
        ruvia::http_status::ok);

    // If-Match and If-None-Match are list fields. Repeated field lines are
    // equivalent to one comma-joined value, so a match on the first line must
    // not be lost when the known-header cache records the second line.
    RUVIA_CHECK_EQ(serve({{"If-Match", etag},
                             {"If-Match", "\"stale\""}})
                       .first,
        ruvia::http_status::ok);
    RUVIA_CHECK_EQ(serve({{"If-None-Match", etag},
                             {"If-None-Match", "\"stale\""}})
                       .first,
        ruvia::http_status::not_modified);

    // Preconditions protect unsafe methods too. A matching If-None-Match or a
    // stale If-Match / If-Unmodified-Since on POST must fail with 412 instead of
    // serving the file as an unconditional 200.
    RUVIA_CHECK_EQ(
        serve_method("POST", {{"If-None-Match", etag}}).first,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(
        serve_method("POST", {{"If-Match", "\"stale\""}}).first,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(serve_method("POST",
                       {{"If-Unmodified-Since", old_date}})
                       .first,
        ruvia::http_status::precondition_failed);
    RUVIA_CHECK_EQ(serve_method("POST", {{"If-Match", etag}}).first,
        ruvia::http_status::ok);
    RUVIA_CHECK_EQ(
        serve_method("POST", {{"If-None-Match", "\"stale\""}})
            .first,
        ruvia::http_status::ok);
    RUVIA_CHECK_EQ(serve_method("POST",
                       {{"If-Modified-Since", future_date}})
                       .first,
        ruvia::http_status::ok);

    // OPTIONS does not select or modify a representation, so RFC 9110 §13.2.1
    // requires these conditional fields to be ignored for that method.
    RUVIA_CHECK_EQ(
        serve_method("OPTIONS", {{"If-Match", "\"stale\""}}).first,
        ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_conditional_request_serving) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_conditional_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view header_name,
                           std::string_view header_value) {
        static_file_test_request request;
        request.set_method("GET");
        if (!header_name.empty()) {
            request.add_header(http_header_view{header_name, header_value});
        }
        return with_static_context(request, [&](ruvia::context& context_value) {
            const auto response =
                context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            return std::pair<ruvia::http_status_code, std::string>(
                response.status(), std::string(response.header("ETag").value_or("")));
        });
    };

    // An unconditional GET yields 200 and a strong ETag validator.
    const auto plain = serve("", "");
    RUVIA_CHECK_EQ(plain.first, ruvia::http_status::ok);
    RUVIA_CHECK(!plain.second.empty());
    const std::string etag = plain.second;

    // If-None-Match with the current ETag -> 304; a stale one falls through to 200.
    RUVIA_CHECK_EQ(
        serve("If-None-Match", etag).first,
        ruvia::http_status::not_modified);
    RUVIA_CHECK_EQ(
        serve("If-None-Match", "\"stale\"").first,
        ruvia::http_status::ok);

    // A comma inside an opaque tag is data, not a list separator. This malformed
    // value closes that tag immediately before the current ETag and must not let
    // the apparent suffix satisfy the condition.
    const std::string malformed_list = std::string("\"stale, ") + etag;
    RUVIA_CHECK_EQ(
        serve("If-None-Match", malformed_list)
            .first,
        ruvia::http_status::ok);

    // If-Match against a non-matching ETag is a 412 precondition failure (thrown).
    bool precondition = false;
    try {
        (void)serve("If-Match", "\"stale\"");
    } catch (const ruvia::http_error& error) {
        precondition = error.info().status() == ruvia::http_status::precondition_failed;
    }
    RUVIA_CHECK(precondition);

    precondition = false;
    try {
        (void)serve("If-Match", malformed_list);
    } catch (const ruvia::http_error& error) {
        precondition = error.info().status() == ruvia::http_status::precondition_failed;
    }
    RUVIA_CHECK(precondition);

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_empty_custom_mime_type) {
    namespace fs = std::filesystem;
    using ruvia::static_mime_type;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_empty_mime_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.custom", std::ios::binary | std::ios::trunc);
        out << "content";
    }

    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_mime_type mime;
    mime.extension_ = ".custom";
    options.mime_types_.push_back(std::move(mime));

    bool rejected = false;
    try {
        static_root root(dir, std::move(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_invalid_static_options_at_construction) {
    namespace fs = std::filesystem;
    using ruvia::static_mime_type;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_invalid_header_options_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "data.custom") << "content";

    const auto rejects = [&dir, &ruvia_ctx](static_root_options options) {
        bool rejected = false;
        try {
            static_root root(dir, std::move(options));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    };

    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        options.cache_control_ = " private";
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        options.default_content_type_ = "text plain";
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        static_mime_type mime;
        mime.extension_ = ".custom";
        mime.content_type_ = "text plain";
        options.mime_types_.push_back(std::move(mime));
        rejects(std::move(options));
    }
    for (const std::string_view invalid_extension :
        {"", ".", "..", "nested/custom", "nested\\custom"}) {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        static_mime_type mime;
        mime.extension_ = std::string(invalid_extension);
        mime.content_type_ = "text/plain";
        options.mime_types_.push_back(std::move(mime));
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.mime_types_ = {
            {.extension_ = "CUSTOM", .content_type_ = "application/x-first"},
            {.extension_ = ".custom", .content_type_ = "application/x-second"},
        };
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        options.dotfiles_ = std::bit_cast<ruvia::static_dotfile_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.range_requests_ = std::bit_cast<ruvia::static_range_request_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }
    {
        static_root_options options;
        options.response_validators_ =
            std::bit_cast<ruvia::static_response_validator_policy>(std::uint8_t{42});
        rejects(std::move(options));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_modified_since_serving) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ims_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(50, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view if_modified_since) {
        static_file_test_request request;
        request.set_method("GET");
        request.add_header(http_header_view{"If-Modified-Since", if_modified_since});
        return with_static_context(request, [&](ruvia::context& context_value) {
            return context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
                .status();
        });
    };

    // The file was just written, so an If-Modified-Since far in the future means
    // "not modified since then" -> 304; one far in the past means it HAS changed
    // -> 200.
    RUVIA_CHECK_EQ(serve("Fri, 01 Jan 2100 00:00:00 GMT"), ruvia::http_status::not_modified);
    RUVIA_CHECK_EQ(serve("Sat, 01 Jan 2000 00:00:00 GMT"), ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_directory_root_index_and_403) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;

    const auto dir = fs::temp_directory_path() / "ruvia_static_dir_index";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "other.txt", std::ios::binary | std::ios::trunc);
        out << "x";
    }

    const auto serve_root = [](static_root& root) -> ruvia::http_status_code {
        static_file_test_request request;
        request.set_method("GET");
        return with_static_context(request, [&](ruvia::context& context_value) {
            try {
                return context_value.static_file(root, {.relative_path_ = "", .content_type_ = "text/html"})
                    .status();
            } catch (const ruvia::http_error& error) {
                return error.info().status();
            }
        });
    };

    // A directory root with no configured index is forbidden (never a listing).
    {
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        static_root root(dir, std::move(options));
        RUVIA_CHECK_EQ(serve_root(root), ruvia::http_status::forbidden);
    }

    // With an index file configured (and present), the directory root serves it.
    {
        std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
        out << "<h1>i</h1>";
        out.close();
        static_root_options options;
        options.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        options.index_file_ = "index.html";
        static_root root(dir, std::move(options));
        RUVIA_CHECK_EQ(serve_root(root), ruvia::http_status::ok);
    }

    fs::remove_all(dir);
}
