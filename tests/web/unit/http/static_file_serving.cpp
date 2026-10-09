#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/context.h"
#include "ruvia/web/error.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/static_files.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "http/static_file_metadata.h"
#include "http/static_root_config_storage.h"
#include "http/static_root_index.h"
#include "http/static_root_options_validation.h"
#include "router/route_table.h"
#include "router/router_impl.h"
#include "server/http_file_open.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class static_file_test_request final {
public:
    explicit static_file_test_request(std::pmr::memory_resource* resource)
        : resource_(resource) {}

    void set_method(std::string_view method) noexcept {
        method_ = method;
    }

    void set_target(std::string_view target) noexcept {
        target_ = target;
    }

    void set_path(std::string_view path) noexcept {
        target_ = path;
    }

    void add_header(ruvia::http_header_view header_value) {
        headers_.push_back(header_value);
    }

    operator ruvia::http_request&() {
        if (!request_.has_value()) {
            auto [request, error] = ruvia::make_parsed_http_request(
                method_, target_, headers_, {}, resource_);
            if (error.has_value()) {
                throw std::invalid_argument("invalid static-file test HTTP request");
            }
            request_.emplace(std::move(request));
        }
        return *request_;
    }

private:
    std::pmr::memory_resource* resource_;
    std::string_view method_ = "GET";
    std::string_view target_ = "/";
    std::vector<ruvia::http_header_view> headers_;
    std::optional<ruvia::http_request> request_;
};

using ruvia::http_known_method;
using ruvia::http_response;

