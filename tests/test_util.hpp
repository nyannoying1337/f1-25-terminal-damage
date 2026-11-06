// Shared by the standalone tests (tests/*.cpp). Pure standard C++: the header-only tests include it too.
//
// Every test starts with "#ifdef NDEBUG / #undef NDEBUG / #endif" before its first #include (zig defines NDEBUG at
// -O1 and -O2, and with NDEBUG every assert compiles to nothing). Include this header last, after the other includes:
// a test that forgot the preamble then fails to build instead of passing without checking anything.
#pragma once
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error "asserts are compiled out (zig defines NDEBUG at -O1 and -O2): put '#ifdef NDEBUG / #undef NDEBUG / #endif' before the first #include of the test"
#endif

namespace tu {

// A failed assert ends the program at once (exit code 3, and its "Assertion failed: expr, file, line" line on stderr).
// Without this, abort() goes through Windows Error Reporting: 2.5 s (up to 9 s under load) and exit code 0xC0000409.
inline const int fast_abort = (_set_abort_behavior(0, _CALL_REPORTFAULT), 0);

// The game's log function the addon is handed (SwoftyApi::log): the tests do not look at it.
inline void quiet_log(const char *, ...) {}

// Not named near/far: windows.h defines both as empty macros, so near(a, b, eps) compiles to a comma expression and an
// assert on it is always true.
inline bool close_to(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

inline uintptr_t addr(const void *p) { return reinterpret_cast<uintptr_t>(p); }

// Writes a value into fake game memory (a raw block or a byte vector; the vector overload checks the range).
template <class T> void put(void *base, size_t at, T v) { std::memcpy(static_cast<uint8_t *>(base) + at, &v, sizeof v); }
template <class T> void put(std::vector<uint8_t> &b, size_t at, T v)
{
    assert(at + sizeof v <= b.size());
    put(b.data(), at, v);
}

}  // namespace tu
