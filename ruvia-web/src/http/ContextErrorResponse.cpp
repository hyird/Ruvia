#include "HttpErrorNormalize.h"
#include "HttpResponseHeaderState.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/ErrorHandlers.h"

#include <exception>

namespace ruvia {

Task<HttpResponse> makeErrorResponse(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection,
    HttpErrorHandler handler) {
    error = detail::normalizeHttpErrorInfo(error);

    if (handler != nullptr) {
        try {
            auto response = co_await handler(context, error);
            if (closeConnection) {
                detail::setResponseHeaderStableView(response, "Connection", "close");
            }
            co_return response;
        } catch (const HttpError& nested) {
            co_return makeErrorResponse(context.resource(), nested.info(), closeConnection);
        } catch (const std::exception&) {
            // The error handler itself threw; keep transport output deterministic
            // and avoid echoing exception detail to the client.
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo(500, "error_handler_failed", "error handler failed"),
                closeConnection);
        } catch (...) {
            co_return makeErrorResponse(
                context.resource(),
                HttpErrorInfo(500, "error_handler_failed", "error handler failed"),
                closeConnection);
        }
    }

    co_return makeErrorResponse(context.resource(), error, closeConnection);
}

}  // namespace ruvia
