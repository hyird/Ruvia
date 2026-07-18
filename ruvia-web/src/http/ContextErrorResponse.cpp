#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/HttpErrorNormalize.h"
#include "ruvia/web/detail/http/HttpErrorResponse.h"

#include <exception>

namespace ruvia {

Task<HttpResponse> detail::invokeErrorHandler(
    Context& context,
    HttpErrorInfo error,
    HttpErrorHandler handler) {
    error = normalizeHttpErrorInfo(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context, error);
        } catch (const HttpError& nested) {
            co_return makeDefaultErrorResponse(context.resource(), nested.info());
        } catch (const std::exception&) {
            // The error handler itself threw; keep transport output deterministic
            // and avoid echoing exception detail to the client.
            co_return makeDefaultErrorResponse(
                context.resource(),
                HttpErrorInfo(ruvia::http_status::kInternalServerError, "error_handler_failed", "error handler failed"));
        } catch (...) {
            co_return makeDefaultErrorResponse(
                context.resource(),
                HttpErrorInfo(ruvia::http_status::kInternalServerError, "error_handler_failed", "error handler failed"));
        }
    }

    co_return makeDefaultErrorResponse(context.resource(), error);
}

}  // namespace ruvia
