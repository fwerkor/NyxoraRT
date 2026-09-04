#include "test.hpp"
#include "pm4_fixture.hpp"
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


NYXORA_TEST(pm4_processor_tracks_register_spaces_and_shader_banks) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 13> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_config_reg, 3), 0x10, 0x1111, 0x2222,
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2), 0x20, 0x3333,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x30, 0x4444,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2, 0x2), 0x31, 0x5555,
    };

    Processor processor(QueueType::graphics);
    const auto submission = processor.process(stream);
    NYXORA_CHECK(submission.packets_decoded == 4);
    NYXORA_CHECK(submission.dwords_consumed == stream.size());
    NYXORA_CHECK(submission.commands.size() == 5);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::config, 0x2010) == 0x1111U);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::config, 0x2011) == 0x2222U);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::context, 0xa020) == 0x3333U);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::shader_graphics, 0x2c30) ==
                 0x4444U);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::shader_compute, 0x2c31) ==
                 0x5555U);
}

NYXORA_TEST(pm4_processor_emits_draw_and_dispatch_commands) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 10> stream{
        nyxora::test::pm4_packet3(Type3Opcode::num_instances, 1), 3,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 36, 0x2,
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 4, 0x2), 8, 4, 2, 0x1,
    };

    Processor processor(QueueType::graphics);
    const auto submission = processor.process(stream);
    NYXORA_CHECK(submission.commands.size() == 3);
    NYXORA_CHECK(processor.state().num_instances() == 3U);

    const auto* instances = std::get_if<SetNumInstances>(&submission.commands[0]);
    const auto* draw = std::get_if<DrawIndexAuto>(&submission.commands[1]);
    const auto* dispatch = std::get_if<DispatchDirect>(&submission.commands[2]);
    NYXORA_CHECK(instances != nullptr && instances->count == 3U);
    NYXORA_CHECK(draw != nullptr && draw->index_count == 36U && draw->initiator == 0x2U);
    NYXORA_CHECK(dispatch != nullptr && dispatch->groups_x == 8U && dispatch->groups_y == 4U &&
                 dispatch->groups_z == 2U && dispatch->initiator == 0x1U);
}

NYXORA_TEST(pm4_processor_tracks_type0_and_uconfig_writes) {
    using namespace nyxora::gpu::pm4;
    constexpr std::uint32_t type0_header = (1U << 16U) | 0x1234U;
    const std::array<std::uint32_t, 6> stream{
        type0_header, 0xaaaa, 0xbbbb,
        nyxora::test::pm4_packet3(Type3Opcode::set_uconfig_reg, 2), 0x3, 0xcccc,
    };

    Processor processor(QueueType::graphics);
    (void)processor.process(stream);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::type0, 0x1234) == 0xaaaaU);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::type0, 0x1235) == 0xbbbbU);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::uconfig, 0xc003) == 0xccccU);
    NYXORA_CHECK(processor.state().register_count(RegisterSpace::type0) == 2U);
}

NYXORA_TEST(pm4_processor_is_transactional_on_invalid_submission) {
    using namespace nyxora::gpu::pm4;
    Processor processor(QueueType::graphics);
    const std::array<std::uint32_t, 3> initial{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2), 0x1, 0x12345678,
    };
    (void)processor.process(initial);

    const std::array<std::uint32_t, 6> invalid{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2), 0x2, 0xdeadbeef,
        nyxora::test::pm4_packet3(static_cast<Type3Opcode>(0x7b), 2), 0, 0,
    };
    bool threw = false;
    try {
        (void)processor.process(invalid);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    NYXORA_CHECK(threw);
    NYXORA_CHECK(processor.state().register_value(RegisterSpace::context, 0xa001) ==
                 0x12345678U);
    NYXORA_CHECK(!processor.state().register_value(RegisterSpace::context, 0xa002).has_value());
}