template <typename result_type>
[[nodiscard]] result_type run_static_compression_task(asio::io_context& context_value, ruvia::task<result_type> task_value) {
    std::optional<result_type> result;
    std::exception_ptr exception;
    asio::co_spawn(
        context_value,
        [task_value = std::move(task_value), &result, &exception]() mutable -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::as_awaitable(std::move(task_value)));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    context_value.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("static compression task produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(static_root_config_storage_normalizes_and_owns_public_configuration) {
    std::pmr::monotonic_buffer_resource resource;
    std::optional<ruvia::detail::static_root_config_storage> stored;
    {
        ruvia::static_root_options options{
            .cache_control_ = "public, max-age=31536000, immutable, must-revalidate",
            .index_file_ = "a-deliberately-long-index-file-name-for-owned-storage.html",
            .default_content_type_ = "application/x-deliberately-long-default-content-type",
            .mime_types_ =
                {
                    {.extension_ = "CUSTOM-B", .content_type_ = "application/x-custom-b-long"},
                    {.extension_ = ".Custom-A", .content_type_ = "application/x-custom-a-long"},
                },
            .file_types_ =
                {
                    .kind_ = ruvia::static_file_type_policy::kind_type::only,
                    .extensions_ = {"CUSTOM-B", ".Custom-A", "custom-b"},
                },
            .range_requests_ = ruvia::static_range_request_policy::ignore,
            .response_validators_ = ruvia::static_response_validator_policy::omit,
            .dotfiles_ = ruvia::static_dotfile_policy::serve,
        };
        stored.emplace(ruvia::detail::make_static_root_config_storage(options, &resource));
    }

    RUVIA_CHECK(stored.has_value());
    RUVIA_CHECK(stored->cache_control_.get_allocator().resource() == &resource);
    RUVIA_CHECK(stored->index_file_.get_allocator().resource() == &resource);
    RUVIA_CHECK(stored->default_content_type_.get_allocator().resource() == &resource);
    RUVIA_CHECK(stored->mime_types_.get_allocator().resource() == &resource);
    RUVIA_CHECK(stored->file_type_extensions_.get_allocator().resource() == &resource);
    RUVIA_CHECK_EQ(stored->mime_types_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(stored->mime_types_[0].extension_, ".custom-a");
    RUVIA_CHECK_EQ(stored->mime_types_[1].extension_, ".custom-b");
    RUVIA_CHECK(stored->mime_types_[0].extension_.get_allocator().resource() == &resource);
    RUVIA_CHECK(stored->mime_types_[0].content_type_.get_allocator().resource() == &resource);
    RUVIA_CHECK_EQ(stored->file_type_extensions_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(stored->file_type_extensions_[0], "custom-a");
    RUVIA_CHECK_EQ(stored->file_type_extensions_[1], "custom-b");
    RUVIA_CHECK(stored->file_type_extensions_[0].get_allocator().resource() == &resource);

    std::pmr::monotonic_buffer_resource copy_resource;
    ruvia::detail::static_root_config_storage copy(*stored, &copy_resource);
    RUVIA_CHECK(copy.cache_control_.get_allocator().resource() == &copy_resource);
    RUVIA_CHECK(copy.mime_types_[0].extension_.get_allocator().resource() == &copy_resource);
    RUVIA_CHECK(copy.file_type_extensions_[0].get_allocator().resource() == &copy_resource);
    RUVIA_CHECK_EQ(copy.mime_types_[0].content_type_, "application/x-custom-a-long");
}

RUVIA_TEST(static_root_config_storage_validates_before_owner_pmr_allocation) {
    bool invalid = false;
    try {
        static_cast<void>(ruvia::detail::make_static_root_config_storage(
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only}},
            std::pmr::null_memory_resource()));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    RUVIA_CHECK(invalid);

    invalid = false;
    try {
        static_cast<void>(ruvia::detail::make_static_root_config_storage(
            {.mime_types_ =
                    {
                        {.extension_ = "CUSTOM", .content_type_ = "application/x-first"},
                        {.extension_ = ".custom", .content_type_ = "application/x-second"},
                    }},
            std::pmr::null_memory_resource()));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    RUVIA_CHECK(invalid);
}

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
        ruvia::static_root root(dir, std::move(options));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_permission_errors_in_index) {
#if defined(_WIN32)
    // Windows ACLs are not expressible through std::filesystem::perms in a
    // portable way, so this permission-specific case cannot run here.
    return;
#else
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_permission_error";
    const auto restricted = dir / "restricted";
    fs::remove_all(dir);
    fs::create_directories(restricted);
    std::ofstream(restricted / "payload.txt") << "content";

    std::error_code ec;
    fs::permissions(restricted, fs::perms::none, fs::perm_options::replace, ec);
    RUVIA_CHECK(!ec);
    if (ec) {
        fs::remove_all(dir);
        return;
    }

    // Tests may run as a privileged user. Only assert the contract when the
    // platform actually reports the permission failure to this process.
    std::error_code probe_ec;
    fs::directory_iterator probe(restricted, probe_ec);
    const fs::directory_iterator end;
    for (; !probe_ec && probe != end; probe.increment(probe_ec)) {
    }
    const bool permission_denied = static_cast<bool>(probe_ec);

    if (!permission_denied) {
        fs::permissions(restricted,
            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all,
            fs::perm_options::replace, ec);
        RUVIA_CHECK(!ec);
        fs::remove_all(dir);
        return;
    }

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    bool rejected = false;
    try {
        ruvia::static_root root(dir, std::move(options));
    } catch (const fs::filesystem_error&) {
        rejected = true;
    }
    fs::permissions(restricted, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all,
        fs::perm_options::replace, ec);
    RUVIA_CHECK(!ec);
    RUVIA_CHECK(rejected);
    fs::remove_all(dir);
#endif
}

// Serving a file: preconditions, ranges, precompressed variants, type policy
// and the traversal-safe path resolution behind them.

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        const std::string target = "/" + std::string(item.relative_url_);
        request.set_target(target);
        auto context_value = ruvia::detail::context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response = context_value.static_file(root, {.relative_path_ = item.relative_url_});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::optional<std::string_view>{item.content_type_});
        const auto file = response.file_body();
        RUVIA_CHECK(file.has_value());
        if (file) {
            RUVIA_CHECK_EQ(file->length(), static_cast<std::uint64_t>(item.body_.size()));
            auto input = ruvia::detail::open_response_file_input(*file);
            std::string body(item.body_.size(), '\0');
            input.read(body.data(), static_cast<std::streamsize>(body.size()));
            RUVIA_CHECK_EQ(input.gcount(), static_cast<std::streamsize>(body.size()));
            RUVIA_CHECK_EQ(body, item.body_);
        }
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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.add_header(item.condition_);
        auto context_value = ruvia::detail::context_access::make(
            memory, request, ruvia::test::test_context_services());
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
    }
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_response_owns_path_after_handler_local_root_is_destroyed) {
    namespace fs = std::filesystem;
    using ruvia::detail::context_access;
    using ruvia::detail::context_services;

    const auto dir = fs::temp_directory_path() / "ruvia_static_local_root";
    fs::create_directories(dir);
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << "owned-static-path";
    }

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());

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
        auto input = ruvia::detail::open_response_file_input(*file);
        std::string body(std::string("owned-static-path").size(), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(input.gcount(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(body, std::string("owned-static-path"));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(response_file_input_rejects_in_place_mutation_after_open) {
#if defined(__unix__) || defined(_WIN32)
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia_static_in_place_mutation.bin";
    fs::remove(path);
    constexpr std::string_view old_contents = "old-static-body";
    constexpr std::string_view new_contents = "new-static-body";

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << old_contents;
    }
    // Some filesystems expose a coarse change-time token; separate the two
    // writes so this test exercises the same identity signal the runtime uses.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::error_code error;
    const auto snapshot = ruvia::detail::snapshot_response_file(path.c_str(), error);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(snapshot.identity_.requires_validation());
    if (error || !snapshot.identity_.requires_validation()) {
        fs::remove(path);
        return;
    }

    const ruvia::http_response_file_view file(
        path.c_str(), snapshot.size_, 0, snapshot.size_, snapshot.identity_);
    auto input = ruvia::detail::open_response_file_input(file);
    RUVIA_CHECK(static_cast<bool>(input));
    if (!input) {
        fs::remove(path);
        return;
    }

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << new_contents;
    }
    const auto changed_snapshot = ruvia::detail::snapshot_response_file(path.c_str(), error);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(changed_snapshot.identity_ != snapshot.identity_);
    RUVIA_CHECK(!input.matches_snapshot(snapshot.identity_, snapshot.size_));
    fs::remove(path);
#endif
}

RUVIA_TEST(static_file_without_sidecar_stays_identity_when_precompressed_variants_are_enabled) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_no_runtime_compression";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.txt") << std::string(4096, 'c');

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    ruvia::worker_memory worker_memory;
    ruvia::request_memory request_memory(worker_memory);
    static_file_test_request request(request_memory.resource());
    request.set_method("GET");
    request.add_header(ruvia::http_header_view{"Accept-Encoding", "gzip"});

    auto context_value = ruvia::detail::context_access::make(
        request_memory, request, ruvia::test::test_context_services().with_precompressed_static_files());
    auto response =
        context_value.static_file(root, {.relative_path_ = "payload.txt", .content_type_ = "text/plain"});
    RUVIA_CHECK(response.file_body().has_value());
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());

    fs::remove_all(dir);
}

