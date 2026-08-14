#include "test_harness.h"

#include "test_io_context.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <concepts>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/response/HttpStaticFileCompression.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;

[[nodiscard]] ruvia::detail::HttpResponseCodingSelection gzipResponseCoding() {
    ruvia::detail::HttpResponseCodingQualities qualities;
    qualities.update("gzip");
    const auto selection = ruvia::detail::HttpResponseCodingSelection::select(qualities);
    const auto* selected = selection.selected();
    if (selected == nullptr) {
        throw std::logic_error("test Accept-Encoding did not select gzip");
    }
    return *selected;
}

template <typename Result>
[[nodiscard]] Result runStaticCompressionTask(asio::io_context& context, ruvia::Task<Result> task) {
    std::optional<Result> result;
    std::exception_ptr exception;
    asio::co_spawn(
        context,
        [task = std::move(task), &result, &exception]() mutable -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::detail::taskAsAwaitable(std::move(task)));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    context.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!result.has_value()) {
        throw std::logic_error("static compression task produced no result");
    }
    return std::move(*result);
}

}  // namespace

RUVIA_TEST(static_root_copies_public_mime_configuration_into_owned_storage) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_mime_resource_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.custom-resource") << "content";

    {
        ruvia::StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        options.mimeTypes.push_back(ruvia::StaticMimeType{
            .extension = ".custom-resource",
            .contentType = "application/x-custom-resource-type",
        });
        ruvia::StaticRoot root(dir, std::move(options));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_permission_errors_in_index) {
#if defined(_WIN32)
    // Windows ACLs are not expressible through std::filesystem::perms in a
    // portable way; the runtime integration guard covers refresh failures.
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
    std::error_code probeEc;
    fs::directory_iterator probe(restricted, probeEc);
    const fs::directory_iterator end;
    for (; !probeEc && probe != end; probe.increment(probeEc)) {
    }
    const bool permissionDenied = static_cast<bool>(probeEc);

    if (!permissionDenied) {
        fs::permissions(restricted, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::replace, ec);
        RUVIA_CHECK(!ec);
        fs::remove_all(dir);
        return;
    }

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    bool rejected = false;
    try {
        ruvia::StaticRoot root(dir, std::move(options));
    } catch (const fs::filesystem_error&) {
        rejected = true;
    }
    fs::permissions(restricted, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all, fs::perm_options::replace, ec);
    RUVIA_CHECK(!ec);
    RUVIA_CHECK(rejected);
    fs::remove_all(dir);
#endif
}

// Serving a file: preconditions, ranges, precompressed variants, type policy
// and the traversal-safe path resolution behind them.

RUVIA_TEST(static_file_response_owns_path_after_handler_local_root_is_destroyed) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::ContextServices;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_local_root";
    fs::create_directories(dir);
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << "owned-static-path";
    }

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    auto response = [&] {
        ruvia::StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        ruvia::StaticRoot handlerLocalRoot(dir, std::move(options));
        return context.staticFile(handlerLocalRoot, "payload.txt", "text/plain");
    }();

    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        auto input = ruvia::detail::openResponseFileInput(*file);
        std::string body(std::string("owned-static-path").size(), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(input.gcount(), static_cast<std::streamsize>(body.size()));
        RUVIA_CHECK_EQ(body, std::string("owned-static-path"));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(response_file_input_rejects_in_place_mutation_after_open) {
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia_static_in_place_mutation.bin";
    fs::remove(path);
    constexpr std::string_view oldContents = "old-static-body";
    constexpr std::string_view newContents = "new-static-body";
    static_assert(oldContents.size() == newContents.size());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << oldContents;
    }
    // Some filesystems expose a coarse change-time token; separate the two
    // writes so this test exercises the same identity signal the runtime uses.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::error_code error;
    const auto snapshot = ruvia::detail::snapshotResponseFile(path.c_str(), error);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(snapshot.identity.requiresValidation());
    if (error || !snapshot.identity.requiresValidation()) {
        fs::remove(path);
        return;
    }

    const auto file = ruvia::detail::ResponseFileBodyAccess::make(path.c_str(), snapshot.size, 0, snapshot.size, snapshot.identity);
    auto input = ruvia::detail::openResponseFileInput(file);
    RUVIA_CHECK(static_cast<bool>(input));
    if (!input) {
        fs::remove(path);
        return;
    }

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << newContents;
    }
    const auto changedSnapshot = ruvia::detail::snapshotResponseFile(path.c_str(), error);
    RUVIA_CHECK(!error);
    RUVIA_CHECK(changedSnapshot.identity != snapshot.identity);
    RUVIA_CHECK(!input.matchesSnapshot(snapshot.identity, snapshot.size));
    fs::remove(path);
#endif
}

