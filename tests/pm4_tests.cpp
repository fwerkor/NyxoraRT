#include "test.hpp"
#include "nyxora/gpu/pm4.hpp"

#include <array>

NYXORA_TEST(pm4_decoder_splits_type3_packets_without_interpreting_payload) {
    constexpr std::uint32_t opcode = 0x37;
    constexpr std::uint32_t payload_words = 3;
    constexpr std::uint32_t header =
        (3U << 30U) | ((payload_words - 1U) << 16U) | (opcode << 8U) | 0x2U;
    const std::array<std::uint32_t, 4> stream{header, 0x11, 0x22, 0x33};

    nyxora::gpu::pm4::Decoder decoder(stream);
    const auto packet = decoder.next();
    NYXORA_CHECK(packet.type == nyxora::gpu::pm4::PacketType::type3);
    NYXORA_CHECK(packet.type3_opcode == opcode);
    NYXORA_CHECK(packet.compute);
    NYXORA_CHECK(!packet.predicate);
    NYXORA_CHECK(packet.payload.size() == payload_words);
    NYXORA_CHECK(packet.payload[2] == 0x33);
    NYXORA_CHECK(decoder.done());
}

NYXORA_TEST(pm4_decoder_rejects_truncated_packet) {
    constexpr std::uint32_t header = (3U << 30U) | (4U << 16U) | (0x10U << 8U);
    const std::array<std::uint32_t, 2> stream{header, 0};
    bool threw = false;
    try {
        nyxora::gpu::pm4::Decoder decoder(stream);
        (void)decoder.next();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    NYXORA_CHECK(threw);
}
