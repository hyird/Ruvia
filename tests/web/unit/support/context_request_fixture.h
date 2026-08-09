#pragma once

#include "test_io_context.h"
#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/Error.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace context_request_test {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpKnownMethod;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;

inline asio::awaitable<void> readHeaderValue(ruvia::Context& context, std::string& output) {
    const auto value = context.req().header("X-Trace");
    if (value) {
        output.assign(value->data(), value->size());
    }
    co_return;
}

struct MethodObservation final {
    std::string method;
    HttpKnownMethod knownMethod{HttpKnownMethod::kGet};
};

inline ruvia::Task<ruvia::ContextRequest::RequestFormData> parseRequestBody(ruvia::Context& context, ruvia::ContextRequest::ParseBodyOptions options) {
    co_return co_await context.req().parseBody(options);
}

inline asio::awaitable<void> readMethod(ruvia::Context& context, MethodObservation& observation) {
    const auto request = context.req();
    observation.method.assign(request.method().data(), request.method().size());
    observation.knownMethod = request.knownMethod();
    co_return;
}

inline asio::awaitable<void> parseProtoBody(ruvia::Context& context, bool& safeOk, bool& protoDropped) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {
                                                                                            .dottedNames = ruvia::ContextRequest::DottedNamePolicy::kExpandPath,
                                                                                        }));
    const auto safe = form.get("safe").value();
    safeOk = safe.has_value() && *safe == std::string_view("ok");
    protoDropped = !static_cast<bool>(form.get("__proto__"));
}

inline asio::awaitable<void> parseArrayForm(ruvia::Context& context, std::size_t& tagsSize, bool& tagsArray, std::size_t& xSize, std::string& xValue) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
    const auto tags = form.get("tags[]");
    tagsSize = tags.size();
    tagsArray = tags.isArray();
    const auto x = form.get("x");
    xSize = x.size();
    if (const auto xv = x.value(); xv.has_value()) {
        xValue.assign(xv->data(), xv->size());
    }
}

inline asio::awaitable<void> parseRepeatedFiles(ruvia::Context& context, std::size_t& count, std::size_t& fileCount, bool& sawA, bool& sawB) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
    const auto photos = form.get("photos");
    count = photos.size();
    for (const auto* field : photos.fields()) {
        if (field == nullptr || !field->isFile()) {
            continue;
        }
        ++fileCount;
        const auto name = field->filename();
        if (name == std::string_view("a.txt")) {
            sawA = true;
        } else if (name == std::string_view("b.txt")) {
            sawB = true;
        }
    }
}

inline asio::awaitable<void> parsePartContentType(ruvia::Context& context, std::string& contentType) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
    const auto upload = form.get("upload");
    if (const auto* field = upload.field(); field != nullptr) {
        const auto value = field->contentType();
        contentType.assign(value.data(), value.size());
    }
}

inline asio::awaitable<void> parseWithFieldCap(ruvia::Context& context, std::size_t maxFields, bool& rejected, int& status) {
    try {
        (void)co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {.maxFields = maxFields}));
    } catch (const ruvia::HttpError& error) {
        rejected = true;
        status = error.info().status().value();
    }
}

inline asio::awaitable<void> parseAllRepeatedScalar(ruvia::Context& context, std::size_t& valueCount, std::string& selectedValue) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {
                                                                                            .repeatedScalars = ruvia::ContextRequest::RepeatedScalarPolicy::kRetainAll,
                                                                                        }));
    const auto value = form.get("x");
    valueCount = value.size();
    if (const auto selected = value.value()) {
        selectedValue.assign(selected->data(), selected->size());
    }
}

inline asio::awaitable<void> parseMultipart(ruvia::Context& context, std::string& nameValue, std::string& fileName, std::string& fileType, std::string& fileData) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
    if (const auto nv = form.get("name").value(); nv.has_value()) {
        nameValue.assign(nv->data(), nv->size());
    }
    const auto file = form.get("file");
    if (const auto* f = file.field(); f != nullptr) {
        fileName.assign(f->filename().data(), f->filename().size());
    }
    if (const auto b = file.blob(); b.has_value()) {
        fileType.assign(b->contentType().data(), b->contentType().size());
        fileData.assign(b->text().data(), b->text().size());
    }
}

inline asio::awaitable<void> parseBodyDiscard(ruvia::Context& context) {
    (void)co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
}

inline asio::awaitable<void> parseScalarPair(ruvia::Context& context, std::string& aValue, bool& aPresent, std::string& bValue, bool& bPresent) {
    const auto form = co_await ruvia::detail::taskAsAwaitable(parseRequestBody(context, {}));
    if (const auto v = form.get("a").value(); v.has_value()) {
        aValue.assign(v->data(), v->size());
        aPresent = true;
    }
    if (const auto v = form.get("b").value(); v.has_value()) {
        bValue.assign(v->data(), v->size());
        bPresent = true;
    }
}

}  // namespace context_request_test

// A prefixed signed cookie reaches the client under its wire name
// ("__Host-session"), so that is the only name the read side can pass back.
// The signature therefore has to bind the wire name: signing the bare name
// made every prefixed signed cookie unverifiable.

// Request observation stays on req(); deleteCookie only queues the response
// mutation, including the configured wire-name prefix.

using namespace context_request_test;  // NOLINT(google-build-using-namespace)