NYXORA_TEST(pm4_processor_rejects_unsupported_state_semantics) {
    using namespace nyxora::gpu::pm4;
    Processor graphics(QueueType::graphics);

    const std::array<std::uint32_t, 3> predicated_set{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2, 0x1), 0, 1,
    };
    bool predicated = false;
    try {
        (void)graphics.process(predicated_set);
    } catch (const std::runtime_error&) {
        predicated = true;
    }
    NYXORA_CHECK(predicated);

    const std::array<std::uint32_t, 3> indexed_set{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x10000, 1,
    };
    bool indexed = false;
    try {
        (void)graphics.process(indexed_set);
    } catch (const std::runtime_error&) {
        indexed = true;
    }
    NYXORA_CHECK(indexed);

    const std::array<std::uint32_t, 3> overflow_set{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 2), 0x3ff, 1,
    };
    bool overflow = false;
    try {
        (void)graphics.process(overflow_set);
    } catch (const std::runtime_error&) {
        overflow = true;
    }
    NYXORA_CHECK(!overflow);

    const std::array<std::uint32_t, 4> overflow_two_values{
        nyxora::test::pm4_packet3(Type3Opcode::set_context_reg, 3), 0x3ff, 1, 2,
    };
    try {
        (void)graphics.process(overflow_two_values);
    } catch (const std::runtime_error&) {
        overflow = true;
    }
    NYXORA_CHECK(overflow);

    Processor compute(QueueType::compute);
    const std::array<std::uint32_t, 3> draw{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 3, 0,
    };
    bool draw_on_compute = false;
    try {
        (void)compute.process(draw);
    } catch (const std::runtime_error&) {
        draw_on_compute = true;
    }
    NYXORA_CHECK(draw_on_compute);
}

NYXORA_TEST(pm4_processor_reset_clears_tracked_state) {
    using namespace nyxora::gpu::pm4;
    Processor processor(QueueType::graphics);
    const std::array<std::uint32_t, 5> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_config_reg, 2), 1, 2,
        nyxora::test::pm4_packet3(Type3Opcode::num_instances, 1), 7,
    };
    (void)processor.process(stream);
    processor.reset();
    NYXORA_CHECK(processor.state().register_count(RegisterSpace::config) == 0U);
    NYXORA_CHECK(!processor.state().num_instances().has_value());
}

NYXORA_TEST(pm4_processor_validates_packet_specific_payloads_and_type0_range) {
    using namespace nyxora::gpu::pm4;
    Processor graphics(QueueType::graphics);

    const std::array<std::uint32_t, 2> short_draw{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 1), 4,
    };
    bool rejected = false;
    try {
        (void)graphics.process(short_draw);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);

    const std::array<std::uint32_t, 4> short_dispatch{
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 3), 1, 1, 1,
    };
    rejected = false;
    try {
        (void)graphics.process(short_dispatch);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);

    constexpr std::uint32_t type0_header = (1U << 16U) | 0xffffU;
    const std::array<std::uint32_t, 3> overflowing_type0{type0_header, 1, 2};
    rejected = false;
    try {
        (void)graphics.process(overflowing_type0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
}

NYXORA_TEST(pm4_compute_queue_routes_sh_registers_to_compute_state) {
    using namespace nyxora::gpu::pm4;
    Processor compute(QueueType::compute);
    const std::array<std::uint32_t, 3> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x40, 0x1234,
    };
    (void)compute.process(stream);
    NYXORA_CHECK(compute.state().register_value(RegisterSpace::shader_compute, 0x2c40) ==
                 0x1234U);
    NYXORA_CHECK(!compute.state().register_value(RegisterSpace::shader_graphics, 0x2c40));
}

NYXORA_TEST(pm4_processor_discovers_graphics_shader_programs_at_draw_time) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 15> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 5),
        0x08,
        0x3456789a,
        0x12,
        0x11112222,
        0x33334444,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 5),
        0x48,
        0xabcdef01,
        0x23,
        0x55556666,
        0x77778888,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2),
        6,
        0,
    };

    Processor processor(QueueType::graphics);
    const auto submission = processor.process(stream);
    const auto* draw = std::get_if<DrawIndexAuto>(&submission.commands.back());
    NYXORA_CHECK(draw != nullptr);
    NYXORA_CHECK(draw->pixel_shader.has_value());
    NYXORA_CHECK(draw->pixel_shader->stage == ShaderStage::pixel);
    NYXORA_CHECK(draw->pixel_shader->address == 0x123456789a00ULL);
    NYXORA_CHECK(draw->pixel_shader->resource1 == 0x11112222U);
    NYXORA_CHECK(draw->pixel_shader->resource2 == 0x33334444U);
    NYXORA_CHECK(draw->vertex_shader.has_value());
    NYXORA_CHECK(draw->vertex_shader->stage == ShaderStage::vertex);
    NYXORA_CHECK(draw->vertex_shader->address == 0x23abcdef0100ULL);
    NYXORA_CHECK(draw->vertex_shader->resource1 == 0x55556666U);
    NYXORA_CHECK(draw->vertex_shader->resource2 == 0x77778888U);
}