RUVIA_TEST(document_root_snapshot_metadata_tracks_refresh) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_refresh_metadata";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "<html></html>";

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    auto equivalent_root =
        ruvia::detail::static_root_access::clone(ruvia::detail::process_resource(), root);
    RUVIA_CHECK(ruvia::detail::static_root_access::same_snapshot(root, *equivalent_root));

    auto cloned_config =
        ruvia::detail::static_root_access::copy_config(root, ruvia::detail::process_resource());
    RUVIA_CHECK(cloned_config.file_type_kind_ == ruvia::static_file_type_policy::kind_type::all);

    std::ofstream(dir / "new-file.txt") << "published on the next refresh";
    auto refreshed =
        ruvia::detail::static_root_access::make(ruvia::detail::process_resource(), dir, cloned_config);
    RUVIA_CHECK(ruvia::detail::static_root_access::fingerprint(root) !=
                ruvia::detail::static_root_access::fingerprint(*refreshed));

    fs::remove_all(dir);
}

RUVIA_TEST(configured_document_root_binding_is_a_move_only_request_snapshot_lease) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_binding_lease";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.txt") << "payload";

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    request.set_target("/payload.txt");
    request.set_path("/payload.txt");

    ruvia::detail::route_table routes_value(memory.resource());
    const auto resolution = routes_value.resolve(request);
    auto binding = ruvia::detail::document_root_binding::configured(root);
    auto task_value = routes_value.dispatch_buffered_response(
        request, resolution, memory, std::move(binding), ruvia::test::test_context_services());
    RUVIA_CHECK(ruvia::detail::static_root_access::has_active_bindings(root));
    asio::io_context io;
    auto result_value = run_static_compression_task(io, std::move(task_value));
    RUVIA_CHECK_EQ(result_value.status(), ruvia::http_status::ok);
    RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(root));

    fs::remove_all(dir);
}

RUVIA_TEST(configured_document_root_binding_counts_belong_to_the_bound_snapshot) {
    namespace fs = std::filesystem;
    const auto first_dir = fs::temp_directory_path() / "ruvia_static_binding_first";
    const auto second_dir = fs::temp_directory_path() / "ruvia_static_binding_second";
    fs::remove_all(first_dir);
    fs::remove_all(second_dir);
    fs::create_directories(first_dir);
    fs::create_directories(second_dir);
    std::ofstream(first_dir / "payload.txt") << "first";
    std::ofstream(second_dir / "payload.txt") << "second";

    ruvia::static_root_options first_options;
    first_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root first(first_dir, std::move(first_options));
    ruvia::static_root_options second_options;
    second_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root second(second_dir, std::move(second_options));

    {
        auto first_binding = ruvia::detail::document_root_binding::configured(first);
        RUVIA_CHECK(ruvia::detail::static_root_access::has_active_bindings(first));
        RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(second));
        {
            auto second_binding = ruvia::detail::document_root_binding::configured(second);
            RUVIA_CHECK(ruvia::detail::static_root_access::has_active_bindings(first));
            RUVIA_CHECK(ruvia::detail::static_root_access::has_active_bindings(second));
        }
        RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(second));
        RUVIA_CHECK(ruvia::detail::static_root_access::has_active_bindings(first));
    }

    RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(first));
    RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(second));
    fs::remove_all(first_dir);
    fs::remove_all(second_dir);
}

