#include <ruvia/http/HttpResponse.h>

int main() {
    const ruvia::HttpResponse response;
    return response.status() == ruvia::http_status::kOk ? 0 : 1;
}
