#pragma once

#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/controller/ControllerRuntime.h"
#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/rule/Rules.h"

namespace ruvia {

template <typename BodyT>
class JsonBody final : public Middleware<JsonBody<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "JsonBody requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kJson, BodyT>(*this, c, next);
    }
};

template <typename BodyT>
class FormBody final : public Middleware<FormBody<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "FormBody requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kForm, BodyT>(*this, c, next);
    }
};

template <typename BodyT>
class QueryModel final : public Middleware<QueryModel<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "QueryModel requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kQuery, BodyT>(*this, c, next);
    }
};

template <typename BodyT>
class PathModel final : public Middleware<PathModel<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "PathModel requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kParam, BodyT>(*this, c, next);
    }
};

template <typename BodyT>
class HeaderModel final : public Middleware<HeaderModel<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "HeaderModel requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kHeader, BodyT>(*this, c, next);
    }
};

template <typename BodyT>
class CookieModel final : public Middleware<CookieModel<BodyT>> {
public:
    static_assert(detail::isRequestModel<BodyT>, "CookieModel requires a RUVIA_REQUEST_MODEL");
    using RuviaValidationBody = BodyT;

    void validate(const BodyT& body, Validator& validator) const {
        detail::ModelValidationAccess::validateModel(body, validator);
    }

    [[nodiscard]] Task<void> handle(Context& c, Next& next) {
        return detail::invokeModelValidator<ValidationTarget::kCookie, BodyT>(*this, c, next);
    }
};

}  // namespace ruvia
