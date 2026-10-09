// In-memory application testing: test_app/test_request/test_response drive the
// production dispatch pipeline -- routing, params, model bodies, middleware,
// fallbacks, url_for and worker state -- without opening a socket. This is the
// pattern an application's own test suite uses; the example doubles as a
// runnable check and exits non-zero on any mismatch.
// Run ruvia_example_testing with no arguments or external services.
// Assertions below check functional responses; test_app owns startup/teardown.

#include "ruvia/web/testing.h"

#include <cstdio>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

RUVIA_MODEL(note_request, RUVIA_OPTIONAL_FIELD(text, ruvia::string));

namespace {

struct note_counter final {
    int stored_{0};
};

class audit_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Audited", "yes");
    }
};

class notes_controller final : public ruvia::controller<notes_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/notes")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/:id", note);
    RUVIA_POST("/", create, ruvia::json_body<note_request>);
    RUVIA_GET("/", stats);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> note(ruvia::context& c) {
        // url_for builds links from registered patterns; the pattern is the
        // route's identity.
        std::pmr::string body(c.arena());
        body.append("note ");
        body.append(c.req().param("id").value_or("?"));
        body.append(" self=");
        body.append(c.url_for("/notes/:id", {c.req().param("id").value_or("0")}));
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto& note = c.req().validated<note_request>();
        ++c.worker_state<note_counter>().stored_;
        c.status(ruvia::http_status::created);
        co_return c.body(note.get<"text">().has_value() ? note.get<"text">()->view() : "empty");
    }

    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        std::pmr::string body(c.arena());
        body.append("stored=");
        body.append(std::to_string(c.worker_state<note_counter>().stored_));
        co_return c.text(std::move(body));
    }
};

ruvia::task<ruvia::http_response> notes_missing(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    co_return c.text("no such note");
}

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAILED: %s\n", what);
    }
}

}  // namespace

int main() {
    ruvia::test_app app;
    app.use<audit_middleware>().on_not_found({.prefix_ = "/notes", .handler_ = &notes_missing});
    app.use_worker_state<note_counter>();

    // Routing, params and url_for.
    const auto note = app.request(ruvia::test_request::get("/notes/7"));
    expect(note.status() == ruvia::http_status::ok, "GET /notes/7 is 200");
    expect(note.body() == "note 7 self=/notes/7", "url_for builds the note link");
    expect(
        note.header("X-Audited").value_or("") == "yes", "global middleware stamped the response");

    // Model bodies keep their production status split: 415 for the wrong
    // media type, 400 for a malformed body of the right type.
    const auto created =
        app.request(ruvia::test_request::post("/notes").json(R"({"text":"remember"})"));
    expect(created.status() == ruvia::http_status::created, "valid JSON is 201");
    expect(created.body() == "remember", "created body echoes the model field");
    const auto wrong_type =
        app.request(ruvia::test_request::post("/notes").body("text", "text/plain"));
    expect(
        wrong_type.status() == ruvia::http_status::unsupported_media_type, "wrong media type is 415");

    // Worker state persisted across the requests above.
    const auto stats = app.request(ruvia::test_request::get("/notes"));
    expect(stats.body() == "stored=1", "worker state persisted");

    // The prefix-scoped not_found handled the miss under /notes.
    const auto missing = app.request(ruvia::test_request::get("/notes/9/edit"));
    expect(missing.status() == ruvia::http_status::not_found, "miss is 404");
    expect(missing.body() == "no such note", "prefix not_found rendered the miss");

    if (g_failures == 0) {
        std::puts("testing facade example: all checks passed");
    }
    return g_failures == 0 ? 0 : 1;
}