RUVIA_TEST(static_file_without_sidecar_uses_bounded_blocking_compression) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_runtime_compression";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string original(4096, 'c');
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << original;
    }
    std::string incompressible(8192, '\0');
    std::uint32_t state = 0x9e3779b9U;
    for (auto& byte : incompressible) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<char>(state >> 24U);
    }
    {
        std::ofstream output(dir / "incompressible.bin", std::ios::binary | std::ios::trunc);
        output.write(incompressible.data(), static_cast<std::streamsize>(incompressible.size()));
    }
    {
        std::ofstream output(dir / "empty.txt", std::ios::binary | std::ios::trunc);
    }

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::DocumentRootRuntimeOptions runtimeOptions;
    runtimeOptions.onDemandCompressionMaxBytes = 8192;
    ruvia::StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setResource(request, requestMemory.resource());
    ruvia::detail::HttpRequestAccess::addHeader(request, ruvia::HttpHeaderView{"Accept-Encoding", "gzip"}, ruvia::detail::HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kAcceptEncoding));
    auto context = ruvia::detail::ContextAccess::make(requestMemory, request);

    ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1});
    asio::io_context& io = ruvia::test::newTestIoContext();
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto workerHandle = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    const auto responseCoding = gzipResponseCoding();

    auto belowMinResponse = context.staticFile(root, "payload.txt", "text/plain");
    const auto skipped = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(belowMinResponse, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = original.size() + 1}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));
    RUVIA_CHECK(skipped.notApplicable());
    RUVIA_CHECK_EQ(skipped.status(), ruvia::detail::HttpStaticFileCompressionStatus::kNotApplicable);
    RUVIA_CHECK(ruvia::detail::responseBody(belowMinResponse).file().has_value());

    auto noTransformResponse = context.staticFile(root, "payload.txt", "text/plain");
    noTransformResponse.header("Cache-Control", "no-transform");
    const auto beforePolicySkip = pool.stats();
    io.restart();
    const auto policySkipped = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(noTransformResponse, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = 1024}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));
    const auto afterPolicySkip = pool.stats();
    RUVIA_CHECK(policySkipped.notApplicable());
    RUVIA_CHECK(ruvia::detail::responseBody(noTransformResponse).file().has_value());
    RUVIA_CHECK_EQ(afterPolicySkip.completed, beforePolicySkip.completed);
    RUVIA_CHECK_EQ(afterPolicySkip.rejected, beforePolicySkip.rejected);

    io.restart();
    auto response = context.staticFile(root, "payload.txt", "text/plain");
    const auto compressed = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(response, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = 1024}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));

    RUVIA_CHECK(compressed.compressed());
    RUVIA_CHECK_EQ(compressed.status(), ruvia::detail::HttpStaticFileCompressionStatus::kCompressed);
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(ruvia::detail::responseBody(response).file() == std::nullopt);
    const auto decoded = ruvia::decodeHttpContent(ruvia::HttpContentCoding::kGzip, ruvia::detail::responseBody(response).bytes(), original.size(), std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view(original));
    }

    // An incompressible representation is a valid policy miss. The encoder
    // must not turn that expected outcome into the 500 path used for I/O or
    // codec failures.
    auto directEncoding = ruvia::encodeHttpContent(
        ruvia::HttpContentCoding::kGzip,
        incompressible,
        incompressible.size() - 1,
        std::pmr::get_default_resource());
    RUVIA_CHECK(directEncoding.failure() != nullptr);
    if (const auto* failure = directEncoding.failure()) {
        RUVIA_CHECK_EQ(failure->error(), ruvia::HttpContentEncodeError::kEncodedSizeExceeded);
    }

    auto notSmallerResponse = context.staticFile(root, "incompressible.bin", "application/octet-stream");
    io.restart();
    const auto notSmaller = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(notSmallerResponse, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = 1024}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));
    RUVIA_CHECK(notSmaller.notApplicable());
    RUVIA_CHECK_EQ(notSmaller.status(), ruvia::detail::HttpStaticFileCompressionStatus::kNotApplicable);
    RUVIA_CHECK(ruvia::detail::responseBody(notSmallerResponse).file().has_value());

    // An empty file is a valid identity representation. With identity
    // forbidden, compression is simply not applicable because every coding
    // has non-zero framing overhead; it must not become an I/O/codec failure.
    auto emptyResponse = context.staticFile(root, "empty.txt", "text/plain");
    io.restart();
    const auto empty = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(emptyResponse, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = 0}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));
    RUVIA_CHECK(empty.notApplicable());
    RUVIA_CHECK_EQ(empty.status(), ruvia::detail::HttpStaticFileCompressionStatus::kNotApplicable);
    RUVIA_CHECK(ruvia::detail::responseBody(emptyResponse).file().has_value());

    // A file that disappears after routing is an internal serving failure, not
    // a negotiation miss. The typed outcome keeps that distinction available to
    // the protocol driver (which maps it to 500 when identity is forbidden).
    auto failedResponse = context.staticFile(root, "payload.txt", "text/plain");
    fs::remove(dir / "payload.txt");
    io.restart();
    const auto failed = runStaticCompressionTask(io, ruvia::detail::tryCompressStaticFileResponse(failedResponse, responseCoding, ruvia::HttpKnownMethod::kGet, ruvia::CompressionConfig{.minBytes = 1024}, runtimeOptions.onDemandCompressionMaxBytes, &pool, workerHandle));
    RUVIA_CHECK(failed.failed());
    RUVIA_CHECK_EQ(failed.status(), ruvia::detail::HttpStaticFileCompressionStatus::kFailed);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_document_root_can_defer_identity_for_runtime_compression) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::ContextServices;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;
    using ruvia::detail::StaticFileSelectionMode;

    const auto dir = fs::temp_directory_path() / "ruvia_static_deferred_compression";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string original(4096, 'd');
    {
        std::ofstream output(dir / "payload.txt", std::ios::binary | std::ios::trunc);
        output << original;
    }

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::DocumentRootRuntimeOptions runtimeOptions;
    runtimeOptions.onDemandCompressionMaxBytes = original.size();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory requestMemory(workerMemory);
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setTarget(request, "/payload.txt");
    HttpRequestAccess::setPath(request, "/payload.txt");
    HttpRequestAccess::setResource(request, requestMemory.resource());
    HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Accept-Encoding", "gzip, identity;q=0"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));

    // Public Context::staticFile remains strict: a direct handler call must
    // not temporarily return identity bytes that violate the request.
    auto context = ContextAccess::make(requestMemory, request);
    bool rejected = false;
    try {
        static_cast<void>(context.staticFile(root, "payload.txt", "text/plain"));
    } catch (const ruvia::HttpError& error) {
        rejected = error.info().status() == ruvia::http_status::kNotAcceptable;
    }
    RUVIA_CHECK(rejected);

    // A server that owns the deferred-compression policy gives handler-produced
    // static files the same placeholder semantics as its document-root
    // fallback. The final session stage is responsible for replacing this file
    // body with the selected coding before it commits the wire plan.
    auto configuredContext = ContextAccess::make(requestMemory, request, ContextServices{}.withDeferredStaticFileCompression());
    auto configuredResponse = configuredContext.staticFile(root, "payload.txt", "text/plain");
    RUVIA_CHECK(configuredResponse.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos);
    RUVIA_CHECK(ruvia::detail::responseBody(configuredResponse).file().has_value());

    auto configuredFileResponse = configuredContext.file(dir / "payload.txt", "text/plain");
    RUVIA_CHECK(configuredFileResponse.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos);
    RUVIA_CHECK(ruvia::detail::responseBody(configuredFileResponse).file().has_value());

    asio::io_context& io = ruvia::test::newTestIoContext();
    ruvia::detail::RouteTable routes(requestMemory.resource());
    const auto resolution = routes.resolve(request);
    auto bufferedResult = runStaticCompressionTask(
        io,
        routes.dispatchBufferedResponse(
            request,
            resolution,
            requestMemory,
            ruvia::detail::DocumentRootBinding::configured(root, runtimeOptions),
            {},
            StaticFileSelectionMode::kAllowDeferredCompression));
    RUVIA_CHECK(bufferedResult.documentRoot() != nullptr);
    auto response = std::move(bufferedResult).takeResponse();
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(ruvia::detail::responseBody(response).file().has_value());

    ruvia::BlockingPool pool(ruvia::BlockingPoolOptions{.threadCount = 1});
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto workerHandle = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    const auto responseCoding = gzipResponseCoding();
    io.restart();
    const auto compressionResult = runStaticCompressionTask(
        io,
        ruvia::detail::tryCompressStaticFileResponse(
            response,
            responseCoding,
            ruvia::HttpKnownMethod::kGet,
            ruvia::CompressionConfig{.minBytes = 1024},
            runtimeOptions.onDemandCompressionMaxBytes,
            &pool,
            workerHandle));
    RUVIA_CHECK(compressionResult.compressed());
    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!ruvia::detail::responseBody(response).file().has_value());
    const auto decoded = ruvia::decodeHttpContent(
        ruvia::HttpContentCoding::kGzip,
        ruvia::detail::responseBody(response).bytes(),
        original.size(),
        std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view(original));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(document_root_runtime_options_control_live_reload_assets) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_live_reload";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "<html></html>";

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::DocumentRootRuntimeOptions runtimeOptions;
    runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;
    runtimeOptions.refreshInterval = std::chrono::milliseconds(37);
    runtimeOptions.onDemandCompressionMaxBytes = 12345;
    runtimeOptions.enableLiveReload = true;
    ruvia::StaticRoot root(dir, std::move(options));

    auto equivalentOptions = ruvia::detail::StaticRootAccess::options(root);
    ruvia::StaticRoot equivalentRoot(dir, std::move(equivalentOptions));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::sameSnapshot(root, equivalentRoot));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::revision(root) != ruvia::detail::StaticRootAccess::revision(equivalentRoot));

    auto clonedOptions = ruvia::detail::StaticRootAccess::options(root);
    RUVIA_CHECK(clonedOptions.fileTypes.kind() == ruvia::StaticFileTypePolicy::Kind::kAll);

    std::ofstream(dir / "new-file.txt") << "published on the next poll";
    ruvia::StaticRoot refreshed(dir, std::move(clonedOptions));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::fingerprint(root) != ruvia::detail::StaticRootAccess::fingerprint(refreshed));

    struct AssetResult final {
        ruvia::HttpStatusCode status;
        std::string contentType;
        std::string cacheControl;
        std::string body;
    };
    const auto fetch = [&root](std::string_view path, const ruvia::DocumentRootRuntimeOptions* runtime) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        auto request = ruvia::detail::HttpRequestAccess::make();
        ruvia::detail::HttpRequestAccess::reset(request);
        ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
        ruvia::detail::HttpRequestAccess::setTarget(request, path);
        ruvia::detail::HttpRequestAccess::setPath(request, path);
        ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
        ruvia::detail::RouteTable routes(memory.resource());
        auto resolution = routes.resolve(request);
        asio::io_context io;
        auto binding = runtime == nullptr ? ruvia::detail::DocumentRootBinding::standalone(root) : ruvia::detail::DocumentRootBinding::configured(root, *runtime);
        auto bufferedResult = runStaticCompressionTask(io, routes.dispatchBufferedResponse(request, resolution, memory, std::move(binding)));
        if ((runtime != nullptr) != (bufferedResult.documentRoot() != nullptr)) {
            throw std::logic_error("buffered dispatch returned an incorrect response origin");
        }
        auto response = std::move(bufferedResult).takeResponse();
        return AssetResult{
            response.status(),
            std::string(response.header("Content-Type").value_or("")),
            std::string(response.header("Cache-Control").value_or("")),
            std::string(ruvia::detail::responseBody(response).bytes()),
        };
    };

    const auto standaloneScript = fetch("/__ruvia/live-reload.js", nullptr);
    RUVIA_CHECK_EQ(standaloneScript.status, ruvia::http_status::kNotFound);

    const auto script = fetch("/__ruvia/live-reload.js", &runtimeOptions);
    RUVIA_CHECK_EQ(script.status, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(script.contentType, std::string("application/javascript; charset=UTF-8"));
    RUVIA_CHECK_EQ(script.cacheControl, std::string("no-store"));
    RUVIA_CHECK(script.body.find("/__ruvia/live-reload-version") != std::string::npos);

    const auto version = fetch("/__ruvia/live-reload-version", &runtimeOptions);
    RUVIA_CHECK_EQ(version.status, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(version.contentType, std::string("text/plain; charset=UTF-8"));
    RUVIA_CHECK_EQ(version.cacheControl, std::string("no-store"));
    RUVIA_CHECK_EQ(version.body, std::to_string(ruvia::detail::StaticRootAccess::revision(root)));

    fs::remove_all(dir);
}

RUVIA_TEST(polling_document_root_binding_is_a_move_only_request_snapshot_lease) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_binding_lease";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.txt") << "payload";

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot root(dir, std::move(options));
    ruvia::DocumentRootRuntimeOptions runtimeOptions;
    runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setTarget(request, "/payload.txt");
    ruvia::detail::HttpRequestAccess::setPath(request, "/payload.txt");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    ruvia::detail::RouteTable routes(memory.resource());
    const auto resolution = routes.resolve(request);
    auto binding = ruvia::detail::DocumentRootBinding::configured(root, runtimeOptions);
    auto task = routes.dispatchBufferedResponse(request, resolution, memory, std::move(binding));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::hasActiveBindings(root));
    asio::io_context io;
    auto result = runStaticCompressionTask(io, std::move(task));
    RUVIA_CHECK(result.documentRoot() != nullptr);
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(root));

    fs::remove_all(dir);
}

