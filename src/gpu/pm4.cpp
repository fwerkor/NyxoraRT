#include "nyxora/gpu/pm4.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

namespace nyxora::gpu::pm4 {
namespace {

constexpr std::uint32_t config_start = 0x2000;
constexpr std::uint32_t config_end = 0x2c00;
constexpr std::uint32_t shader_start = 0x2c00;
constexpr std::uint32_t shader_end = 0x3000;
constexpr std::uint32_t context_start = 0xa000;
constexpr std::uint32_t context_end = 0xa400;
constexpr std::uint32_t uconfig_start = 0xc000;
constexpr std::uint32_t uconfig_end = 0xc400;

[[nodiscard]] Type3Opcode opcode_of(const PacketView& packet) {
    return static_cast<Type3Opcode>(packet.type3_opcode);
}

void require_payload(const PacketView& packet, std::size_t expected) {
    if (packet.payload.size() != expected) {
        throw std::runtime_error("invalid PM4 type-3 payload size for opcode " +
                                 std::to_string(packet.type3_opcode));
    }
}

void require_unpredicated(const PacketView& packet) {
    if (packet.predicate) {
        throw std::runtime_error("predicated PM4 packets require predication state");
    }
}

void append_register_writes(const PacketView& packet, RegisterSpace space,
                            std::uint32_t range_start, std::uint32_t range_end,
                            std::vector<Command>& commands) {
    if (packet.payload.size() < 2) {
        throw std::runtime_error("PM4 set-register packet has no register values");
    }
    if ((packet.payload[0] & 0xffff0000U) != 0) {
        throw std::runtime_error("indexed PM4 set-register packets are not supported yet");
    }

    const auto offset = packet.payload[0];
    const auto value_count = packet.payload.size() - 1U;
    if (offset >= range_end - range_start || value_count > range_end - range_start - offset) {
        throw std::runtime_error("PM4 set-register packet exceeds its register window");
    }

    const auto first = range_start + offset;
    for (std::size_t i = 0; i < value_count; ++i) {
        commands.emplace_back(
            RegisterWrite{.space = space,
                          .address = first + static_cast<std::uint32_t>(i),
                          .value = packet.payload[i + 1U]});
    }
}

} // namespace

PacketView Decoder::next() {
    if (done()) {
        throw std::out_of_range("PM4 decoder is at end of stream");
    }

    const auto header = stream_[cursor_];
    const auto type = static_cast<PacketType>((header >> 30U) & 0x3U);

    std::size_t payload_words = 0;
    switch (type) {
    case PacketType::type0:
    case PacketType::type3:
        payload_words = static_cast<std::size_t>((header >> 16U) & 0x3fffU) + 1U;
        break;
    case PacketType::type2:
        payload_words = 0;
        break;
    case PacketType::type1:
        throw std::runtime_error("PM4 type-1 packets are not supported yet");
    }

    const auto remaining = stream_.size() - cursor_ - 1U;
    if (payload_words > remaining) {
        throw std::runtime_error("truncated PM4 packet");
    }

    PacketView packet{
        .type = type,
        .header = header,
        .type0_base = 0,
        .type3_opcode = 0,
        .predicate = false,
        .compute = false,
        .payload = stream_.subspan(cursor_ + 1U, payload_words),
    };

    if (type == PacketType::type0) {
        packet.type0_base = static_cast<std::uint16_t>(header & 0xffffU);
    } else if (type == PacketType::type3) {
        packet.type3_opcode = static_cast<std::uint8_t>((header >> 8U) & 0xffU);
        packet.predicate = (header & 0x1U) != 0;
        packet.compute = (header & 0x2U) != 0;
    }

    cursor_ += 1U + payload_words;
    return packet;
}

std::vector<PacketView> decode_all(std::span<const std::uint32_t> stream) {
    Decoder decoder(stream);
    std::vector<PacketView> packets;
    while (!decoder.done()) {
        packets.push_back(decoder.next());
    }
    return packets;
}

std::size_t State::index(RegisterSpace space) noexcept {
    return static_cast<std::size_t>(space);
}

std::optional<std::uint32_t> State::register_value(RegisterSpace space,
                                                   std::uint32_t address) const {
    const auto& registers = registers_[index(space)];
    if (const auto it = registers.find(address); it != registers.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::size_t State::register_count(RegisterSpace space) const noexcept {
    return registers_[index(space)].size();
}

void State::apply(const Command& command) {
    std::visit(
        [this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RegisterWrite>) {
                registers_[index(value.space)][value.address] = value.value;
            } else if constexpr (std::is_same_v<T, SetNumInstances>) {
                num_instances_ = value.count;
            }
        },
        command);
}

Submission Processor::process(std::span<const std::uint32_t> stream) {
    Submission submission{.dwords_consumed = stream.size(), .packets_decoded = 0, .commands = {}};
    Decoder decoder(stream);

    while (!decoder.done()) {
        const auto packet = decoder.next();
        ++submission.packets_decoded;

        switch (packet.type) {
        case PacketType::type0:
            if (packet.payload.size() > 0x10000U - static_cast<std::uint32_t>(packet.type0_base)) {
                throw std::runtime_error("PM4 type-0 packet exceeds the register address space");
            }
            for (std::size_t i = 0; i < packet.payload.size(); ++i) {
                submission.commands.emplace_back(RegisterWrite{
                    .space = RegisterSpace::type0,
                    .address = static_cast<std::uint32_t>(packet.type0_base) +
                               static_cast<std::uint32_t>(i),
                    .value = packet.payload[i],
                });
            }
            break;
        case PacketType::type2:
            break;
        case PacketType::type1:
            throw std::runtime_error("PM4 type-1 packets are not supported yet");
        case PacketType::type3:
            switch (opcode_of(packet)) {
            case Type3Opcode::nop:
                break;
            case Type3Opcode::set_config_reg:
                require_unpredicated(packet);
                append_register_writes(packet, RegisterSpace::config, config_start, config_end,
                                       submission.commands);
                break;
            case Type3Opcode::set_context_reg:
                require_unpredicated(packet);
                append_register_writes(packet, RegisterSpace::context, context_start, context_end,
                                       submission.commands);
                break;
            case Type3Opcode::set_sh_reg:
                require_unpredicated(packet);
                append_register_writes(packet,
                                       queue_type_ == QueueType::compute || packet.compute
                                           ? RegisterSpace::shader_compute
                                           : RegisterSpace::shader_graphics,
                                       shader_start, shader_end, submission.commands);
                break;
            case Type3Opcode::set_uconfig_reg:
                require_unpredicated(packet);
                append_register_writes(packet, RegisterSpace::uconfig, uconfig_start, uconfig_end,
                                       submission.commands);
                break;
            case Type3Opcode::num_instances:
                require_unpredicated(packet);
                if (queue_type_ != QueueType::graphics || packet.compute) {
                    throw std::runtime_error("NUM_INSTANCES is only supported on the graphics path");
                }
                require_payload(packet, 1);
                submission.commands.emplace_back(SetNumInstances{.count = packet.payload[0]});
                break;
            case Type3Opcode::draw_index_auto:
                require_unpredicated(packet);
                if (queue_type_ != QueueType::graphics || packet.compute) {
                    throw std::runtime_error("DRAW_INDEX_AUTO is only supported on the graphics path");
                }
                require_payload(packet, 2);
                submission.commands.emplace_back(DrawIndexAuto{
                    .index_count = packet.payload[0], .initiator = packet.payload[1]});
                break;
            case Type3Opcode::dispatch_direct:
                require_unpredicated(packet);
                require_payload(packet, 4);
                submission.commands.emplace_back(DispatchDirect{
                    .groups_x = packet.payload[0],
                    .groups_y = packet.payload[1],
                    .groups_z = packet.payload[2],
                    .initiator = packet.payload[3],
                });
                break;
            default:
                throw std::runtime_error("unsupported PM4 type-3 opcode " +
                                         std::to_string(packet.type3_opcode));
            }
            break;
        }
    }

    for (const auto& command : submission.commands) {
        state_.apply(command);
    }
    return submission;
}

} // namespace nyxora::gpu::pm4
