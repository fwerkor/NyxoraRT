#pragma once

#include <string_view>

#include "nyxora/runtime/hle_registry.hpp"

namespace nyxora::hle::libkernel {

void register_core(runtime::HleRegistry& registry);
[[nodiscard]] bool provides_module(std::string_view filename) noexcept;

} // namespace nyxora::hle::libkernel