NYXORA_TEST(pm4_processor_freezes_shader_binding_for_each_draw) {
    using namespace nyxora::gpu::pm4;
    Processor processor(QueueType::graphics);
    const std::array<std::uint32_t, 4> initial_shader{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x48, 0x100, 0x1,
    };
    (void)processor.process(initial_shader);

    const std::array<std::uint32_t, 10> stream{
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 3, 0,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3), 0x48, 0x200, 0x2,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 6, 0,
    };
    const auto submission = processor.process(stream);

    const auto* first_draw = std::get_if<DrawIndexAuto>(&submission.commands[0]);
    const auto* second_draw = std::get_if<DrawIndexAuto>(&submission.commands[3]);
    NYXORA_CHECK(first_draw != nullptr && first_draw->vertex_shader.has_value());
    NYXORA_CHECK(second_draw != nullptr && second_draw->vertex_shader.has_value());
    NYXORA_CHECK(first_draw->vertex_shader->address == 0x10000010000ULL);
    NYXORA_CHECK(second_draw->vertex_shader->address == 0x20000020000ULL);
    NYXORA_CHECK(processor.state().shader_program(ShaderStage::vertex)->address ==
                 second_draw->vertex_shader->address);
}

NYXORA_TEST(pm4_processor_discovers_compute_shader_and_ignores_hi_control_bits) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 13> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3, 0x2),
        0x20c,
        0x12345678,
        0x1ab,
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 3, 0x2),
        0x212,
        0x01020304,
        0x05060708,
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 4, 0x2),
        8,
        2,
        1,
        1,
    };

    Processor processor(QueueType::compute);
    const auto submission = processor.process(stream);
    const auto* dispatch = std::get_if<DispatchDirect>(&submission.commands.back());
    NYXORA_CHECK(dispatch != nullptr && dispatch->compute_shader.has_value());
    NYXORA_CHECK(dispatch->compute_shader->stage == ShaderStage::compute);
    NYXORA_CHECK(dispatch->compute_shader->address == 0xab1234567800ULL);
    NYXORA_CHECK(dispatch->compute_shader->resource1 == 0x01020304U);
    NYXORA_CHECK(dispatch->compute_shader->resource2 == 0x05060708U);
}

NYXORA_TEST(pm4_processor_rejects_incomplete_shader_address_transactionally) {
    using namespace nyxora::gpu::pm4;
    Processor processor(QueueType::graphics);
    const std::array<std::uint32_t, 6> stream{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x08, 0x1234,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 3, 0,
    };

    bool rejected = false;
    try {
        (void)processor.process(stream);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    NYXORA_CHECK(rejected);
    NYXORA_CHECK(!processor.state().register_value(RegisterSpace::shader_graphics, 0x2c08));
    NYXORA_CHECK(!processor.state().shader_program(ShaderStage::pixel));
}

NYXORA_TEST(pm4_processor_allows_shader_address_programming_across_submissions) {
    using namespace nyxora::gpu::pm4;
    Processor processor(QueueType::graphics);
    const std::array<std::uint32_t, 3> lo_only{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x48, 0x1234,
    };
    (void)processor.process(lo_only);

    const std::array<std::uint32_t, 6> finish_and_draw{
        nyxora::test::pm4_packet3(Type3Opcode::set_sh_reg, 2), 0x49, 0x5,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_auto, 2), 3, 0,
    };
    const auto submission = processor.process(finish_and_draw);
    const auto* draw = std::get_if<DrawIndexAuto>(&submission.commands.back());
    NYXORA_CHECK(draw != nullptr && draw->vertex_shader.has_value());
    NYXORA_CHECK(draw->vertex_shader->address == 0x50000123400ULL);
}
