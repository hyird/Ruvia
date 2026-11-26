#include <cstdint>

#include "controller.h"

namespace controller_model_signature_test {

RUVIA_MODEL(create_request, RUVIA_REQUIRED_FIELD(id, std::uint32_t));
RUVIA_MODEL(update_request, RUVIA_REQUIRED_FIELD(id, std::uint32_t));

class items_controller final {
public:
    RUVIA_ROUTES_BEGIN(items_controller)
    RUVIA_POST("/items", create, ruvia::json_body<create_request>);
    RUVIA_ROUTES_END

private:
#ifdef RUVIA_TEST_MISMATCHED_MODEL
    using input_type = update_request;
#else
    using input_type = create_request;
#endif
    ruvia::task<ruvia::http_response> create(ruvia::context& c, const input_type& input) {
        co_return c.json(input);
    }
};

}  // namespace controller_model_signature_test
