#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

namespace {

void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
    char buffer[32]{};
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
}

}  // namespace

class ApiSurfaceController final : public ruvia::Controller<ApiSurfaceController> {
public:
    RUVIA_CONTROLLER_GROUP("/surface")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/request", requestInfo);
    RUVIA_POST("/multipart", bufferedMultipart);
    RUVIA_POST("/discard", discard);
    RUVIA_PUT("/items/:id", replaceItem);
    RUVIA_PATCH("/items/:id", patchItem);
    RUVIA_DELETE("/items/:id", deleteItem);
    RUVIA_GET("/cookies", cookies);
    RUVIA_GET("/manual/copy", manualCopy);
    RUVIA_GET("/manual/view", manualView);
    RUVIA_PUT_STREAM("/upload/:id", streamPut);
    RUVIA_PATCH_STREAM("/upload/:id", streamPatch);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> requestInfo(ruvia::Context& c) {
        const auto& request = c.req();
        std::pmr::string body(c.allocator<char>());
        body.append("method=");
        body.append(ruvia::methodName(request.method()));
        body.append("\ntarget=");
        body.append(request.target());
        body.append("\npath=");
        body.append(request.path());
        body.append("\nquery=");
        body.append(request.queryString());
        body.append("\nversion=");
        body.append(request.httpVersion());
        body.append("\naccepts-json=");
        body.append(c.accepts("application/json") ? "true" : "false");
        body.append("\ndecoded-path=");
        if (auto decoded = c.decodedPath()) {
            body.append(*decoded);
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> bufferedMultipart(ruvia::Context& c) {
        auto parts = co_await c.multipart();
        std::pmr::string body(c.allocator<char>());
        body.append("parts=");
        appendUnsigned(body, parts.size());
        for (const auto& part : parts) {
            body.append("\nname=");
            body.append(part.name);
            body.append(";filename=");
            body.append(part.filename);
            body.append(";content-type=");
            body.append(part.contentType);
            body.append(";bytes=");
            appendUnsigned(body, part.body.size());
        }
        body.push_back('\n');
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> discard(ruvia::Context& c) {
        co_await c.discardBody();
        co_return c.status(204).text("");
    }

    ruvia::Task<ruvia::HttpResponse> replaceItem(ruvia::Context& c) {
        const auto body = co_await c.body();
        std::pmr::string output(c.allocator<char>());
        output.append("replace id=");
        output.append(c.param("id").toStringView().value_or(""));
        output.append(" bytes=");
        appendUnsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> patchItem(ruvia::Context& c) {
        const auto body = co_await c.body();
        std::pmr::string output(c.allocator<char>());
        output.append("patch id=");
        output.append(c.param("id").toStringView().value_or(""));
        output.append(" bytes=");
        appendUnsigned(output, body.size());
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> deleteItem(ruvia::Context& c) {
        std::pmr::string output(c.allocator<char>());
        output.append("deleted id=");
        output.append(c.param("id").toStringView().value_or(""));
        output.push_back('\n');
        co_return c.text(output);
    }

    ruvia::Task<ruvia::HttpResponse> cookies(ruvia::Context& c) {
        ruvia::CookieOptions options;
        options.httpOnly = true;
        options.sameSite = "Lax";
        options.maxAge = 3600;
        co_return c
            .setCookie("session", "example", options)
            .setCookie("theme", "light")
            .text("cookies set\n");
    }

    ruvia::Task<ruvia::HttpResponse> manualCopy(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setStatus(202, "Accepted");
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        response.setHeader("X-Manual-Body", "copy");
        response.setBodyCopy(std::string_view("copied body\n"));
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> manualView(ruvia::Context& c) {
        ruvia::HttpResponse response(c.resource());
        response.setHeader("Content-Type", "text/plain; charset=utf-8");
        response.setHeader("X-Manual-Body", "view");
        response.setBodyView("borrowed static view\n");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> streamPut(ruvia::Context& c) {
        co_return co_await countStreamingBody(c, "put");
    }

    ruvia::Task<ruvia::HttpResponse> streamPatch(ruvia::Context& c) {
        co_return co_await countStreamingBody(c, "patch");
    }

    static ruvia::Task<ruvia::HttpResponse> countStreamingBody(ruvia::Context& c, std::string_view verb) {
        std::uint64_t bytes = 0;
        auto& reader = c.bodyReader();
        while (auto chunk = co_await reader.read()) {
            bytes += chunk->size();
        }

        std::pmr::string body(c.allocator<char>());
        body.append(verb);
        body.append(" stream id=");
        body.append(c.param("id").toStringView().value_or(""));
        body.append(" bytes=");
        appendUnsigned(body, bytes);
        body.push_back('\n');
        co_return c.text(body);
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8088)
        .setThreadNum(2)
        .run();
}
