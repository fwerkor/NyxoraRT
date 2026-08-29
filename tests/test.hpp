#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& test_registry();
struct TestRegistration {
    TestRegistration(const char* name, std::function<void()> fn) { test_registry().push_back({name, std::move(fn)}); }
};
#define NYXORA_TEST(name) \
    static void name(); \
    static TestRegistration reg_##name(#name, name); \
    static void name()
#define NYXORA_CHECK(expr) do { if (!(expr)) throw std::runtime_error(std::string("check failed: ") + #expr); } while (false)