RUVIA_TEST(polling_document_root_binding_counts_belong_to_the_bound_snapshot) {
    namespace fs = std::filesystem;
    const auto firstDir = fs::temp_directory_path() / "ruvia_static_binding_first";
    const auto secondDir = fs::temp_directory_path() / "ruvia_static_binding_second";
    fs::remove_all(firstDir);
    fs::remove_all(secondDir);
    fs::create_directories(firstDir);
    fs::create_directories(secondDir);
    std::ofstream(firstDir / "payload.txt") << "first";
    std::ofstream(secondDir / "payload.txt") << "second";

    ruvia::StaticRootOptions firstOptions;
    firstOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot first(firstDir, std::move(firstOptions));
    ruvia::StaticRootOptions secondOptions;
    secondOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot second(secondDir, std::move(secondOptions));
    ruvia::DocumentRootRuntimeOptions runtimeOptions;
    runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;

    {
        auto firstBinding = ruvia::detail::DocumentRootBinding::configured(first, runtimeOptions);
        RUVIA_CHECK(ruvia::detail::StaticRootAccess::hasActiveBindings(first));
        RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(second));
        {
            auto secondBinding = ruvia::detail::DocumentRootBinding::configured(second, runtimeOptions);
            RUVIA_CHECK(ruvia::detail::StaticRootAccess::hasActiveBindings(first));
            RUVIA_CHECK(ruvia::detail::StaticRootAccess::hasActiveBindings(second));
        }
        RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(second));
        RUVIA_CHECK(ruvia::detail::StaticRootAccess::hasActiveBindings(first));
    }

    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(first));
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(second));
    fs::remove_all(firstDir);
    fs::remove_all(secondDir);
}

RUVIA_TEST(immutable_document_root_bindings_do_not_share_request_lease_state) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_immutable_bindings";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "payload.txt") << "payload";

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot root(dir, std::move(options));
    ruvia::DocumentRootRuntimeOptions runtimeOptions;

    {
        auto binding = ruvia::detail::DocumentRootBinding::configured(root, runtimeOptions);
        RUVIA_CHECK_EQ(binding.root(), &root);
        RUVIA_CHECK_EQ(binding.runtimeOptions(), &runtimeOptions);
        RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(root));
    }

    constexpr std::size_t kWorkerCount = 4;
    constexpr std::size_t kBindingsPerWorker = 10'000;
    std::array<std::thread, kWorkerCount> workers;
    for (auto& worker : workers) {
        worker = std::thread([&root, &runtimeOptions]() {
            for (std::size_t index = 0; index < kBindingsPerWorker; ++index) {
                auto binding = ruvia::detail::DocumentRootBinding::configured(root, runtimeOptions);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::hasActiveBindings(root));
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_replacement_cannot_reuse_indexed_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_replacement_identity";
    const auto servedPath = dir / "payload.txt";
    const auto replacementPath = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(servedPath, std::ios::binary);
        output << "old-representation";
    }

    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot root(dir, std::move(options));

    // Use the same byte length so a size-only guard would accept and transmit
    // the replacement under the old ETag/Last-Modified framing.
    {
        std::ofstream output(replacementPath, std::ios::binary);
        output << "new-representation";
    }
    static_assert(std::string_view("old-representation").size() == std::string_view("new-representation").size());
#if defined(_WIN32)
    fs::remove(servedPath);
#endif
    fs::rename(replacementPath, servedPath);

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);
    auto response = context.staticFile(root, "payload.txt", "text/plain");
    const std::string oldEtag(response.header("ETag").value_or(""));
    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requiresValidation());
        auto input = ruvia::detail::openResponseFileInput(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
        char byte = '\0';
        input.read(&byte, 1);
        RUVIA_CHECK_EQ(input.gcount(), std::streamsize{0});
    }

    ruvia::StaticRootOptions refreshedOptions;
    refreshedOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot refreshedRoot(dir, std::move(refreshedOptions));
    auto refreshed = context.staticFile(refreshedRoot, "payload.txt", "text/plain");
    RUVIA_CHECK(!oldEtag.empty());
    RUVIA_CHECK(refreshed.header("ETag").value_or("") != oldEtag);

    fs::remove_all(dir);
}

