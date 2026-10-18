// The miniaudio implementation, compiled once with only device I/O switched on.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "ma_config.hpp"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
