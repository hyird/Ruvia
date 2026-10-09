#pragma once

#include "ruvia/web/detail/controller/controller_runtime.h"
#include "ruvia/web/detail/model/rule/rules.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/next.h"
#include "ruvia/web/validation.h"

namespace ruvia {

template <typename body_t_type>
class json_body final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "json_body requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::json, body_t_type>(*this, c, next_value);
    }
};

template <typename body_t_type>
class form_body final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "form_body requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::form, body_t_type>(*this, c, next_value);
    }
};

template <typename body_t_type>
class query_model final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "query_model requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::query, body_t_type>(*this, c, next_value);
    }
};

template <typename body_t_type>
class path_model final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "path_model requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::param, body_t_type>(*this, c, next_value);
    }
};

template <typename body_t_type>
class header_model final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "header_model requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::header, body_t_type>(*this, c, next_value);
    }
};

template <typename body_t_type>
class cookie_model final : public middleware {
public:
    static_assert(detail::is_model<body_t_type>, "cookie_model requires a RUVIA_MODEL");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        detail::model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        return detail::invoke_model_validator<validation_target::cookie, body_t_type>(*this, c, next_value);
    }
};

}  // namespace ruvia
