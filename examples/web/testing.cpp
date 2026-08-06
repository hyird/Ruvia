// In-memory application testing: TestApp/TestRequest/TestResponse drive the
// production dispatch pipeline -- routing, params, model bodies, middleware,
// fallbacks, urlFor and worker state -- without opening a socket. This is the
// pattern an application's own test suite uses; the example doubles as a
// runnable check and exits non-zero on any mismatch.

#include <cstdio>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Testing.h"

struct NoteRequest final {
    RUVIA_OPTIONAL_FIELD(text, ruvia::String);
    RUVIA_MODEL(NoteRequest, text);
};

namespace {

struct NoteCounter final {
    int stored{0};
};

class AuditMiddleware final : public ruvia::Middleware<AuditMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Audited", "yes");
    }
};

class NotesController final : public ruvia::Controller<NotesController> {
public:
    RUVIA_CONTROLLER_GROUP("/notes")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/:id", note);
    RUVIA_POST("/", create);
    RUVIA_GET("/", stats);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> note(ruvia::Context& c) {
        // urlFor builds links from registered patterns; the pattern is the
        // route's identity.
        std::pmr::string body(c.resource());
        body.append("note ");
        body.append(c.req().param("id").value_or("?"));
        body.append(" self=");
        body.append(c.urlFor("/notes/:id", {c.req().param("id").value_or("0")}));
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        const auto note = co_await c.req().json<NoteRequest>();
        ++c.workerState<NoteCounter>().stored;
        c.status(ruvia::http_status::kCreated);
        co_return c.body(note.text().has_value() ? note.text()->view() : "empty");
    }

    ruvia::Task<ruvia::HttpResponse> stats(ruvia::Context& c) {
        std::pmr::string body(c.resource());
        body.append("stored=");
        body.append(std::to_string(c.workerState<NoteCounter>().stored));
        co_return c.text(std::move(body));
    }
};

ruvia::Task<ruvia::HttpResponse> notesMissing(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
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
    ruvia::TestApp app;
    app.use<AuditMiddleware>().onNotFound("/notes", &notesMissing);
    app.useWorkerState<NoteCounter>();

    // Routing, params and urlFor.
    const auto note = app.request(ruvia::TestRequest::get("/notes/7"));
    expect(note.status() == ruvia::http_status::kOk, "GET /notes/7 is 200");
    expect(note.body() == "note 7 self=/notes/7", "urlFor builds the note link");
    expect(note.header("X-Audited").value_or("") == "yes", "global middleware stamped the response");

    // Model bodies keep their production status split: 415 for the wrong
    // media type, 400 for a malformed body of the right type.
    const auto created = app.request(ruvia::TestRequest::post("/notes").json(R"({"text":"remember"})"));
    expect(created.status() == ruvia::http_status::kCreated, "valid JSON is 201");
    expect(created.body() == "remember", "created body echoes the model field");
    const auto wrongType = app.request(ruvia::TestRequest::post("/notes").body("text", "text/plain"));
    expect(wrongType.status() == ruvia::http_status::kUnsupportedMediaType, "wrong media type is 415");

    // Worker state persisted across the requests above.
    const auto stats = app.request(ruvia::TestRequest::get("/notes"));
    expect(stats.body() == "stored=1", "worker state persisted");

    // The prefix-scoped notFound handled the miss under /notes.
    const auto missing = app.request(ruvia::TestRequest::get("/notes/9/edit"));
    expect(missing.status() == ruvia::http_status::kNotFound, "miss is 404");
    expect(missing.body() == "no such note", "prefix notFound rendered the miss");

    if (g_failures == 0) {
        std::puts("testing facade example: all checks passed");
    }
    return g_failures == 0 ? 0 : 1;
}
