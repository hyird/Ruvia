#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory_resource>
#include <optional>
#include <random>
#include <string_view>
#include <system_error>
#include <thread>

#include "ruvia/app/App.h"
#include "ruvia/db/Db.h"
#include "ruvia/http/Controller.h"
#include "ruvia/http/HttpTypes.h"

namespace {

struct World final {
    std::uint32_t id{0};
    std::uint32_t randomNumber{0};
};

struct Fortune final {
    std::uint32_t id{0};
    std::pmr::string message;

    explicit Fortune(std::pmr::memory_resource* resource)
        : message(resource) {}
};

std::optional<std::string_view> envValue(std::string_view first, std::string_view second = {}, std::string_view third = {}) {
    std::array names{first, second, third};
    for (const auto name : names) {
        if (name.empty()) {
            continue;
        }
        if (const auto* value = std::getenv(std::pmr::string(name).c_str())) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> parseEnvNumber(std::string_view first, std::string_view second = {}, std::string_view third = {}) {
    const auto value = envValue(first, second, third);
    if (!value || value->empty()) {
        return std::nullopt;
    }

    T parsed{};
    const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (ec != std::errc{} || ptr != value->data() + value->size()) {
        return std::nullopt;
    }
    return parsed;
}

void assignEnvString(std::pmr::string& target, std::string_view first, std::string_view second, std::string_view third) {
    if (const auto value = envValue(first, second, third)) {
        target.assign(value->data(), value->size());
    }
}

ruvia::DbConfig dbConfigFromEnv() {
    ruvia::DbConfig config;
    config.host.assign("tfb-database");
    config.username.assign("benchmarkdbuser");
    config.password.assign("benchmarkdbpass");
    config.database.assign("hello_world");
    config.acquireTimeout = std::chrono::seconds(2);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(30);

    assignEnvString(config.host, "RUVIA_DB_HOST", "DBHOST", "DATABASE_HOST");
    assignEnvString(config.username, "RUVIA_DB_USER", "DBUSER", "DATABASE_USER");
    assignEnvString(config.password, "RUVIA_DB_PASSWORD", "DBPASSWORD", "DATABASE_PASSWORD");
    assignEnvString(config.database, "RUVIA_DB_DATABASE", "DBNAME", "DATABASE_NAME");

    if (const auto port = parseEnvNumber<std::uint16_t>("RUVIA_DB_PORT", "DBPORT", "DATABASE_PORT")) {
        config.port = *port;
    }
    if (const auto poolSize = parseEnvNumber<std::size_t>("RUVIA_DB_POOL_SIZE")) {
        config.poolSize = *poolSize;
    }
    return config;
}

std::size_t threadCountFromEnv() {
    if (const auto threads = parseEnvNumber<std::size_t>("RUVIA_THREADS", "MAX_THREADS")) {
        return std::max<std::size_t>(1, *threads);
    }
    const auto detected = std::thread::hardware_concurrency();
    return detected == 0 ? 2 : detected;
}

bool noDbModeFromEnv() {
    const auto value = envValue("RUVIA_TFB_NO_DB");
    return value == "1" || value == "true" || value == "TRUE";
}

std::uint32_t randomWorldId() {
    thread_local std::mt19937 generator(std::random_device{}());
    thread_local std::uniform_int_distribution<std::uint32_t> distribution(1, 10000);
    return distribution(generator);
}

std::uint32_t parseUnsigned(std::string_view value) {
    std::uint32_t parsed{};
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return 0;
    }
    return parsed;
}

std::uint32_t boundedQueryCount(const ruvia::Context& c, std::string_view name) {
    const auto value = c.query(name).toUInt32().value_or(1);
    return std::clamp<std::uint32_t>(value, 1, 500);
}

void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

void appendWorldJson(std::pmr::string& output, const World& world) {
    output.append("{\"id\":");
    appendUnsigned(output, world.id);
    output.append(",\"randomNumber\":");
    appendUnsigned(output, world.randomNumber);
    output.push_back('}');
}

void appendJsonString(std::pmr::string& output, std::string_view value) {
    output.push_back('"');
    for (const char ch : value) {
        switch (ch) {
        case '"': output.append("\\\""); break;
        case '\\': output.append("\\\\"); break;
        case '\b': output.append("\\b"); break;
        case '\f': output.append("\\f"); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                output.append("\\u00");
                output.push_back(kHex[(static_cast<unsigned char>(ch) >> 4) & 0x0f]);
                output.push_back(kHex[static_cast<unsigned char>(ch) & 0x0f]);
            } else {
                output.push_back(ch);
            }
            break;
        }
    }
    output.push_back('"');
}

void appendHtmlEscaped(std::pmr::string& output, std::string_view value) {
    for (const char ch : value) {
        switch (ch) {
        case '&': output.append("&amp;"); break;
        case '<': output.append("&lt;"); break;
        case '>': output.append("&gt;"); break;
        case '"': output.append("&quot;"); break;
        default: output.push_back(ch); break;
        }
    }
}

ruvia::HttpResponse exactResponse(
    ruvia::Context& c,
    std::string_view contentType,
    std::pmr::string& body) {
    ruvia::HttpResponse response(c.resource());
    response.setHeader("Content-Type", contentType);
    response.setBody(std::move(body));
    return response;
}

ruvia::HttpResponse exactStaticResponse(
    ruvia::Context& c,
    std::string_view contentType,
    std::string_view body) {
    ruvia::HttpResponse response(c.resource());
    response.setHeader("Content-Type", contentType);
    response.setBodyView(body);
    return response;
}

World worldFromRow(const ruvia::DbRow& row) {
    World world;
    if (row.size() >= 2) {
        world.id = parseUnsigned(row[0].text());
        world.randomNumber = parseUnsigned(row[1].text());
    }
    return world;
}

void appendWorldArrayJson(std::pmr::string& output, std::span<const World> worlds) {
    output.push_back('[');
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (i != 0) {
            output.push_back(',');
        }
        appendWorldJson(output, worlds[i]);
    }
    output.push_back(']');
}

}

