#pragma once
//
// Minimal test harness. A dependency-free prototype should not need GoogleTest
// to be evaluated; swapping this out later is a one-file change.
//
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(std::string name, std::function<void()> fn);
};

/// Thrown by a failing check; caught per test case by runAll().
struct Failure {
    std::string message;
};

[[noreturn]] void fail(const char* file, int line, const std::string& msg);

int runAll();

template <class T>
concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

/// Best-effort rendering: enums print as their value, anything with no
/// operator<< prints a placeholder rather than failing to compile.
template <class T>
void emit(std::ostream& os, const T& v) {
    if constexpr (std::is_enum_v<T>) {
        os << static_cast<long long>(v);
    } else if constexpr (Streamable<T>) {
        os << v;
    } else {
        os << "<unprintable>";
    }
}

template <class A, class B>
std::string describe(const char* expr, const A& a, const B& b) {
    std::ostringstream os;
    os << expr << "\n      left  = ";
    emit(os, a);
    os << "\n      right = ";
    emit(os, b);
    return os.str();
}

}  // namespace testing

#define TEST(name)                                                   \
    static void name();                                              \
    static ::testing::Registrar registrar_##name(#name, name);       \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        const auto& va_ = (a);                                                 \
        const auto& vb_ = (b);                                                 \
        if (!(va_ == vb_)) {                                                   \
            ::testing::fail(__FILE__, __LINE__,                                \
                            ::testing::describe(#a " == " #b, va_, vb_));      \
        }                                                                      \
    } while (0)

#define CHECK_NE(a, b)                                                         \
    do {                                                                       \
        const auto& va_ = (a);                                                 \
        const auto& vb_ = (b);                                                 \
        if (va_ == vb_) {                                                      \
            ::testing::fail(__FILE__, __LINE__,                                \
                            ::testing::describe(#a " != " #b, va_, vb_));      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        const double va_ = static_cast<double>(a);                             \
        const double vb_ = static_cast<double>(b);                             \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                \
            ::testing::fail(__FILE__, __LINE__,                                \
                            ::testing::describe(#a " ~= " #b, va_, vb_));      \
        }                                                                      \
    } while (0)