RUVIA_TEST(standalone_static_root_bindings_do_not_share_request_lease_state) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_immutable_bindings";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.txt") << "payload";

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));
    {
        auto binding = ruvia::detail::document_root_binding::standalone(root);
        RUVIA_CHECK_EQ(binding.root(), &root);
        RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(root));
    }

    constexpr std::size_t worker_count = 4;
    constexpr std::size_t bindings_per_worker = 10'000;
    std::array<std::thread, worker_count> workers;
    for (auto& worker : workers) {
        worker = std::thread([&root]() {
            for (std::size_t index = 0; index < bindings_per_worker; ++index) {
                auto binding = ruvia::detail::document_root_binding::standalone(root);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    RUVIA_CHECK(!ruvia::detail::static_root_access::has_active_bindings(root));
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_replacement_cannot_reuse_indexed_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_replacement_identity";
    const auto served_path = dir / "payload.txt";
    const auto replacement_path = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(served_path, std::ios::binary);
        output << "old-representation";
    }

    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    // Use the same byte length so a size-only guard would accept and transmit
    // the replacement under the old ETag/Last-Modified framing.
    {
        std::ofstream output(replacement_path, std::ios::binary);
        output << "new-representation";
    }

#if defined(_WIN32)
    fs::remove(served_path);
#endif
    fs::rename(replacement_path, served_path);

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
    auto response =
        context_value.static_file(root, {.relative_path_ = "payload.txt", .content_type_ = "text/plain"});
    const std::string old_etag(response.header("ETag").value_or(""));
    const auto file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requires_validation());
        auto input = ruvia::detail::open_response_file_input(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
        char byte = '\0';
        input.read(&byte, 1);
        RUVIA_CHECK_EQ(input.gcount(), std::streamsize{0});
    }

    ruvia::static_root_options refreshed_options;
    refreshed_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root refreshed_root(dir, std::move(refreshed_options));
    auto refreshed = context_value.static_file(
        refreshed_root, {.relative_path_ = "payload.txt", .content_type_ = "text/plain"});
    RUVIA_CHECK(!old_etag.empty());
    RUVIA_CHECK(refreshed.header("ETag").value_or("") != old_etag);

    fs::remove_all(dir);
}

RUVIA_TEST(context_file_replacement_cannot_reuse_response_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_context_file_replacement_identity";
    const auto served_path = dir / "payload.txt";
    const auto replacement_path = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(served_path, std::ios::binary);
        output << "old-context-body";
    }

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
    auto response = context_value.file({.path_ = served_path, .content_type_ = "text/plain"});

    {
        std::ofstream output(replacement_path, std::ios::binary);
        output << "new-context-body";
    }
#if defined(_WIN32)
    fs::remove(served_path);
#endif
    fs::rename(replacement_path, served_path);

    const auto file = response.file_body();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requires_validation());
        auto input = ruvia::detail::open_response_file_input(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_type_policy_has_closed_exact_alternatives) {
    bool empty_only_threw = false;
    try {
        ruvia::detail::validate_static_root_options(
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only}});
    } catch (const std::invalid_argument&) {
        empty_only_threw = true;
    }
    RUVIA_CHECK(empty_only_threw);

    for (const std::string_view invalid : {"", ".", "..", "a/b", "a\\b"}) {
        bool invalid_type_threw = false;
        try {
            ruvia::detail::validate_static_root_options(
                {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only,
                     .extensions_ = {std::string(invalid)}}});
        } catch (const std::invalid_argument&) {
            invalid_type_threw = true;
        }
        RUVIA_CHECK(invalid_type_threw);
    }

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_file_type_policy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "html";
    std::ofstream(dir / "asset.custom") << "custom";

    ruvia::static_root default_root(dir);
    RUVIA_CHECK(ruvia::detail::static_root_access::find(default_root, "index.html").has_value());
    RUVIA_CHECK(!ruvia::detail::static_root_access::find(default_root, "asset.custom").has_value());

    ruvia::static_root_options all_options;
    all_options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root all_root(dir, std::move(all_options));
    RUVIA_CHECK(ruvia::detail::static_root_access::find(all_root, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::static_root_access::find(all_root, "asset.custom").has_value());

    ruvia::static_root_options only_options;
    only_options.file_types_ = ruvia::static_file_type_policy{
        .kind_ = ruvia::static_file_type_policy::kind_type::only, .extensions_ = {".CUSTOM"}};
    ruvia::static_root only_root(dir, std::move(only_options));
    RUVIA_CHECK(!ruvia::detail::static_root_access::find(only_root, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::static_root_access::find(only_root, "asset.custom").has_value());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_extension_preserves_unicode_without_ascii_aliasing) {
    std::pmr::monotonic_buffer_resource resource;
    const auto extension = ruvia::detail::lower_static_file_extension(
        std::filesystem::path(u8"asset.\u0168TML"), &resource);

    RUVIA_CHECK_EQ(std::string_view(extension),
        std::string_view(reinterpret_cast<const char*>(u8".\u0168tml")));
    RUVIA_CHECK(extension != ".html");
}

RUVIA_TEST(static_file_range_serving_status_and_content_range) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method(method);
        if (!range.empty()) {
            request.add_header(http_header_view{"Range", range});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response =
            context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"});
        // Copy out before the request arena unwinds.
        return std::pair<ruvia::http_status_code, std::string>(
            response.status(), std::string(response.header("Content-Range").value_or("")));
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

RUVIA_TEST(multipart_file_response_segments_frame_and_stream_the_selected_slices) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia_multipart_ranges.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "0123456789";
    }
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.add_header({"Range", "bytes=0-2,7-9"});
    auto context_value = ruvia::detail::context_access::make(
        memory, request, ruvia::test::test_context_services());
    context_value.header("Content-Type", "application/x-original");
    context_value.header("Content-Range", "bytes 0-1/10");
    context_value.header("Content-Encoding", "br");
    context_value.header("Content-Length", "1");
    auto response = context_value.file({.path_ = path, .content_type_ = "text/plain"});
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::partial_content);
    const auto content_type_value = response.header("Content-Type").value_or("");
    RUVIA_CHECK(content_type_value.starts_with("multipart/byteranges; boundary="));
    RUVIA_CHECK(!response.header("Content-Range").has_value());
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    constexpr std::string_view marker = "boundary=";
    const auto boundary_position = content_type_value.find(marker);
    RUVIA_CHECK(boundary_position != std::string_view::npos);
    const auto boundary = boundary_position == std::string_view::npos
                              ? std::string_view{}
                              : content_type_value.substr(boundary_position + marker.size());
    RUVIA_CHECK_EQ(boundary.size(), std::size_t{48});

    std::string body;
    for (std::size_t index = 0; index < response.body_segment_count(); ++index) {
        const auto segment = response.body_segment(index);
        if (!segment.file_) {
            body.append(segment.bytes_);
            continue;
        }
        auto input = ruvia::detail::open_response_file_input(*segment.file_);
        RUVIA_CHECK(static_cast<bool>(input));
        if (!input) {
            continue;
        }
        input.seekg(static_cast<std::streamoff>(segment.file_->offset()), std::ios::beg);
        std::string bytes_value(static_cast<std::size_t>(segment.file_->length()), '\0');
        input.read(bytes_value.data(), static_cast<std::streamsize>(bytes_value.size()));
        RUVIA_CHECK_EQ(input.gcount(), static_cast<std::streamsize>(bytes_value.size()));
        body.append(bytes_value);
    }
    const auto expected = "--" + std::string(boundary) +
                          "\r\nContent-Type: application/x-original\r\nContent-Range: bytes 0-2/10\r\n\r\n012\r\n--" +
                          std::string(boundary) +
                          "\r\nContent-Type: application/x-original\r\nContent-Range: bytes 7-9/10\r\n\r\n789\r\n--" +
                          std::string(boundary) + "--\r\n";
    RUVIA_CHECK_EQ(body, expected);
    const auto write_plan = ruvia::plan_buffered_http_response_write(http_known_method::get, response);
    RUVIA_CHECK_EQ(write_plan.content_length(), static_cast<std::uint64_t>(body.size()));
    RUVIA_CHECK(response.has_multipart_file_body());
    fs::remove(path);
}

