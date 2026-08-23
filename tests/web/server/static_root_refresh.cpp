// Document-root refresh must publish a newly indexed file without
// moving directory scans or file reads onto the HTTP worker.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

namespace {

[[nodiscard]] std::string readHead(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::error_code& ec) {
    const std::size_t bytes = asio::read_until(socket, buffer, "\r\n\r\n", ec);
    if (ec) {
        return {};
    }
    std::string head(asio::buffers_begin(buffer.data()), asio::buffers_begin(buffer.data()) + bytes);
    buffer.consume(bytes);
    return head;
}

[[nodiscard]] std::size_t contentLength(std::string_view head) {
    for (std::string_view rest = head; !rest.empty();) {
        const auto eol = rest.find("\r\n");
        const auto line = rest.substr(0, eol);
        constexpr std::string_view name = "content-length:";
        if (line.size() > name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                const auto lower = [](char c) noexcept {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                if (lower(line[i]) != name[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::size_t value = 0;
                for (char c : line.substr(name.size())) {
                    if (c >= '0' && c <= '9') {
                        value = value * 10 + static_cast<std::size_t>(c - '0');
                    }
                }
                return value;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(eol + 2);
    }
    return std::string_view::npos;
}

[[nodiscard]] std::string readBody(asio::ip::tcp::socket& socket, asio::streambuf& buffer, std::size_t size, std::error_code& ec) {
    std::string body(size, '\0');
    std::size_t copied = std::min(buffer.size(), size);
    if (copied != 0) {
        asio::buffer_copy(asio::buffer(body.data(), copied), buffer.data());
        buffer.consume(copied);
    }
    if (copied < size) {
        asio::read(socket, asio::buffer(body.data() + copied, size - copied), ec);
    }
    return body;
}

struct FetchResult final {
    std::string head;
    std::string body;
};

[[nodiscard]] bool hasHeaderValue(std::string_view head, std::string_view name, std::string_view value) {
    for (std::string_view rest = head; !rest.empty();) {
        const auto eol = rest.find("\r\n");
        const auto line = rest.substr(0, eol);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto lower = [](char c) noexcept {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            bool nameMatches = colon == name.size();
            for (std::size_t i = 0; nameMatches && i < name.size(); ++i) {
                nameMatches = lower(line[i]) == lower(name[i]);
            }
            std::string_view headerValue = line.substr(colon + 1);
            while (!headerValue.empty() && (headerValue.front() == ' ' || headerValue.front() == '\t')) {
                headerValue.remove_prefix(1);
            }
            while (!headerValue.empty() && (headerValue.back() == ' ' || headerValue.back() == '\t')) {
                headerValue.remove_suffix(1);
            }
            if (nameMatches && headerValue == value) {
                return true;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(eol + 2);
    }
    return false;
}

[[nodiscard]] std::string gzipDecode(std::string_view encoded) {
    auto result = ruvia::decodeHttpContent(
        ruvia::HttpContentCoding::kGzip,
        encoded,
        {.maxDecodedBytes = 4096, .resource = std::pmr::get_default_resource()});
    auto* decoded = result.decoded();
    return decoded == nullptr ? std::string{} : std::string(decoded->bytes());
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_root_refresh_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string initialBody = "initial";
    {
        std::ofstream output(dir / "initial.txt", std::ios::binary);
        output << initialBody;
    }
    ruvia::StaticRootOptions rootOptions;
    rootOptions.fileTypes = ruvia::StaticFileTypePolicy{.kind = ruvia::StaticFileTypePolicy::Kind::kAll};
    ruvia::StaticRoot root(dir, std::move(rootOptions));

    ruvia::DocumentRootRuntimeConfig documentRootRuntime;
    documentRootRuntime.refreshInterval = std::chrono::milliseconds(20);

    std::pmr::memory_resource* resource = std::pmr::get_default_resource();
    ruvia::detail::RouteTable routes(resource);
    ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1});
    ruvia::detail::HttpServerOptions options;
    options.documentRoot = ruvia::detail::HttpServerOptions::DocumentRoot::refreshing(
        root,
        documentRootRuntime,
        {.gzip = true, .minBytes = 1, .maxBytes = 4096});
    options.compression.emplace();
    options.blockingPool = &pool;

    ruvia::detail::WebWorkerRuntime server(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), routes, {}, options);
    server.start();
    const auto endpoint = server.localEndpoint();

    int rc = 0;
    auto fail = [&](int code, const char* message) {
        std::fputs(message, stderr);
        std::fputc('\n', stderr);
        rc = code;
    };

    asio::io_context context;
    const auto fetch = [&](std::string_view path, std::string_view acceptEncoding = {}) -> std::optional<FetchResult> {
        asio::ip::tcp::socket socket(context);
        std::error_code ec;
        socket.connect(endpoint, ec);
        if (ec) {
            return std::nullopt;
        }
        std::string request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n";
        if (!acceptEncoding.empty()) {
            request += "Accept-Encoding: ";
            request.append(acceptEncoding.data(), acceptEncoding.size());
            request += "\r\n";
        }
        request += "\r\n";
        asio::write(socket, asio::buffer(request), ec);
        if (ec) {
            return std::nullopt;
        }
        asio::streambuf buffer;
        const auto head = readHead(socket, buffer, ec);
        const auto length = contentLength(head);
        if (ec || length == std::string_view::npos) {
            return std::nullopt;
        }
        const auto body = readBody(socket, buffer, length, ec);
        if (ec || !head.starts_with("HTTP/1.1 200")) {
            return std::nullopt;
        }
        return FetchResult{head, body};
    };

    const auto fetchBody = [&](std::string_view path) -> std::optional<std::string> {
        const auto result = fetch(path);
        if (!result.has_value()) {
            return std::nullopt;
        }
        return result->body;
    };

    const auto initial = fetchBody("/initial.txt");
    if (!initial.has_value() || *initial != initialBody) {
        fail(1, "initial document-root file was not served");
    }

    const std::string refreshedBody(2048, 'r');
    {
        std::ofstream output(dir / "new.txt", std::ios::binary);
        output << refreshedBody;
    }

    bool refreshed = false;
    for (int attempt = 0; attempt != 60 && !refreshed; ++attempt) {
        const auto body = fetchBody("/new.txt");
        refreshed = body.has_value() && *body == refreshedBody;
        if (!refreshed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!refreshed) {
        fail(2, "new document-root file was not published by refresh");
    }

    bool refreshedGzip = false;
    for (int attempt = 0; attempt != 60 && !refreshedGzip; ++attempt) {
        const auto result = fetch("/new.txt", "gzip, identity;q=0");
        refreshedGzip = result.has_value() &&
                        hasHeaderValue(result->head, "Content-Encoding", "gzip") &&
                        gzipDecode(result->body) == refreshedBody;
        if (!refreshedGzip) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!refreshedGzip) {
        fail(6, "new document-root file was not published as a refreshed gzip variant");
    }

    // A failed rebuild must retain the last complete snapshot and become
    // observable through server stats. Replace the directory with a regular
    // file so the background constructor cannot index it, then restore it for
    // normal teardown.
    const auto displacedRoot = dir.parent_path() / (dir.filename().string() + "_displaced");
    std::error_code fsError;
    fs::remove_all(displacedRoot, fsError);
    fsError.clear();
    fs::rename(dir, displacedRoot, fsError);
    if (fsError) {
        fail(3, "could not displace document-root directory for failure test");
    } else {
        std::ofstream invalidRoot(dir, std::ios::binary);
        invalidRoot << "not a directory";
        invalidRoot.close();

        bool sawRefreshFailure = false;
        for (int attempt = 0; attempt != 60 && !sawRefreshFailure; ++attempt) {
            sawRefreshFailure = server.stats().documentRootRefreshFailures != 0;
            if (!sawRefreshFailure) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!sawRefreshFailure) {
            fail(4, "failed document-root refresh was not observable");
        }

        fs::remove(dir, fsError);
        fsError.clear();
        fs::rename(displacedRoot, dir, fsError);
        if (fsError) {
            fail(5, "could not restore document-root directory after failure test");
        }
    }

    server.stop();
    server.join();
    fs::remove_all(dir);
    fs::remove_all(displacedRoot);
    return rc;
}
