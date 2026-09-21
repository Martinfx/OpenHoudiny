#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <exception>

namespace testing {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

Registrar::Registrar(std::string name, std::function<void()> fn) {
    registry().push_back(TestCase{std::move(name), std::move(fn)});
}

void fail(const char* file, int line, const std::string& msg) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + msg};
}

int runAll() {
    int failed = 0;
    const auto start = std::chrono::steady_clock::now();

    for (const auto& tc : registry()) {
        try {
            tc.fn();
            std::printf("  ok    %s\n", tc.name.c_str());
        } catch (const Failure& f) {
            std::printf("  FAIL  %s\n        %s\n", tc.name.c_str(), f.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n        unexpected exception: %s\n",
                        tc.name.c_str(), e.what());
            ++failed;
        }
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
    std::printf("\n%zu tests, %d failed  (%lld ms)\n", registry().size(), failed,
                static_cast<long long>(ms));
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

int main() {
    std::printf("running %zu tests\n\n", testing::registry().size());
    return testing::runAll();
}
