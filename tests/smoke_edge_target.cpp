#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpResponse.h"

int main() {
    ruvia::HttpResponse response;
    response.status(200);
    const auto cache = ruvia::parseCacheControl("public, max-age=1");
    return response.status() == 200 && cache.maxAge.has_value() ? 0 : 1;
}
