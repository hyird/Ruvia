#pragma once

#include "ruvia/web/detail/model/ModelOptions.h"

// Public field option types plus preprocessor iteration used only by route
// validation rule lists. Model registration itself uses C++ parameter packs.

#define RUVIA_MODEL_EXPAND(x) x

// Route validation currently expands its rule list in the preprocessor. These
// counters must never be used by RUVIA_REQUEST_MODEL/RUVIA_RESPONSE_MODEL.
#define RUVIA_VALIDATION_NARG(...) RUVIA_VALIDATION_NARG_(__VA_ARGS__, RUVIA_VALIDATION_RSEQ())
#define RUVIA_VALIDATION_NARG_(...) RUVIA_MODEL_EXPAND(RUVIA_VALIDATION_ARG_N(__VA_ARGS__))
#define RUVIA_VALIDATION_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15,   \
    _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, \
    _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, \
    _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, N, ...)                                 \
    N
#define RUVIA_VALIDATION_RSEQ()                                                                 \
    64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, \
        41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, \
        19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#define RUVIA_MODEL_CAT(a, b) RUVIA_MODEL_CAT_(a, b)
#define RUVIA_MODEL_CAT_(a, b) a##b

#define RUVIA_DEFAULT(value) ::ruvia::detail::model::StaticDefault<[]() constexpr { return value; }>
#define RUVIA_OMIT_EMPTY ::ruvia::detail::model::OmitEmpty
#define RUVIA_EMIT_NULL ::ruvia::detail::model::EmitNull
#define RUVIA_MODEL_FE_1(m, T, x) m(T, x)
#define RUVIA_MODEL_FE_2(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_1(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_3(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_2(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_4(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_3(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_5(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_4(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_6(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_5(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_7(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_6(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_8(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_7(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_9(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_8(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_10(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_9(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_11(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_10(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_12(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_11(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_13(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_12(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_14(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_13(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_15(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_14(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_16(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_15(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_17(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_16(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_18(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_17(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_19(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_18(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_20(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_19(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_21(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_20(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_22(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_21(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_23(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_22(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_24(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_23(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_25(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_24(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_26(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_25(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_27(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_26(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_28(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_27(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_29(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_28(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_30(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_29(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_31(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_30(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_32(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_31(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_33(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_32(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_34(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_33(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_35(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_34(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_36(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_35(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_37(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_36(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_38(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_37(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_39(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_38(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_40(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_39(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_41(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_40(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_42(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_41(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_43(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_42(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_44(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_43(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_45(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_44(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_46(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_45(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_47(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_46(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_48(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_47(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_49(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_48(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_50(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_49(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_51(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_50(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_52(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_51(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_53(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_52(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_54(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_53(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_55(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_54(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_56(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_55(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_57(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_56(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_58(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_57(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_59(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_58(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_60(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_59(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_61(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_60(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_62(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_61(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_63(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_62(m, T, __VA_ARGS__))
#define RUVIA_MODEL_FE_64(m, T, x, ...) \
    m(T, x) RUVIA_MODEL_EXPAND(RUVIA_MODEL_FE_63(m, T, __VA_ARGS__))

#define RUVIA_VALIDATION_FOR_EACH(m, T, ...) \
    RUVIA_MODEL_EXPAND(                      \
        RUVIA_MODEL_CAT(RUVIA_MODEL_FE_, RUVIA_VALIDATION_NARG(__VA_ARGS__))(m, T, __VA_ARGS__))

#define RUVIA_VALIDATION_UNPAREN(...) __VA_ARGS__
