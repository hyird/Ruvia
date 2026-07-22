#include <ruvia/edge/EdgeServer.h>

int main() {
    ruvia::edge::EdgeServer server({"127.0.0.1", 0});
    if (!server.addOrigin(
            "front.local",
            ruvia::edge::OriginSettings{"origin.local", 8080, false})) {
        return 2;
    }
    return server.localEndpoint().port == 0 ? 1 : 0;
}
