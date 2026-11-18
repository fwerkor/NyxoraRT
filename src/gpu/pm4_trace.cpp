#include "nyxora/gpu/pm4_trace.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace nyxora::gpu::pm4 {
namespace {

void hex_string(std::ostream& output, std::uint64_t value) {
    const auto flags = output.flags();
    output << "\"0x" << std::hex << value << "\"";
    output.flags(flags);
}

void shader_json(std::ostream& output, const std::optional<ShaderProgram>& shader) {
    if (!shader) {
        output << "null";
        return;
    }
    constexpr std::array<std::string_view, 3> names{"vertex", "pixel", "compute"};
    output << "{\"stage\":\"" << names[static_cast<std::size_t>(shader->stage)]
           << "\",\"address\":";
    hex_string(output, shader->address);
    output << ",\"resource1\":";
    if (shader->resource1) {
        output << *shader->resource1;
    } else {
        output << "null";
    }
    output << ",\"resource2\":";
    if (shader->resource2) {
        output << *shader->resource2;
    } else {
        output << "null";
    }
    output << "}";
}

void draw_shaders(std::ostream& output, const std::optional<ShaderProgram>& vertex,
                  const std::optional<ShaderProgram>& pixel) {
    output << ",\"vertex_shader\":";
    shader_json(output, vertex);
    output << ",\"pixel_shader\":";
    shader_json(output, pixel);
}

std::string_view register_name(RegisterSpace space) {
    switch (space) {
    case RegisterSpace::type0:
        return "type0";
    case RegisterSpace::config:
        return "config";
    case RegisterSpace::context:
        return "context";
    case RegisterSpace::shader_graphics:
        return "shader_graphics";
    case RegisterSpace::shader_compute:
        return "shader_compute";
    case RegisterSpace::uconfig:
        return "uconfig";
    }
    throw std::runtime_error("unknown PM4 register space");
}

void write_command(std::ostream& output, const Command& command) {
    std::visit(
        [&output](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RegisterWrite>) {
                output << "{\"kind\":\"register_write\",\"space\":\""
                       << register_name(value.space) << "\",\"address\":";
                hex_string(output, value.address);
                output << ",\"value\":" << value.value;
            } else if constexpr (std::is_same_v<T, SetNumInstances>) {
                output << "{\"kind\":\"num_instances\",\"count\":" << value.count;
            } else if constexpr (std::is_same_v<T, SetIndexType>) {
                output << "{\"kind\":\"index_type\",\"raw\":" << value.raw;
            } else if constexpr (std::is_same_v<T, DrawIndexAuto>) {
                output << "{\"kind\":\"draw_index_auto\",\"index_count\":"
                       << value.index_count << ",\"initiator\":" << value.initiator;
                draw_shaders(output, value.vertex_shader, value.pixel_shader);
            } else if constexpr (std::is_same_v<T, DrawIndex2>) {
                output << "{\"kind\":\"draw_index_2\",\"max_size\":" << value.max_size
                       << ",\"index_address\":";
                hex_string(output, value.index_address);
                output << ",\"index_count\":" << value.index_count
                       << ",\"initiator\":" << value.initiator << ",\"index_type\":";
                if (value.index_type) {
                    output << *value.index_type;
                } else {
                    output << "null";
                }
                draw_shaders(output, value.vertex_shader, value.pixel_shader);
            } else if constexpr (std::is_same_v<T, DispatchDirect>) {
                output << "{\"kind\":\"dispatch_direct\",\"groups\":[" << value.groups_x
                       << "," << value.groups_y << "," << value.groups_z
                       << "],\"initiator\":" << value.initiator << ",\"compute_shader\":";
                shader_json(output, value.compute_shader);
            }
            output << "}";
        },
        command);
}

} // namespace

std::vector<std::uint32_t> read_binary_trace(std::istream& input, std::size_t max_dwords) {
    constexpr std::size_t bytes_per_dword = sizeof(std::uint32_t);
    if (max_dwords == 0 || max_dwords > static_cast<std::size_t>(-1) / bytes_per_dword) {
        throw std::runtime_error("invalid PM4 trace size limit");
    }

    std::vector<std::uint32_t> words;
    std::array<unsigned char, bytes_per_dword> pending{};
    std::size_t pending_count = 0;
    for (char byte{}; input.get(byte);) {
        if (words.size() == max_dwords) {
            throw std::runtime_error("PM4 trace exceeds size limit");
        }
        pending[pending_count++] = static_cast<unsigned char>(byte);
        if (pending_count == pending.size()) {
            const auto word = static_cast<std::uint32_t>(pending[0]) |
                              (static_cast<std::uint32_t>(pending[1]) << 8U) |
                              (static_cast<std::uint32_t>(pending[2]) << 16U) |
                              (static_cast<std::uint32_t>(pending[3]) << 24U);
            words.push_back(word);
            pending_count = 0;
        }
    }

    if (input.bad() || (!input.eof() && input.fail())) {
        throw std::runtime_error("failed to read PM4 trace");
    }
    if (pending_count != 0) {
        throw std::runtime_error("PM4 trace length is not a multiple of four bytes");
    }
    if (words.empty()) {
        throw std::runtime_error("PM4 trace is empty");
    }
    return words;
}

void write_submission_json(std::ostream& output, const Submission& submission, QueueType queue,
                           std::optional<std::uint32_t> queue_id) {
    output << "{\"queue\":\"" << (queue == QueueType::graphics ? "graphics" : "compute")
           << "\"";
    if (queue_id) {
        output << ",\"queue_id\":" << *queue_id;
    }
    output << ",\"dwords\":" << submission.dwords_consumed
           << ",\"packets\":" << submission.packets_decoded << ",\"commands\":[";
    bool first = true;
    for (const auto& command : submission.commands) {
        if (!std::exchange(first, false)) {
            output << ",";
        }
        write_command(output, command);
    }
    output << "]}\n";
}

} // namespace nyxora::gpu::pm4
