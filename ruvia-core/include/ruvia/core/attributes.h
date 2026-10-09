#pragma once

#ifndef RUVIA_LIFETIMEBOUND
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(gnu::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[gnu::lifetimebound]]
#else
#define RUVIA_LIFETIMEBOUND
#endif
#else
#define RUVIA_LIFETIMEBOUND
#endif
#endif