RUVIA_TEST(context_file_replacement_cannot_reuse_response_metadata) {
    namespace fs = std::filesystem;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_context_file_replacement_identity";
    const auto servedPath = dir / "payload.txt";
    const auto replacementPath = dir / "replacement.txt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream output(servedPath, std::ios::binary);
        output << "old-context-body";
    }

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);
    auto response = context.file(servedPath, "text/plain");

    {
        std::ofstream output(replacementPath, std::ios::binary);
        output << "new-context-body";
    }
#if defined(_WIN32)
    fs::remove(servedPath);
#endif
    fs::rename(replacementPath, servedPath);

    const auto file = ruvia::detail::responseBody(response).file();
    RUVIA_CHECK(file.has_value());
    if (file.has_value()) {
        RUVIA_CHECK(file->identity().requiresValidation());
        auto input = ruvia::detail::openResponseFileInput(*file);
        RUVIA_CHECK(!static_cast<bool>(input));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_type_policy_has_closed_exact_alternatives) {
    static_assert(!std::default_initializable<ruvia::StaticFileTypePolicy>);
    static_assert(!std::is_aggregate_v<ruvia::StaticFileTypePolicy>);

    bool emptyOnlyThrew = false;
    try {
        (void)ruvia::StaticFileTypePolicy::only({});
    } catch (const std::invalid_argument&) {
        emptyOnlyThrew = true;
    }
    RUVIA_CHECK(emptyOnlyThrew);

    for (const std::string_view invalid : {"", ".", "..", "a/b", "a\\b"}) {
        bool invalidTypeThrew = false;
        try {
            (void)ruvia::StaticFileTypePolicy::only({invalid});
        } catch (const std::invalid_argument&) {
            invalidTypeThrew = true;
        }
        RUVIA_CHECK(invalidTypeThrew);
    }

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_static_file_type_policy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "index.html") << "html";
    std::ofstream(dir / "asset.custom") << "custom";

    ruvia::StaticRoot defaultRoot(dir);
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(defaultRoot, "index.html").has_value());
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::find(defaultRoot, "asset.custom").has_value());

    ruvia::StaticRootOptions allOptions;
    allOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot allRoot(dir, std::move(allOptions));
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(allRoot, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(allRoot, "asset.custom").has_value());

    ruvia::StaticRootOptions onlyOptions;
    onlyOptions.fileTypes = ruvia::StaticFileTypePolicy::only({".CUSTOM"});
    ruvia::StaticRoot onlyRoot(dir, std::move(onlyOptions));
    RUVIA_CHECK(!ruvia::detail::StaticRootAccess::find(onlyRoot, "index.html").has_value());
    RUVIA_CHECK(ruvia::detail::StaticRootAccess::find(onlyRoot, "asset.custom").has_value());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_extension_preserves_unicode_without_ascii_aliasing) {
    std::pmr::monotonic_buffer_resource resource;
    const auto extension = ruvia::detail::lowerStaticFileExtension(std::filesystem::path(u8"asset.\u0168TML"), &resource);

    RUVIA_CHECK_EQ(std::string_view(extension), std::string_view(reinterpret_cast<const char*>(u8".\u0168tml")));
    RUVIA_CHECK(extension != ".html");
}

