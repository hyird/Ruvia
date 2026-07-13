#pragma once

#include "ruvia/web/detail/model/ModelOptions.h"

// Common preprocessor machinery and public field option macros for Ruvia models.

#define RUVIA_MODEL_EXPAND(x) x

// Field counter. A model with 1..16 fields resolves to that count; 17..32 fields
// resolve to the token OVERFLOW so the count-guard below can emit a readable
// "at most 16 fields" static_assert instead of a cryptic deep-preprocessor error.
#define RUVIA_MODEL_NARG(...) RUVIA_MODEL_NARG_(__VA_ARGS__, RUVIA_MODEL_RSEQ())
#define RUVIA_MODEL_NARG_(...) RUVIA_MODEL_EXPAND(RUVIA_MODEL_ARG_N(__VA_ARGS__))
#define RUVIA_MODEL_ARG_N( \
    _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, \
    _17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,_32, \
    N,...) N
#define RUVIA_MODEL_RSEQ() \
    OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW, \
    OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW,OVERFLOW, \
    16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0

#define RUVIA_MODEL_CAT(a, b) RUVIA_MODEL_CAT_(a, b)
#define RUVIA_MODEL_CAT_(a, b) a##b

// Field-count overflow guard. Expands to nothing for a supported field count and to a
// single readable static_assert for 17..32 fields. Placed once at class scope so an
// over-large model fails with this message rather than a garbage RUVIA_MODEL_FE_<junk>.
#define RUVIA_MODEL_GUARD_0
#define RUVIA_MODEL_GUARD_1
#define RUVIA_MODEL_GUARD_2
#define RUVIA_MODEL_GUARD_3
#define RUVIA_MODEL_GUARD_4
#define RUVIA_MODEL_GUARD_5
#define RUVIA_MODEL_GUARD_6
#define RUVIA_MODEL_GUARD_7
#define RUVIA_MODEL_GUARD_8
#define RUVIA_MODEL_GUARD_9
#define RUVIA_MODEL_GUARD_10
#define RUVIA_MODEL_GUARD_11
#define RUVIA_MODEL_GUARD_12
#define RUVIA_MODEL_GUARD_13
#define RUVIA_MODEL_GUARD_14
#define RUVIA_MODEL_GUARD_15
#define RUVIA_MODEL_GUARD_16
#define RUVIA_MODEL_GUARD_OVERFLOW \
    static_assert(false, \
        "Ruvia models support at most 16 fields; split large models into nested models");
#define RUVIA_MODEL_FIELD_COUNT_GUARD(...) \
    RUVIA_MODEL_CAT(RUVIA_MODEL_GUARD_, RUVIA_MODEL_NARG(__VA_ARGS__))
// Sink for the FOR_EACH dispatch on an overflowing model: emit nothing so the single
// guard static_assert above is the only diagnostic (no cryptic FE_OVERFLOW error).
#define RUVIA_MODEL_FE_OVERFLOW(m, T, ...)

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