class TechEmpowerController final : public ruvia::Controller<TechEmpowerController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/json", json);
    RUVIA_GET("/plaintext", plaintext);
    RUVIA_GET("/db", db);
    RUVIA_GET("/queries", queries);
    RUVIA_GET("/fortunes", fortunes);
    RUVIA_GET("/updates", updates);
    RUVIA_GET("/cached-worlds", cachedWorlds);
    RUVIA_GET("/cached-queries", cachedWorlds);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> json(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        body.append("{\"message\":\"Hello, World!\"}");
        co_return exactResponse(c, "application/json", body);
    }

    ruvia::Task<ruvia::HttpResponse> plaintext(ruvia::Context& c) {
        co_return exactStaticResponse(c, "text/plain", "Hello, World!");
    }

    ruvia::Task<ruvia::HttpResponse> db(ruvia::Context& c) {
        const auto world = co_await fetchWorld(c, randomWorldId());
        std::pmr::string body(c.allocator<char>());
        appendWorldJson(body, world);
        co_return exactResponse(c, "application/json", body);
    }

    ruvia::Task<ruvia::HttpResponse> queries(ruvia::Context& c) {
        const auto count = boundedQueryCount(c, "queries");
        std::pmr::vector<World> worlds(c.allocator<World>());
        worlds.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            worlds.push_back(co_await fetchWorld(c, randomWorldId()));
        }

        std::pmr::string body(c.allocator<char>());
        appendWorldArrayJson(body, worlds);
        co_return exactResponse(c, "application/json", body);
    }

    ruvia::Task<ruvia::HttpResponse> fortunes(ruvia::Context& c) {
        auto result = co_await c.db().query("SELECT id, message FROM Fortune");
        std::pmr::vector<Fortune> fortunes(c.allocator<Fortune>());
        fortunes.reserve(result.rows().size() + 1);

        for (const auto& row : result.rows()) {
            Fortune fortune(c.resource());
            if (row.size() >= 2) {
                fortune.id = parseUnsigned(row[0].text());
                fortune.message.assign(row[1].text().data(), row[1].text().size());
            }
            fortunes.emplace_back(std::move(fortune));
        }

        Fortune extra(c.resource());
        extra.id = 0;
        extra.message.assign("Additional fortune added at request time.");
        fortunes.emplace_back(std::move(extra));

        std::ranges::sort(fortunes, [](const Fortune& left, const Fortune& right) {
            return left.message < right.message;
        });

        std::pmr::string body(c.allocator<char>());
        body.append("<!DOCTYPE html><html><head><title>Fortunes</title></head><body><table>");
        body.append("<tr><th>id</th><th>message</th></tr>");
        for (const auto& fortune : fortunes) {
            body.append("<tr><td>");
            appendUnsigned(body, fortune.id);
            body.append("</td><td>");
            appendHtmlEscaped(body, fortune.message);
            body.append("</td></tr>");
        }
        body.append("</table></body></html>");

        co_return exactResponse(c, "text/html; charset=utf-8", body);
    }

    ruvia::Task<ruvia::HttpResponse> updates(ruvia::Context& c) {
        const auto count = boundedQueryCount(c, "queries");
        std::pmr::vector<World> worlds(c.allocator<World>());
        worlds.reserve(count);

        auto tx = co_await c.db().beginTransaction();
        for (std::uint32_t i = 0; i < count; ++i) {
            auto result = co_await tx.query(
                "SELECT id, randomNumber FROM World WHERE id = ?",
                {randomWorldId()});
            World world = result.rows().empty() ? World{} : worldFromRow(result.rows()[0]);
            world.randomNumber = randomWorldId();
            auto updateResult = co_await tx.execute(
                "UPDATE World SET randomNumber = ? WHERE id = ?",
                {world.randomNumber, world.id});
            (void)updateResult;
            worlds.push_back(world);
        }
        co_await tx.commit();

        std::pmr::string body(c.allocator<char>());
        appendWorldArrayJson(body, worlds);
        co_return exactResponse(c, "application/json", body);
    }

    ruvia::Task<ruvia::HttpResponse> cachedWorlds(ruvia::Context& c) {
        const auto count = boundedQueryCount(c, "count");
        co_await ensureCache(c);

        std::pmr::vector<World> worlds(c.allocator<World>());
        worlds.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto id = randomWorldId();
            worlds.push_back(cache()[id]);
        }

        std::pmr::string body(c.allocator<char>());
        appendWorldArrayJson(body, worlds);
        co_return exactResponse(c, "application/json", body);
    }

    static ruvia::Task<World> fetchWorld(ruvia::Context& c, std::uint32_t id) {
        auto result = co_await c.db().query(
            "SELECT id, randomNumber FROM World WHERE id = ?",
            {id});
        co_return result.rows().empty() ? World{} : worldFromRow(result.rows()[0]);
    }

    static std::pmr::vector<World>& cache() {
        thread_local std::pmr::vector<World> cached(std::pmr::get_default_resource());
        return cached;
    }

    static ruvia::Task<void> ensureCache(ruvia::Context& c) {
        auto& cached = cache();
        if (cached.size() == 10001) {
            co_return;
        }

        cached.assign(10001, World{});
        auto result = co_await c.db().query("SELECT id, randomNumber FROM CachedWorld");
        for (const auto& row : result.rows()) {
            const auto world = worldFromRow(row);
            if (world.id < cached.size()) {
                cached[world.id] = world;
            }
        }
    }
};

int main() {
    ruvia::MemoryPoolConfig memory;
    memory.requestInitialBufferBytes = 4096;

    auto& app = ruvia::app();
    app
        .setListenAddress("0.0.0.0", 8080)
        .setThreadNum(threadCountFromEnv())
        .setIdleTimeout(std::chrono::seconds(60))
        .setHeaderTimeout(std::chrono::seconds(15))
        .setBodyTimeout(std::chrono::seconds(30))
        .setWriteTimeout(std::chrono::seconds(30))
        .setMaxConnectionsPerWorker(10000)
        .setMaxRequestsPerConnection(0)
        .setMemoryPoolConfig(memory);

    if (!noDbModeFromEnv()) {
        app.useDb(dbConfigFromEnv());
    }

    app.run();
}
