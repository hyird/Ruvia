#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/server_config.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// A document root shares paths with a guarded wildcard route. This executable
// owns its application, so the document root cannot leak into other suites.

namespace {

namespace fs = std::filesystem;

class document_root_guard final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        if (!c.req().header("X-Test-Credential").has_value()) {
            throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized});
        }
        co_await next_value();
    }
};

class document_root_routes final {
    RUVIA_ROUTES_BEGIN(document_root_routes)
    RUVIA_GET("/private/*", private_file, document_root_guard);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> private_file(ruvia::context& c) {
        co_return c.text("guarded");
    }
};

[[nodiscard]] fs::path make_document_root() {
    const auto dir = fs::temp_directory_path() / "ruvia_document_root_dot_segment_test";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir / "private");
    std::ofstream(dir / "private" / "file.txt") << "secret";
    std::ofstream(dir / "public.txt") << "public";
    std::ofstream(dir / "my report.txt") << "spaced";
    return dir;
}

// Sends one request on a fresh connection and returns the complete response.
[[nodiscard]] std::string exchange(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    asio::write(socket, asio::buffer(request));
    std::string output;
    std::error_code error;
    asio::read(socket, asio::dynamic_buffer(output), error);
    return output;
}

[[nodiscard]] bool has_status(std::string_view response, std::string_view status) {
    return response.starts_with("HTTP/1.1 ") && response.substr(9).starts_with(status);
}

}  // namespace

RUVIA_TEST(document_root_never_serves_targets_rejected_by_routing) {
    const auto dir = make_document_root();
    ruvia::document_root_config config;
    config.root_ = dir;
    config.static_options_.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::app().document_root(std::move(config));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);

    // The document root still answers paths no route claims.
    const auto public_file = exchange(io, server.endpoint(), "/public.txt");
    RUVIA_CHECK(has_status(public_file, "200"));
    RUVIA_CHECK(public_file.ends_with("public"));

    // The guarded route owns its prefix.
    RUVIA_CHECK(has_status(exchange(io, server.endpoint(), "/private/file.txt"), "401"));

    // A dot-segment spelling of the same resource is rejected outright: it
    // reaches neither the route nor the document root's normalized lookup.
    for (const std::string_view target :
        {"/private/./file.txt", "/private/%2E/file.txt", "/private/%2e/file.txt", "/private/x/../file.txt"}) {
        const auto response = exchange(io, server.endpoint(), target);
        RUVIA_CHECK(has_status(response, "404"));
        RUVIA_CHECK(response.find("secret") == std::string::npos);
    }

    // The request path is decoded exactly once into the file name, and no
    // escape may reveal a separator routing never split on.
    const auto spaced = exchange(io, server.endpoint(), "/my%20report.txt");
    RUVIA_CHECK(has_status(spaced, "200"));
    RUVIA_CHECK(spaced.ends_with("spaced"));
    for (const std::string_view target : {"/my%2520report.txt", "/private%2Ffile.txt", "/private%5Cfile.txt"}) {
        const auto response = exchange(io, server.endpoint(), target);
        RUVIA_CHECK(has_status(response, "404"));
        RUVIA_CHECK(response.find("secret") == std::string::npos);
    }
    server.finish();
}