RUVIA_TEST(static_file_resolves_percent_encoded_name_and_stays_traversal_safe) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        ruvia::http_status_code status = ruvia::http_status::internal_server_error;
        try {
            status = context_value.static_file(root, {.relative_path_ = path, .content_type_ = "text/plain"})
                         .status();
        } catch (const ruvia::http_error& error) {
            status = error.info().status();
        }
        return status;
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
    using ruvia::detail::context_access;

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

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());

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
}

RUVIA_TEST(static_file_preserves_context_vary_when_adding_accept_encoding) {
    namespace fs = std::filesystem;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

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

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
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
}

RUVIA_TEST(sse_stream_head_defaults_cache_control_but_honors_a_caller_value) {
    using ruvia::http_response_stream_framing;
    using ruvia::http_response_stream_kind;
    using ruvia::http_response_trailer_intent;
    using ruvia::prepare_http_response_stream_head;
    using ruvia::detail::context_access;

    const auto head = [](bool preset_no_cache) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        if (preset_no_cache) {
            context_access::set_response_header(context_value, "Cache-Control", "no-cache");
        }
        auto response = context_access::streaming_head(context_value);
        auto stream_head = prepare_http_response_stream_head(std::move(response), http_response_stream_kind::sse,
            ruvia::plan_http_response_stream_commit(http_response_stream_framing::http1_chunked,
                http_known_method::get, ruvia::http_status::ok, http_response_trailer_intent::none));
        return std::string(stream_head.response().header("Cache-Control").value_or(""));
    };

    // With no caller value, an SSE stream defaults to no-store so the event stream
    // is never cached.
    RUVIA_CHECK_EQ(head(false), std::string("no-store"));
    // A handler that set its own Cache-Control -- e.g. the recommended SSE
    // "no-cache" -- must have it preserved, not clobbered with no-store.
    RUVIA_CHECK_EQ(head(true), std::string("no-cache"));
}