RUVIA_TEST(static_file_range_serving_status_and_content_range) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

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
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serveMethod = [&root](std::string_view method, std::string_view path, std::string_view range) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setResource(request, memory.resource());
        if (!range.empty()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"Range", range}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, path, "text/plain");
        // Copy out before the request arena unwinds.
        return std::pair<ruvia::HttpStatusCode, std::string>(response.status(), std::string(response.header("Content-Range").value_or("")));
    };
    const auto serveFile = [&serveMethod](std::string_view path, std::string_view range) { return serveMethod("GET", path, range); };
    const auto serve = [&serveFile](std::string_view range) { return serveFile("data.txt", range); };

    // A valid single range -> 206 with the byte range echoed.
    const auto ok = serve("bytes=0-4");
    RUVIA_CHECK_EQ(ok.first, ruvia::http_status::kPartialContent);
    RUVIA_CHECK_EQ(ok.second, std::string("bytes 0-4/100"));

    // Multiple ranges are not supported, so the whole file is served (RFC 7233).
    const auto multi = serve("bytes=0-9,20-29");
    RUVIA_CHECK_EQ(multi.first, ruvia::http_status::kOk);

    // A wholly unsatisfiable range -> 416 with "bytes */size".
    const auto bad = serve("bytes=1000-2000");
    RUVIA_CHECK_EQ(bad.first, ruvia::http_status::kRangeNotSatisfiable);
    RUVIA_CHECK_EQ(bad.second, std::string("bytes */100"));

    // An unknown range unit MUST be ignored (RFC 9110 §14.2) -> full 200, not 416.
    const auto unknownUnit = serve("items=0-9");
    RUVIA_CHECK_EQ(unknownUnit.first, ruvia::http_status::kOk);

    // A syntactically malformed byte range is likewise ignored -> full 200.
    const auto malformed = serve("bytes=abc");
    RUVIA_CHECK_EQ(malformed.first, ruvia::http_status::kOk);

    // Range units are case-insensitive (RFC 9110 §14.1); this is still a 206.
    const auto caseInsensitiveUnit = serve("Bytes=5-9");
    RUVIA_CHECK_EQ(caseInsensitiveUnit.first, ruvia::http_status::kPartialContent);
    RUVIA_CHECK_EQ(caseInsensitiveUnit.second, std::string("bytes 5-9/100"));

    // RFC 9110 §14.2 defines Range handling only for GET. A HEAD request
    // carrying the same field must describe the full representation with 200,
    // not invent a partial 206 response with Content-Range metadata.
    const auto head = serveMethod("HEAD", "data.txt", "bytes=0-4");
    RUVIA_CHECK_EQ(head.first, ruvia::http_status::kOk);
    RUVIA_CHECK(head.second.empty());

    // This server uses RFC 9110 §14.2's permitted ignore policy for a selected
    // representation with no content, avoiding an invalid zero-length 206 range.
    const auto empty = serveFile("empty.txt", "bytes=0-0");
    RUVIA_CHECK_EQ(empty.first, ruvia::http_status::kOk);
    RUVIA_CHECK(empty.second.empty());

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_resolves_percent_encoded_name_and_stays_traversal_safe) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_pct_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "my report.txt", std::ios::binary | std::ios::trunc);
        const std::string content(20, 'z');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view path) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        ruvia::HttpStatusCode status = ruvia::http_status::kInternalServerError;
        try {
            status = context.staticFile(root, path, "text/plain").status();
        } catch (const ruvia::HttpError& error) {
            status = error.info().status();
        }
        return status;
    };

    // "%20" resolves to the space in the real on-disk name (RFC 3986 percent
    // equivalence). Before decoding this 404'd: the raw bytes "my%20report.txt"
    // were compared against the decoded index key "my report.txt".
    RUVIA_CHECK_EQ(serve("my%20report.txt"), ruvia::http_status::kOk);

    // Decoding must not open a traversal hole: "%2e%2e%2f" -> "../" is still
    // clamped at the root (403), and encoded separators plus dot-segments cannot
    // ascend past it either.
    RUVIA_CHECK_EQ(serve("%2e%2e%2fetc%2fpasswd"), ruvia::http_status::kForbidden);
    RUVIA_CHECK_EQ(serve("sub%2f%2e%2e%2f%2e%2e%2fetc"), ruvia::http_status::kForbidden);

    // A decoded NUL ("%00") cannot occur in a filename and is rejected outright.
    RUVIA_CHECK_EQ(serve("my%00report.txt"), ruvia::http_status::kForbidden);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_declares_vary_accept_encoding_but_context_file_does_not) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_vary_dir";
    fs::create_directories(dir);
    const auto filePath = dir / "app.js";
    {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        const std::string content(50, 'x');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    // No sidecar and no Accept-Encoding -> the identity file is served, but it must
    // STILL declare Vary: Accept-Encoding: the same URL would serve a compressed
    // variant to a capable client, so a shared cache keyed only on the URL must not
    // reuse this identity body for everyone (RFC 9110 12.5.5 / RFC 9111 4.1). The
    // identity body carries no Content-Encoding.
    const auto served = context.staticFile(root, "app.js", "text/javascript");
    RUVIA_CHECK_EQ(served.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(served.header("Vary").value_or("").find("Accept-Encoding") != std::string_view::npos);
    RUVIA_CHECK(!served.header("Content-Encoding").has_value());

    // Context::file serves a single path with no encoding negotiation, so it must
    // NOT declare Vary: Accept-Encoding (which would needlessly fragment caches).
    const auto direct = context.file(filePath);
    RUVIA_CHECK_EQ(direct.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(!direct.header("Vary").has_value());
    RUVIA_CHECK_EQ(direct.header("Content-Type").value_or(""), std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK(ruvia::detail::responseBody(direct).ownedFile() != nullptr);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_preserves_context_vary_when_adding_accept_encoding) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_vary_merge_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "app.js", std::ios::binary | std::ios::trunc);
        out << "console.log('ok');";
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);
    context.header("Vary", "Origin");

    const auto response = context.staticFile(root, "app.js", "text/javascript");
    const auto vary = response.header("Vary").value_or("");
    // Context response metadata is applied after the file's base headers. It
    // must not erase the negotiation dimension, or a shared cache can reuse an
    // identity/encoded representation for the wrong Accept-Encoding request.
    RUVIA_CHECK(ruvia::detail::httpHasToken(vary, "Origin"));
    RUVIA_CHECK(ruvia::detail::httpHasToken(vary, "Accept-Encoding"));

    fs::remove_all(dir);
}

