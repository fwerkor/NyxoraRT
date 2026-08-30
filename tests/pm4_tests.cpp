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

NYXORA_TEST(pm4_decoder_covers_type0_type2_and_decode_all) {
    constexpr std::uint32_t type0_header = (0U << 30U) | (0U << 16U) | 0x1234U;
    constexpr std::uint32_t type2_header = (2U << 30U);
    constexpr std::uint32_t type3_header = (3U << 30U) | (0U << 16U) | (0x55U << 8U) | 0x1U;
    const std::array<std::uint32_t, 5> stream{
        type0_header, 0xabcdef01U, type2_header, type3_header, 0x42U};

    const auto packets = nyxora::gpu::pm4::decode_all(stream);
    NYXORA_CHECK(packets.size() == 3);
    NYXORA_CHECK(packets[0].type == nyxora::gpu::pm4::PacketType::type0);
    NYXORA_CHECK(packets[0].type0_base == 0x1234U);
    NYXORA_CHECK(packets[0].payload.size() == 1);
    NYXORA_CHECK(packets[1].type == nyxora::gpu::pm4::PacketType::type2);
    NYXORA_CHECK(packets[1].payload.empty());
    NYXORA_CHECK(packets[2].type == nyxora::gpu::pm4::PacketType::type3);
    NYXORA_CHECK(packets[2].type3_opcode == 0x55U);
    NYXORA_CHECK(packets[2].predicate);
    NYXORA_CHECK(!packets[2].compute);
}

NYXORA_TEST(pm4_decoder_rejects_type1_and_next_after_end) {
    const std::array<std::uint32_t, 1> type1{1U << 30U};
    bool unsupported = false;
    try {
        nyxora::gpu::pm4::Decoder decoder(type1);
        (void)decoder.next();
    } catch (const std::runtime_error&) {
        unsupported = true;
    }
    NYXORA_CHECK(unsupported);

    const std::array<std::uint32_t, 1> type2{2U << 30U};
    nyxora::gpu::pm4::Decoder decoder(type2);
    (void)decoder.next();
    bool at_end = false;
    try {
        (void)decoder.next();
    } catch (const std::out_of_range&) {
        at_end = true;
    }
    NYXORA_CHECK(at_end);
}
