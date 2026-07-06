#ifdef RUVIA_ENABLE_HTTP_CLIENT

// FetchResponseStream is now header-only: it holds status + headers + a unified HttpBodyStream
// (ruvia/http/HttpBodyStream.h) and all its members are inline. The former out-of-line pimpl
// implementation and the FetchStreamSource virtual interface were removed by the streaming
// unification, so this translation unit is intentionally empty.

#endif  // RUVIA_ENABLE_HTTP_CLIENT