RUVIA_TEST(sse_stream_head_defaults_cache_control_but_honors_a_caller_value) {
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::prepareResponseStreamHead;
    using ruvia::detail::ResponseStreamFraming;
    using ruvia::detail::ResponseStreamKind;
    using ruvia::detail::ResponseTrailerIntent;

    const auto head = [](bool presetNoCache) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        if (presetNoCache) {
            ContextAccess::setResponseHeader(context, "Cache-Control", "no-cache");
        }
        auto response = ContextAccess::streamingHead(context);
        auto streamHead = prepareResponseStreamHead(std::move(response), ResponseStreamKind::kSse, ruvia::detail::httpResponseStreamCommitPlan(ResponseStreamFraming::kHttp1Chunked, HttpKnownMethod::kGet, ruvia::http_status::kOk, ResponseTrailerIntent::kNone));
        return std::string(streamHead.response().header("Cache-Control").value_or(""));
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
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_if_range_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    // Serve with a Range plus an optional If-Range; returns (status, Last-Modified).
    const auto serve = [&root](std::optional<std::string_view> ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Range", "bytes=0-4"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (ifRange.has_value()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"If-Range", *ifRange}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto ctx = ContextAccess::make(memory, request);
        const auto response = ctx.staticFile(root, "data.txt", "text/plain");
        return std::pair<ruvia::HttpStatusCode, std::string>(response.status(), std::string(response.header("Last-Modified").value_or("")));
    };

    // Discover the representation's current Last-Modified via a bare range request.
    const auto base = serve(std::nullopt);
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::kPartialContent);
    RUVIA_CHECK(!base.second.empty());
    const auto lastModified = ruvia::detail::httpParseHttpDate(base.second);
    RUVIA_CHECK(lastModified.has_value());
    const std::time_t modified = lastModified.value_or(0);

    const auto fmt = [](std::time_t t) {
        const auto out = ruvia::detail::httpFormatDate(std::pmr::get_default_resource(), t);
        return std::string(out.data(), out.size());
    };

    // Exact match -> the representation is unchanged, so the range is honored (206).
    RUVIA_CHECK_EQ(serve(fmt(modified)).first, ruvia::http_status::kPartialContent);

    // A present empty If-Range is not an entity-tag or HTTP-date. Its condition
    // is therefore false, so it must suppress the Range rather than being
    // confused with an absent field and producing a partial response.
    RUVIA_CHECK_EQ(serve(std::string_view{}).first, ruvia::http_status::kOk);

    // If-Range date NEWER than Last-Modified: the file's mtime is older, so it is a
    // DIFFERENT representation than the client holds. RFC 9110 §13.1.5 requires an
    // exact match, so the range MUST be refused and the full 200 served. (The old
    // "<=" comparison wrongly returned 206 here -- the corruption path.)
    RUVIA_CHECK_EQ(serve(fmt(modified + 86400)).first, ruvia::http_status::kOk);

    // If-Range date OLDER than Last-Modified: representation has since changed -> 200.
    RUVIA_CHECK_EQ(serve(fmt(modified - 86400)).first, ruvia::http_status::kOk);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_clamps_future_last_modified_and_rejects_it_for_if_range) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

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

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::optional<std::string_view> ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Range", "bytes=0-1"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (ifRange.has_value()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"If-Range", *ifRange}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<ruvia::HttpStatusCode, std::string>(response.status(), std::string(response.header("Last-Modified").value_or("")));
    };

    const auto before = std::time(nullptr);
    const auto base = serve(std::nullopt);
    const auto after = std::time(nullptr);
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::kPartialContent);
    const auto lastModified = ruvia::detail::httpParseHttpDate(base.second);
    RUVIA_CHECK(lastModified.has_value());
    RUVIA_CHECK(lastModified.value_or(after + 1) >= before);
    RUVIA_CHECK(lastModified.value_or(after + 1) <= after);

    // The clamped wire date is the response time, not the file's actual
    // validator. It is therefore weak and cannot authorize stitching a partial
    // response into the client's stored representation.
    RUVIA_CHECK_EQ(serve(base.second).first, ruvia::http_status::kOk);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_without_response_validators_still_enforces_preconditions) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ifrange_novalidator_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    options.enableRanges = true;
    options.enableValidators = false;  // no ETag / Last-Modified on responses
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifRange) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Range", "bytes=0-4"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kRange));
        if (!ifRange.empty()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"If-Range", ifRange}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfRange));
        }
        auto ctx = ContextAccess::make(memory, request);
        return ctx.staticFile(root, "data.txt", "text/plain").status();
    };

    // A plain range with no If-Range is still honored without validators -> 206.
    RUVIA_CHECK_EQ(serve(""), ruvia::http_status::kPartialContent);
    // A range WITH If-Range but no server validator cannot be confirmed, so the
    // Range MUST be ignored and the full representation served (RFC 9110 13.1.5) --
    // not a 206 stitched from bytes the client cannot verify it still holds.
    // (Gating the If-Range check on enableValidators skipped it and returned 206.)
    RUVIA_CHECK_EQ(serve("\"stale-etag\""), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(serve("Wed, 21 Oct 2015 07:28:00 GMT"), ruvia::http_status::kOk);

    struct ConditionalResult final {
        ruvia::HttpStatusCode status;
        bool hasEtag;
        bool hasLastModified;
    };
    const auto serveConditional = [&root](std::string_view method, std::optional<RequestKnownHeader> slot, std::string_view name, std::string_view value) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setResource(request, memory.resource());
        if (slot.has_value()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{name, value}, HttpRequestAccess::knownHeaderSlot(*slot));
        }
        auto context = ContextAccess::make(memory, request);
        try {
            const auto response = context.staticFile(root, "data.txt", "text/plain");
            return ConditionalResult{response.status(), response.header("ETag").has_value(), response.header("Last-Modified").has_value()};
        } catch (const ruvia::HttpError& error) {
            return ConditionalResult{error.info().status(), false, false};
        }
    };

    const auto plain = serveConditional("GET", std::nullopt, {}, {});
    RUVIA_CHECK_EQ(plain.status, ruvia::http_status::kOk);
    RUVIA_CHECK(!plain.hasEtag);
    RUVIA_CHECK(!plain.hasLastModified);

    // Disabling response validator fields does not disable request
    // preconditions. Wildcard conditions test whether a current representation
    // exists and therefore require no ETag at all; date conditions use the
    // origin's file metadata even when Last-Modified is not emitted.
    const auto getExisting = serveConditional("GET", RequestKnownHeader::kIfNoneMatch, "If-None-Match", "*");
    RUVIA_CHECK_EQ(getExisting.status, ruvia::http_status::kNotModified);
    RUVIA_CHECK(!getExisting.hasEtag);
    RUVIA_CHECK(!getExisting.hasLastModified);
    RUVIA_CHECK_EQ(serveConditional("POST", RequestKnownHeader::kIfNoneMatch, "If-None-Match", "*").status, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveConditional("POST", RequestKnownHeader::kIfMatch, "If-Match", "*").status, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(serveConditional("POST", RequestKnownHeader::kIfMatch, "If-Match", "\"stale\"").status, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveConditional("POST", RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT").status, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveConditional("GET", RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", "Fri, 31 Dec 9999 23:59:59 GMT").status, ruvia::http_status::kNotModified);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_match_takes_precedence_over_if_unmodified_since) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_precedence_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    struct Header {
        RequestKnownHeader slot;
        std::string_view name;
        std::string_view value;
    };
    const auto serveMethod = [&root](std::string_view method, std::initializer_list<Header> headers) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, method);
        HttpRequestAccess::setResource(request, memory.resource());
        for (const auto& header : headers) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{header.name, header.value}, HttpRequestAccess::knownHeaderSlot(header.slot));
        }
        auto context = ContextAccess::make(memory, request);
        ruvia::HttpStatusCode status = ruvia::http_status::kInternalServerError;
        std::string etag;
        try {
            const auto response = context.staticFile(root, "data.txt", "text/plain");
            status = response.status();
            etag.assign(response.header("ETag").value_or(""));
        } catch (const ruvia::HttpError& error) {
            status = error.info().status();
        }
        return std::pair<ruvia::HttpStatusCode, std::string>(status, std::move(etag));
    };
    const auto serve = [&serveMethod](std::initializer_list<Header> headers) { return serveMethod("GET", headers); };

    // Discover the current strong ETag with a bare request.
    const auto base = serve({});
    RUVIA_CHECK_EQ(base.first, ruvia::http_status::kOk);
    const std::string etag = base.second;
    RUVIA_CHECK(!etag.empty());
    // A date well before the file's mtime -> If-Unmodified-Since fails on its own.
    constexpr std::string_view kOldDate = "Thu, 01 Jan 1970 00:00:00 GMT";

    // If-Unmodified-Since alone (stale date) is a 412 precondition failure.
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", kOldDate}}).first, ruvia::http_status::kPreconditionFailed);

    // With a matching If-Match present, RFC 9110 §13.2.2 requires If-Unmodified-Since
    // to be ignored -- the strong validator matched, so serve 200 rather than 412.
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfMatch, "If-Match", etag}, {RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", kOldDate}}).first, ruvia::http_status::kOk);

    // Presence is distinct from a non-empty field value. The empty #entity-tag
    // list matches no current representation, so an empty If-Match fails rather
    // than being treated as if the field were absent.
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfMatch, "If-Match", ""}}).first, ruvia::http_status::kPreconditionFailed);

    constexpr std::string_view kFutureDate = "Fri, 31 Dec 9999 23:59:59 GMT";
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", kFutureDate}}).first, ruvia::http_status::kNotModified);
    // Even an empty If-None-Match is present and therefore takes precedence over
    // If-Modified-Since. Its empty list does not match, so the response is 200.
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfNoneMatch, "If-None-Match", ""}, {RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", kFutureDate}}).first, ruvia::http_status::kOk);

    // If-Match and If-None-Match are list fields. Repeated field lines are
    // equivalent to one comma-joined value, so a match on the first line must
    // not be lost when the known-header cache records the second line.
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfMatch, "If-Match", etag}, {RequestKnownHeader::kIfMatch, "If-Match", "\"stale\""}}).first, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(serve({{RequestKnownHeader::kIfNoneMatch, "If-None-Match", etag}, {RequestKnownHeader::kIfNoneMatch, "If-None-Match", "\"stale\""}}).first, ruvia::http_status::kNotModified);

    // Preconditions protect unsafe methods too. A matching If-None-Match or a
    // stale If-Match / If-Unmodified-Since on POST must fail with 412 instead of
    // serving the file as an unconditional 200.
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfNoneMatch, "If-None-Match", etag}}).first, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfMatch, "If-Match", "\"stale\""}}).first, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfUnmodifiedSince, "If-Unmodified-Since", kOldDate}}).first, ruvia::http_status::kPreconditionFailed);
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfMatch, "If-Match", etag}}).first, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfNoneMatch, "If-None-Match", "\"stale\""}}).first, ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(serveMethod("POST", {{RequestKnownHeader::kIfModifiedSince, "If-Modified-Since", kFutureDate}}).first, ruvia::http_status::kOk);

    // OPTIONS does not select or modify a representation, so RFC 9110 §13.2.1
    // requires these conditional fields to be ignored for that method.
    RUVIA_CHECK_EQ(serveMethod("OPTIONS", {{RequestKnownHeader::kIfMatch, "If-Match", "\"stale\""}}).first, ruvia::http_status::kOk);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_conditional_request_serving) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_conditional_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(100, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](ruvia::detail::RequestKnownHeader slot, std::string_view headerName, std::string_view headerValue) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!headerName.empty()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{headerName, headerValue}, HttpRequestAccess::knownHeaderSlot(slot));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, "data.txt", "text/plain");
        return std::pair<ruvia::HttpStatusCode, std::string>(response.status(), std::string(response.header("ETag").value_or("")));
    };

    // An unconditional GET yields 200 and a strong ETag validator.
    const auto plain = serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "", "");
    RUVIA_CHECK_EQ(plain.first, ruvia::http_status::kOk);
    RUVIA_CHECK(!plain.second.empty());
    const std::string etag = plain.second;

    // If-None-Match with the current ETag -> 304; a stale one falls through to 200.
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", etag).first, ruvia::http_status::kNotModified);
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", "\"stale\"").first, ruvia::http_status::kOk);

    // A comma inside an opaque tag is data, not a list separator. This malformed
    // value closes that tag immediately before the current ETag and must not let
    // the apparent suffix satisfy the condition.
    const std::string malformedList = std::string("\"stale, ") + etag;
    RUVIA_CHECK_EQ(serve(ruvia::detail::RequestKnownHeader::kIfNoneMatch, "If-None-Match", malformedList).first, ruvia::http_status::kOk);

    // If-Match against a non-matching ETag is a 412 precondition failure (thrown).
    bool precondition = false;
    try {
        (void)serve(ruvia::detail::RequestKnownHeader::kIfMatch, "If-Match", "\"stale\"");
    } catch (const ruvia::HttpError& error) {
        precondition = error.info().status() == ruvia::http_status::kPreconditionFailed;
    }
    RUVIA_CHECK(precondition);

    precondition = false;
    try {
        (void)serve(ruvia::detail::RequestKnownHeader::kIfMatch, "If-Match", malformedList);
    } catch (const ruvia::HttpError& error) {
        precondition = error.info().status() == ruvia::http_status::kPreconditionFailed;
    }
    RUVIA_CHECK(precondition);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_selects_precompressed_representation_atomically) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

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
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    struct ServedRepresentation final {
        std::string contentEncoding;
        std::string vary;
        std::uint64_t size{0};
    };
    const auto serve = [&root](std::string_view relative, std::string_view acceptEncoding) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!acceptEncoding.empty()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept-Encoding", acceptEncoding}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
        }
        auto context = ContextAccess::make(memory, request);
        const auto response = context.staticFile(root, relative, "text/plain");
        const auto file = ruvia::detail::responseBody(response).file();
        return ServedRepresentation{.contentEncoding = std::string(response.header("Content-Encoding").value_or("")), .vary = std::string(response.header("Vary").value_or("")), .size = file.has_value() ? file->length() : 0};
    };

    // Accept-Encoding: gzip with a .gz sidecar present serves the gzip variant,
    // marked Content-Encoding: gzip and Vary: Accept-Encoding so a cache keys on it.
    const auto gz = serve("data.txt", "gzip");
    RUVIA_CHECK_EQ(gz.contentEncoding, std::string("gzip"));
    RUVIA_CHECK_EQ(gz.size, std::uint64_t{20});
    RUVIA_CHECK(gz.vary.find("Accept-Encoding") != std::string_view::npos);

    const auto br = serve("data.txt", "br");
    RUVIA_CHECK_EQ(br.contentEncoding, std::string("br"));
    RUVIA_CHECK_EQ(br.size, std::uint64_t{30});

    const auto zstd = serve("data.txt", "zstd");
    RUVIA_CHECK_EQ(zstd.contentEncoding, std::string("zstd"));
    RUVIA_CHECK_EQ(zstd.size, std::uint64_t{40});

    // Equal quality uses the one canonical server preference order.
    const auto tied = serve("data.txt", "gzip, zstd, br");
    RUVIA_CHECK_EQ(tied.contentEncoding, std::string("br"));
    RUVIA_CHECK_EQ(tied.size, std::uint64_t{30});

    const auto prefersGzip = serve("data.txt", "gzip;q=1, br;q=0.5, zstd;q=0.25");
    RUVIA_CHECK_EQ(prefersGzip.contentEncoding, std::string("gzip"));
    RUVIA_CHECK_EQ(prefersGzip.size, std::uint64_t{20});

    // identity is implicitly q=1. A lower-quality gzip preference must leave
    // the original representation selected even when a sidecar exists.
    const auto prefersIdentity = serve("data.txt", "gzip;q=0.5");
    RUVIA_CHECK(prefersIdentity.contentEncoding.empty());
    RUVIA_CHECK_EQ(prefersIdentity.size, std::uint64_t{100});

    // Without Accept-Encoding the plain file is served, with no Content-Encoding.
    const auto plain = serve("data.txt", "");
    RUVIA_CHECK(plain.contentEncoding.empty());
    RUVIA_CHECK_EQ(plain.size, std::uint64_t{100});

    // When the best client preference is unavailable on disk, the selector
    // must choose the best existing sidecar rather than reimplementing q-value
    // ranking in the static-file layer.
    const auto missingBrotli = serve("gzip-only.txt", "br, gzip, identity;q=0");
    RUVIA_CHECK_EQ(missingBrotli.contentEncoding, std::string("gzip"));
    RUVIA_CHECK_EQ(missingBrotli.size, std::uint64_t{4});

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_rejects_a_stale_precompressed_sidecar) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

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
    const auto identityTime = fs::last_write_time(dir / "data.txt", ec);
    RUVIA_CHECK(!ec);
    fs::last_write_time(dir / "data.txt.gz", identityTime - std::chrono::seconds(10), ec);
    RUVIA_CHECK(!ec);

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept-Encoding", "gzip"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
    auto context = ContextAccess::make(memory, request);
    const auto response = context.staticFile(root, "data.txt", "text/plain");
    const auto file = ruvia::detail::responseBody(response).file();

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
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

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
    StaticRoot root(dir);

    const auto serve = [](const StaticRoot& selectedRoot, std::string_view path, std::string_view acceptEncoding) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        if (!acceptEncoding.empty()) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept-Encoding", acceptEncoding}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
        }
        auto context = ContextAccess::make(memory, request);
        try {
            const auto response = context.staticFile(selectedRoot, path);
            const auto file = ruvia::detail::responseBody(response).file();
            return std::tuple{response.status(), std::string(response.header("Content-Encoding").value_or("")), file.has_value() ? file->length() : std::uint64_t{0}};
        } catch (const ruvia::HttpError& error) {
            return std::tuple{error.info().status(), std::string{}, std::uint64_t{0}};
        }
    };

    const auto negotiated = serve(root, "app.js", "gzip");
    RUVIA_CHECK_EQ(std::get<0>(negotiated), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(std::get<1>(negotiated), std::string("gzip"));
    RUVIA_CHECK_EQ(std::get<2>(negotiated), std::uint64_t{4});

    // Directly requesting the internal sidecar must follow the same extension
    // allow-list as every other public path. Before this guard, it returned the
    // raw compressed bytes as an identity application/octet-stream response.
    RUVIA_CHECK_EQ(std::get<0>(serve(root, "app.js.gz", "")), ruvia::http_status::kNotFound);

    // A policy that explicitly allows .gz still exposes it as a normal file;
    // only entries admitted solely because their base type is allowed are
    // internal-only.
    ruvia::StaticRootOptions gzipOptions;
    gzipOptions.fileTypes = ruvia::StaticFileTypePolicy::only({"gz"});
    StaticRoot gzipRoot(dir, std::move(gzipOptions));
    RUVIA_CHECK_EQ(std::get<0>(serve(gzipRoot, "app.js.gz", "")), ruvia::http_status::kOk);

    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_empty_custom_mime_type) {
    namespace fs = std::filesystem;
    using ruvia::StaticMimeType;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;

    const auto dir = fs::temp_directory_path() / "ruvia_static_empty_mime_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.custom", std::ios::binary | std::ios::trunc);
        out << "content";
    }

    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticMimeType mime;
    mime.extension = ".custom";
    options.mimeTypes.push_back(std::move(mime));

    bool rejected = false;
    try {
        StaticRoot root(dir, std::move(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    fs::remove_all(dir);
}

RUVIA_TEST(static_root_rejects_invalid_static_header_options_at_construction) {
    namespace fs = std::filesystem;
    using ruvia::StaticMimeType;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;

    const auto dir = fs::temp_directory_path() / "ruvia_static_invalid_header_options_dir";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "data.custom") << "content";

    const auto rejects = [&dir, &ruvia_ctx](StaticRootOptions options) {
        bool rejected = false;
        try {
            StaticRoot root(dir, std::move(options));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    };

    {
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        options.cacheControl = " private";
        rejects(std::move(options));
    }
    {
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        options.defaultContentType = "text plain";
        rejects(std::move(options));
    }
    {
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        StaticMimeType mime;
        mime.extension = ".custom";
        mime.contentType = "text plain";
        options.mimeTypes.push_back(std::move(mime));
        rejects(std::move(options));
    }
    for (const std::string_view invalidExtension : {"", ".", "..", "nested/custom", "nested\\custom"}) {
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        StaticMimeType mime;
        mime.extension = std::string(invalidExtension);
        mime.contentType = "text/plain";
        options.mimeTypes.push_back(std::move(mime));
        rejects(std::move(options));
    }

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_rejects_an_empty_accept_encoding_set) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_no_acceptable_coding_dir";
    fs::create_directories(dir);
    std::ofstream(dir / "data.txt") << "content";
    std::ofstream(dir / "data.txt.gz") << "compressed-content";
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "GET");
    HttpRequestAccess::setTarget(request, "/data.txt");
    HttpRequestAccess::setPath(request, "/data.txt");
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept-Encoding", "identity;q=0, *;q=0"}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAcceptEncoding));
    auto context = ContextAccess::make(memory, request);

    bool rejected = false;
    try {
        static_cast<void>(context.staticFile(root, "data.txt", "text/plain"));
    } catch (const ruvia::HttpError& error) {
        rejected = error.info().status() == ruvia::http_status::kNotAcceptable;
    }
    RUVIA_CHECK(rejected);

    asio::io_context io;
    ruvia::detail::RouteTable routes(memory.resource());
    const auto resolution = routes.resolve(request);
    auto routedResult = runStaticCompressionTask(io, routes.dispatchBufferedResponse(request, resolution, memory, ruvia::detail::DocumentRootBinding::standalone(root)));
    RUVIA_CHECK(routedResult.application() != nullptr);
    auto routed = std::move(routedResult).takeResponse();
    RUVIA_CHECK_EQ(routed.status(), ruvia::http_status::kNotAcceptable);
    fs::remove_all(dir);
}

