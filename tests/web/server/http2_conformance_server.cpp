#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <system_error>

class Http2ConformanceController final : public ruvia::Controller<Http2ConformanceController> {
public:
    RUVIA_CONTROLLER_GROUP("")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", root);
    RUVIA_POST("/", root);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> root(ruvia::Context& context) {
        co_return context.text("ok\n");
    }
};

int main(int argc, char** argv) {
    if (argc != 2) {
        return EXIT_FAILURE;
    }

    std::uint16_t port{};
    const std::string_view text(argv[1]);
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() || port == 0) {
        return EXIT_FAILURE;
    }

    ruvia::app().setListeners({ruvia::ListenerConfig::http("127.0.0.1", port)}).setWorkersPerListener(1).setSignalShutdown(true).setCompression(std::nullopt).run();
}
