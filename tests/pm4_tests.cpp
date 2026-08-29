#include "test.hpp"
#include "asteria/gpu/pm4.hpp"

#include <array>

ASTERIA_TEST(pm4_decoder_splits_type3_packets_without_interpreting_payload) {
    constexpr std::uint32_t opcode = 0x37;
    constexpr std::uint32_t payload_words = 3;
    constexpr std::uint32_t header =
        (3U << 30U) | ((payload_words - 1U) << 16U) | (opcode << 8U) | 0x2U;
    const std::array<std::uint32_t, 4> stream{header, 0x11, 0x22, 0x33};

    asteria::gpu::pm4::Decoder decoder(stream);
    const auto packet = decoder.next();
    ASTERIA_CHECK(packet.type == asteria::gpu::pm4::PacketType::type3);
    ASTERIA_CHECK(packet.type3_opcode == opcode);
    ASTERIA_CHECK(packet.compute);
    ASTERIA_CHECK(!packet.predicate);
    ASTERIA_CHECK(packet.payload.size() == payload_words);
    ASTERIA_CHECK(packet.payload[2] == 0x33);
    ASTERIA_CHECK(decoder.done());
}

ASTERIA_TEST(pm4_decoder_rejects_truncated_packet) {
    constexpr std::uint32_t header = (3U << 30U) | (4U << 16U) | (0x10U << 8U);
    const std::array<std::uint32_t, 2> stream{header, 0};
    bool threw = false;
    try {
        asteria::gpu::pm4::Decoder decoder(stream);
        (void)decoder.next();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASTERIA_CHECK(threw);
}
