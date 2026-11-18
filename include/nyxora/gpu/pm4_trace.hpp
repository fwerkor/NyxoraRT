#pragma once

#include "nyxora/gpu/pm4.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <vector>

namespace nyxora::gpu::pm4 {

// Reads a headerless, little-endian PM4 dword stream. The size limit bounds
// memory use for malformed and untrusted trace files.
std::vector<std::uint32_t> read_binary_trace(std::istream& input,
                                             std::size_t max_dwords = 1U << 20U);

// Emits stable, machine-readable normalized commands. Guest GPU addresses
// are hexadecimal strings to preserve 64-bit precision in JSON consumers.
void write_submission_json(std::ostream& output, const Submission& submission, QueueType queue,
                           std::optional<std::uint32_t> queue_id = std::nullopt);

} // namespace nyxora::gpu::pm4