RUVIA_TEST(static_file_if_range_date_requires_exact_match) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        auto ctx = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response =
            ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
        return std::pair<ruvia::http_status_code, std::string>(
            response.status(), std::string(response.header("Last-Modified").value_or("")));
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve(std::nullopt);
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::partial_content);
    RUVIA_CHECK(!base.second.empty());
    const auto last_modified = ruvia::parse_http_date(base.second);
    RUVIA_CHECK(last_modified.has_value());
    // Exact match -> the representation is unchanged, so the range is honored (206).
    RUVIA_CHECK_EQ(serve(base.second).first, ruvia::http_status::partial_content);

    // A present empty If-Range is not an entity-tag or HTTP-date. Its condition
    // is therefore false, so it must suppress the Range rather than being
    // confused with an absent field and producing a partial response.
    RUVIA_CHECK_EQ(serve(std::string_view{}).first, ruvia::http_status::ok);

    // If-Range date NEWER than Last-Modified: the file's mtime is older, so it is a
    // DIFFERENT representation than the client holds. RFC 9110 §13.1.5 requires an
    // exact match, so the range MUST be refused and the full 200 served. (The old
    // "<=" comparison wrongly returned 206 here -- the corruption path.)
    RUVIA_CHECK_EQ(serve("Fri, 31 Dec 9999 23:59:59 GMT").first, ruvia::http_status::ok);

    // If-Range date OLDER than Last-Modified: representation has since changed -> 200.
    RUVIA_CHECK_EQ(serve("Thu, 01 Jan 1970 00:00:00 GMT").first, ruvia::http_status::ok);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_historical_last_modified_supports_date_preconditions) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        if (since) {
            request.add_header(ruvia::http_header_view{"If-Modified-Since", *since});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response = snapshot
                                  ? context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
                                  : context_value.file({.path_ = path, .content_type_ = "text/plain"});
        return std::pair(response.status(), std::string(response.header("Last-Modified").value_or("")));
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
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-1"});
        if (if_range.has_value()) {
            request.add_header(http_header_view{"If-Range", *if_range});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response =
            context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
        return std::pair<ruvia::http_status_code, std::string>(
            response.status(), std::string(response.header("Last-Modified").value_or("")));
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
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        request.add_header(http_header_view{"Range", "bytes=0-4"});
        if (!if_range.empty()) {
            request.add_header(http_header_view{"If-Range", if_range});
        }
        auto ctx = context_access::make(memory, request, ruvia::test::test_context_services());
        return ctx.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
            .status();
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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method(method);
        if (!name.empty()) {
            request.add_header(http_header_view{name, value});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        try {
            const auto response =
                context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
            return conditional_result{response.status(), response.header("ETag").has_value(),
                response.header("Last-Modified").has_value()};
        } catch (const ruvia::http_error& error) {
            return conditional_result{error.info().status(), false, false};
        }
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
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method(method);
        for (const auto& header : headers) {
            request.add_header(http_header_view{header.name_, header.value_});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
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
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        if (!header_name.empty()) {
            request.add_header(http_header_view{header_name, header_value});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response =
            context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
        return std::pair<ruvia::http_status_code, std::string>(
            response.status(), std::string(response.header("ETag").value_or("")));
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

RUVIA_TEST(static_file_stale_index_rejects_conditional_response) {
#if defined(__unix__) || defined(_WIN32)
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_stale_conditional_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto path = dir / "data.txt";
    constexpr std::string_view old_contents = "old-static-data";
    constexpr std::string_view new_contents = "new-static-data";
    static_assert(old_contents.size() == new_contents.size());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << old_contents;
    }
    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    const auto serve = [&root](std::string_view method, std::string_view header_name = {},
                           std::string_view header_value = {}) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method(method);
        if (!header_name.empty()) {
            request.add_header(http_header_view{header_name, header_value});
        }
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        const auto response =
            context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
        return std::pair(response.status(), std::string(response.header("ETag").value_or("")));
    };

    const auto [initial_status, old_etag] = serve("GET");
    RUVIA_CHECK_EQ(initial_status, ruvia::http_status::ok);
    RUVIA_CHECK(!old_etag.empty());
    RUVIA_CHECK_EQ(serve("GET", "If-None-Match", old_etag).first,
        ruvia::http_status::not_modified);

    std::error_code error;
    const auto before = ruvia::detail::snapshot_response_file(path.c_str(), error);
    RUVIA_CHECK(!error);
    bool identity_changed = false;
    for (int attempt_value = 0; attempt_value < 20 && !identity_changed; ++attempt_value) {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << new_contents;
        }
        const auto after = ruvia::detail::snapshot_response_file(path.c_str(), error);
        RUVIA_CHECK(!error);
        identity_changed = !error && after.identity_ != before.identity_;
        if (!identity_changed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    RUVIA_CHECK(identity_changed);
    if (identity_changed) {
        const auto expect_stale_rejected = [&](std::string_view method, std::string_view header_name,
                                               std::string_view header_value) {
            bool failed_closed = false;
            try {
                (void)serve(method, header_name, header_value);
            } catch (const ruvia::http_error& http_error_value) {
                failed_closed = http_error_value.info().status() == ruvia::http_status::internal_server_error;
            }
            RUVIA_CHECK(failed_closed);
        };
        expect_stale_rejected("GET", "If-None-Match", old_etag);
        expect_stale_rejected("HEAD", {}, {});
        expect_stale_rejected("GET", "Range", "bytes=999999-");
        expect_stale_rejected("POST", "If-Match", "\"different\"");
    }
    fs::remove_all(dir);
#endif
}

RUVIA_TEST(static_file_selects_precompressed_representation_atomically) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_variant_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "data.txt.gz", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'g');  // sidecar bytes; served verbatim
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "data.txt.br", std::ios::binary | std::ios::trunc);
        const std::string content(30, 'b');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "data.txt.zst", std::ios::binary | std::ios::trunc);
        const std::string content(40, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    {
        std::ofstream out(dir / "gzip-only.txt", std::ios::binary | std::ios::trunc);
        out << "identity";
    }
    {
        std::ofstream out(dir / "gzip-only.txt.gz", std::ios::binary | std::ios::trunc);
        out << "gzip";
    }
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    struct served_representation final {
        std::string content_encoding_;
        std::string vary_;
        std::uint64_t size_{0};
    };
    const auto serve = [&root](std::string_view relative, std::string_view accept_encoding,
                           bool precompressed = true) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        if (!accept_encoding.empty()) {
            request.add_header(http_header_view{"Accept-Encoding", accept_encoding});
        }
        auto services = ruvia::test::test_context_services();
        if (precompressed) {
            services = services.with_precompressed_static_files();
        }
        auto context_value = context_access::make(memory, request, services);
        const auto response =
            context_value.static_file(root, {.relative_path_ = relative, .content_type_ = "text/plain"});
        const auto file = response.file_body();
        return served_representation{
            .content_encoding_ = std::string(response.header("Content-Encoding").value_or("")),
            .vary_ = std::string(response.header("Vary").value_or("")),
            .size_ = file.has_value() ? file->length() : 0};
    };

    // Accept-Encoding: gzip with a .gz sidecar present serves the gzip variant,
    // marked Content-Encoding: gzip and Vary: Accept-Encoding so a cache keys on it.
    const auto gz = serve("data.txt", "gzip");
    RUVIA_CHECK_EQ(gz.content_encoding_, std::string("gzip"));
    RUVIA_CHECK_EQ(gz.size_, std::uint64_t{20});
    RUVIA_CHECK((gz.vary_.find("Accept-Encoding") != std::string_view::npos));

    const auto compression_disabled = serve("data.txt", "gzip", false);
    RUVIA_CHECK(compression_disabled.content_encoding_.empty());
    RUVIA_CHECK_EQ(compression_disabled.size_, std::uint64_t{100});

    const auto br = serve("data.txt", "br");
    RUVIA_CHECK_EQ(br.content_encoding_, std::string("br"));
    RUVIA_CHECK_EQ(br.size_, std::uint64_t{30});

    const auto zstd = serve("data.txt", "zstd");
    RUVIA_CHECK_EQ(zstd.content_encoding_, std::string("zstd"));
    RUVIA_CHECK_EQ(zstd.size_, std::uint64_t{40});

    // Equal quality uses the one canonical server preference order.
    const auto tied = serve("data.txt", "gzip, zstd, br");
    RUVIA_CHECK_EQ(tied.content_encoding_, std::string("br"));
    RUVIA_CHECK_EQ(tied.size_, std::uint64_t{30});

    const auto prefers_gzip = serve("data.txt", "gzip;q=1, br;q=0.5, zstd;q=0.25");
    RUVIA_CHECK_EQ(prefers_gzip.content_encoding_, std::string("gzip"));
    RUVIA_CHECK_EQ(prefers_gzip.size_, std::uint64_t{20});

    // identity is implicitly q=1. A lower-quality gzip preference must leave
    // the original representation selected even when a sidecar exists.
    const auto prefers_identity = serve("data.txt", "gzip;q=0.5");
    RUVIA_CHECK(prefers_identity.content_encoding_.empty());
    RUVIA_CHECK_EQ(prefers_identity.size_, std::uint64_t{100});

    // Without Accept-Encoding the plain file is served, with no Content-Encoding.
    const auto plain = serve("data.txt", "");
    RUVIA_CHECK(plain.content_encoding_.empty());
    RUVIA_CHECK_EQ(plain.size_, std::uint64_t{100});

    // When the best client preference is unavailable on disk, the selector
    // must choose the best existing sidecar rather than reimplementing q-value
    // ranking in the static-file layer.
    const auto missing_brotli = serve("gzip-only.txt", "br, gzip, identity;q=0");
    RUVIA_CHECK_EQ(missing_brotli.content_encoding_, std::string("gzip"));
    RUVIA_CHECK_EQ(missing_brotli.size_, std::uint64_t{4});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_rejects_a_stale_precompressed_sidecar) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_stale_sidecar_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        out << "current identity";
    }
    {
        std::ofstream out(dir / "data.txt.gz", std::ios::binary | std::ios::trunc);
        out << "old gzip bytes";
    }

    std::error_code ec;
    const auto identity_time = fs::last_write_time(dir / "data.txt", ec);
    RUVIA_CHECK(!ec);
    fs::last_write_time(dir / "data.txt.gz", identity_time - std::chrono::seconds(10), ec);
    RUVIA_CHECK(!ec);

    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    request.add_header(http_header_view{"Accept-Encoding", "gzip"});
    auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
    const auto response =
        context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"});
    const auto file = response.file_body();

    // The stale sidecar is not a representation of the current identity file;
    // negotiation must fall back to the current file instead of serving old
    // bytes under Content-Encoding: gzip.
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK_EQ(file->length(), std::uint64_t{std::string_view("current identity").size()});
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_internal_sidecar_does_not_bypass_file_type_policy) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_sidecar_policy_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "app.js", std::ios::binary | std::ios::trunc);
        out << "identity";
    }
    {
        std::ofstream out(dir / "app.js.gz", std::ios::binary | std::ios::trunc);
        out << "gzip";
    }
    // The default policy allows .js but not .gz. The .gz file is indexed only
    // as an internal representation of app.js for Accept-Encoding negotiation.
    static_root root(dir);

    const auto serve = [](const static_root& selected_root, std::string_view path,
                           std::string_view accept_encoding) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        if (!accept_encoding.empty()) {
            request.add_header(http_header_view{"Accept-Encoding", accept_encoding});
        }
        auto context_value = context_access::make(
            memory, request, ruvia::test::test_context_services().with_precompressed_static_files());
        try {
            const auto response = context_value.static_file(selected_root, {.relative_path_ = path});
            const auto file = response.file_body();
            return std::tuple{response.status(),
                std::string(response.header("Content-Encoding").value_or("")),
                file.has_value() ? file->length() : std::uint64_t{0}};
        } catch (const ruvia::http_error& error) {
            return std::tuple{error.info().status(), std::string{}, std::uint64_t{0}};
        }
    };

    const auto negotiated = serve(root, "app.js", "gzip");
    RUVIA_CHECK_EQ(std::get<0>(negotiated), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(std::get<1>(negotiated), std::string("gzip"));
    RUVIA_CHECK_EQ(std::get<2>(negotiated), std::uint64_t{4});

    // Directly requesting the internal sidecar must follow the same extension
    // allow-list as every other public path. Before this guard, it returned the
    // raw compressed bytes as an identity application/octet-stream response.
    RUVIA_CHECK_EQ(std::get<0>(serve(root, "app.js.gz", "")), ruvia::http_status::not_found);

    // A policy that explicitly allows .gz still exposes it as a normal file;
    // only entries admitted solely because their base type is allowed are
    // internal-only.
    ruvia::static_root_options gzip_options;
    gzip_options.file_types_ = ruvia::static_file_type_policy{
        .kind_ = ruvia::static_file_type_policy::kind_type::only, .extensions_ = {"gz"}};
    static_root gzip_root(dir, std::move(gzip_options));
    RUVIA_CHECK_EQ(std::get<0>(serve(gzip_root, "app.js.gz", "")), ruvia::http_status::ok);

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

RUVIA_TEST(static_file_rejects_an_empty_accept_encoding_set) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_no_acceptable_coding_dir";
    fs::create_directories(dir);
    std::ofstream(dir / "data.txt") << "content";
    std::ofstream(dir / "data.txt.gz") << "compressed-content";
    static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    static_root root(dir, std::move(options));

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    static_file_test_request request(memory.resource());
    request.set_method("GET");
    request.set_target("/data.txt");
    request.set_path("/data.txt");
    request.add_header(http_header_view{"Accept-Encoding", "identity;q=0, *;q=0"});
    auto context_value = context_access::make(
        memory, request, ruvia::test::test_context_services().with_precompressed_static_files());

    bool rejected = false;
    try {
        static_cast<void>(
            context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"}));
    } catch (const ruvia::http_error& error) {
        rejected = error.info().status() == ruvia::http_status::not_acceptable;
    }
    RUVIA_CHECK(rejected);

    asio::io_context io;
    ruvia::detail::route_table routes_value(memory.resource());
    const auto resolution = routes_value.resolve(request);
    auto routed =
        run_static_compression_task(io, routes_value.dispatch_buffered_response(request, resolution, memory,
                                            ruvia::detail::document_root_binding::standalone(root),
                                            ruvia::test::test_context_services(),
                                            ruvia::detail::static_file_selection_mode::precompressed));
    RUVIA_CHECK_EQ(routed.status(), ruvia::http_status::not_acceptable);
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_modified_since_serving) {
    namespace fs = std::filesystem;
    using ruvia::http_header_view;
    using ruvia::static_root;
    using ruvia::static_root_options;
    using ruvia::detail::context_access;

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
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        request.add_header(http_header_view{"If-Modified-Since", if_modified_since});
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        return context_value.static_file(root, {.relative_path_ = "data.txt", .content_type_ = "text/plain"})
            .status();
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
    using ruvia::detail::context_access;

    const auto dir = fs::temp_directory_path() / "ruvia_static_dir_index";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "other.txt", std::ios::binary | std::ios::trunc);
        out << "x";
    }

    const auto serve_root = [](static_root& root) -> ruvia::http_status_code {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method("GET");
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        try {
            return context_value.static_file(root, {.relative_path_ = "", .content_type_ = "text/html"})
                .status();
        } catch (const ruvia::http_error& error) {
            return error.info().status();
        }
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

RUVIA_TEST(document_root_responses_run_global_unmatched_security_middleware) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_security_headers";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "<html>safe</html>";
    ruvia::static_root root(dir);
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    const std::array middleware_value{ruvia::detail::make_middleware_descriptor<ruvia::security_headers_middleware>()};
    impl.set_global_middlewares(middleware_value);
    impl.finalize();
    for (const auto method : {"GET", "HEAD"}) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        static_file_test_request request(memory.resource());
        request.set_method(method);
        request.set_path("/index.html");
        auto& routes_value = impl.route_table();
        const auto resolution = routes_value.resolve(request);
        asio::io_context io;
        auto response = run_static_compression_task(io, routes_value.dispatch_buffered_response(
                                                            request, resolution, memory, ruvia::detail::document_root_binding::configured(root),
                                                            ruvia::test::test_context_services().with_tls_transport("203.0.113.9")));
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK(response.header("X-Content-Type-Options") == "nosniff");
        RUVIA_CHECK(response.header("X-Frame-Options").has_value());
        RUVIA_CHECK(response.header("Strict-Transport-Security").has_value());
    }
    fs::remove_all(dir);
}
