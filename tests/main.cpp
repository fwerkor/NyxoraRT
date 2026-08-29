#include "test.hpp"
#include <iostream>

std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int main() {
    std::size_t failures = 0;
    for (const auto& test : test_registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << std::endl;
        }
    }
    return failures == 0 ? 0 : 1;
}
