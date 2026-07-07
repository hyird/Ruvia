#pragma once

#include "ruvia/http/detail/model/ModelOptions.h"

// Common preprocessor machinery and public field option macros for RUVIA_MODEL.

#define RUVIA_MODEL_EXPAND(x) x

#define RUVIA_MODEL_NARG(...) RUVIA_MODEL_NARG_(__VA_ARGS__, RUVIA_MODEL_RSEQ())
#define RUVIA_MODEL_NARG_(...) RUVIA_MODEL_EXPAND(RUVIA_MODEL_ARG_N(__VA_ARGS__))
#define RUVIA_MODEL_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N
#define RUVIA_MODEL_RSEQ() 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0

#define RUVIA_MODEL_CAT(a, b) RUVIA_MODEL_CAT_(a, b)
#define RUVIA_MODEL_CAT_(a, b) a##b

#define RUVIA_DEFAULT(value) ::ruvia::detail::model::Default{value}
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::OmitEmpty{}
#define RUVIA_EMIT_NULL ::ruvia::detail::model::EmitNull{}
#define RUVIA_FIELD(field, type, ...) \
    ((type), field, (#field), (::ruvia::detail::model::ModelOptions{__VA_ARGS__}))
#define RUVIA_FIELD_NAME(wire_name, field, type, ...) \
    ((type), field, (wire_name), (::ruvia::detail::model::ModelOptions{__VA_ARGS__}))

#define RUVIA_MODEL_FE_1(m, T, x)       m(T, x)
#define RUVIA_MODEL_FE_2(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_1(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_3(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_2(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_4(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_3(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_5(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_4(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_6(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_5(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_7(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_6(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_8(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_7(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_9(m, T, x, ...)  m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_8(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_10(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_9(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_11(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_10(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_12(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_11(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_13(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_12(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_14(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_13(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_15(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_14(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_16(m, T, x, ...) m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_15(m, T, __VA_ARGS__))

#define RUVIA_MODEL_FOR_EACH(m, T, ...) \
    RUVIA_MODEL_EXPAND(RUVIA_MODEL_CAT(RUVIA_MODEL_FE_, RUVIA_MODEL_NARG(__VA_ARGS__))(m, T, __VA_ARGS__))

#define RUVIA_MODEL_UNPAREN(...) __VA_ARGS__
