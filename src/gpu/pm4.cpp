#include "nyxora/gpu/pm4.hpp"

#include <stdexcept>

namespace nyxora::gpu::pm4 {

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

} // namespace nyxora::gpu::pm4