RUVIA_TEST(static_file_if_modified_since_serving) {
    namespace fs = std::filesystem;
    using ruvia::HttpHeaderView;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;
    using ruvia::detail::RequestKnownHeader;

    const auto dir = fs::temp_directory_path() / "ruvia_static_ims_dir";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "data.txt", std::ios::binary | std::ios::trunc);
        const std::string content(50, 'a');
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    StaticRoot root(dir, std::move(options));

    const auto serve = [&root](std::string_view ifModifiedSince) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        HttpRequestAccess::addHeader(request, HttpHeaderView{"If-Modified-Since", ifModifiedSince}, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kIfModifiedSince));
        auto context = ContextAccess::make(memory, request);
        return context.staticFile(root, "data.txt", "text/plain").status();
    };

    // The file was just written, so an If-Modified-Since far in the future means
    // "not modified since then" -> 304; one far in the past means it HAS changed
    // -> 200.
    RUVIA_CHECK_EQ(serve("Fri, 01 Jan 2100 00:00:00 GMT"), ruvia::http_status::kNotModified);
    RUVIA_CHECK_EQ(serve("Sat, 01 Jan 2000 00:00:00 GMT"), ruvia::http_status::kOk);

    fs::remove_all(dir);
}

RUVIA_TEST(static_file_directory_root_index_and_403) {
    namespace fs = std::filesystem;
    using ruvia::StaticRoot;
    using ruvia::StaticRootOptions;
    using ruvia::detail::ContextAccess;
    using ruvia::detail::HttpRequestAccess;

    const auto dir = fs::temp_directory_path() / "ruvia_static_dir_index";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "other.txt", std::ios::binary | std::ios::trunc);
        out << "x";
    }

    const auto serveRoot = [](StaticRoot& root) -> ruvia::HttpStatusCode {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        auto context = ContextAccess::make(memory, request);
        try {
            return context.staticFile(root, "", "text/html").status();
        } catch (const ruvia::HttpError& error) {
            return error.info().status();
        }
    };

    // A directory root with no configured index is forbidden (never a listing).
    {
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), ruvia::http_status::kForbidden);
    }

    // With an index file configured (and present), the directory root serves it.
    {
        std::ofstream out(dir / "index.html", std::ios::binary | std::ios::trunc);
        out << "<h1>i</h1>";
        out.close();
        StaticRootOptions options;
        options.fileTypes = ruvia::StaticFileTypePolicy::all();
        options.indexFile = "index.html";
        StaticRoot root(dir, std::move(options));
        RUVIA_CHECK_EQ(serveRoot(root), ruvia::http_status::kOk);
    }

    fs::remove_all(dir);
}
