#include "ruvia/web/Error.h"

#include <span>
#include <utility>

template <typename Issues>
concept AcceptsRvalueHttpErrorInfoIssues = requires(Issues&& issues) {
    ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .validationIssues = std::forward<Issues>(issues)});
};

static_assert(sizeof(ruvia::ValidationIssue) > 0);
static_assert(AcceptsRvalueHttpErrorInfoIssues<std::span<const ruvia::ValidationIssue>>);

int main() {
    return 0;
}
