#include <ruvia/http/HttpResponse.h>

int main() {
    const ruvia::HttpResponse response;
    return response.status() == 200 ? 0 : 1;
}
