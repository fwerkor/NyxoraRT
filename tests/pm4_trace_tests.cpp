#include "test.hpp"
#include "pm4_fixture.hpp"
#include "nyxora/gpu/pm4_trace.hpp"

#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string encode_le(std::span<const std::uint32_t> words) {
    std::string bytes;
    for (const auto word : words) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<char>((word >> shift) & 0xffU));
        }
    }
    return bytes;
}

template <typename F>
bool fails(F&& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

NYXORA_TEST(pm4_trace_reader_decodes_little_endian_words) {
    const std::array<std::uint32_t, 3> expected{0xc0002a00U, 0x01020304U, 0xabcdef89U};
    std::istringstream input(encode_le(expected), std::ios::in | std::ios::binary);
    const auto words = nyxora::gpu::pm4::read_binary_trace(input);
    NYXORA_CHECK(words.size() == 3);
    NYXORA_CHECK(words[0] == expected[0]);
    NYXORA_CHECK(words[1] == expected[1]);
    NYXORA_CHECK(words[2] == expected[2]);
}

NYXORA_TEST(pm4_trace_reader_rejects_empty_truncated_and_oversized_files) {
    std::istringstream empty("", std::ios::in | std::ios::binary);
    NYXORA_CHECK(fails([&] { (void)nyxora::gpu::pm4::read_binary_trace(empty); }));

    const std::array<std::uint32_t, 1> one{0x12345678U};
    auto bad_bytes = encode_le(one);
    bad_bytes.push_back('x');
    std::istringstream truncated(bad_bytes, std::ios::in | std::ios::binary);
    NYXORA_CHECK(fails([&] { (void)nyxora::gpu::pm4::read_binary_trace(truncated); }));

    const std::array<std::uint32_t, 3> three{1, 2, 3};
    std::istringstream oversized(encode_le(three), std::ios::in | std::ios::binary);
    NYXORA_CHECK(fails([&] { (void)nyxora::gpu::pm4::read_binary_trace(oversized, 2); }));
    std::istringstream within_limit(encode_le(three), std::ios::in | std::ios::binary);
    NYXORA_CHECK(nyxora::gpu::pm4::read_binary_trace(within_limit, 3).size() == 3);
}

NYXORA_TEST(pm4_trace_emits_json_with_precision_safe_guest_addresses) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 8> raw{
        nyxora::test::pm4_packet3(Type3Opcode::index_type, 1), 1,
        nyxora::test::pm4_packet3(Type3Opcode::draw_index_2, 5),
        8, 0x89abcdef, 0x12345678, 8, 0,
    };
    std::istringstream input(encode_le(raw), std::ios::in | std::ios::binary);
    const auto decoded = read_binary_trace(input);
    Processor gpu(QueueType::graphics);
    const auto submission = gpu.process(decoded);

    std::ostringstream output;
    write_submission_json(output, submission, QueueType::graphics);
    const auto json = output.str();
    NYXORA_CHECK(json.find("\"queue\":\"graphics\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"dwords\":8,\"packets\":2") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"index_type\",\"raw\":1") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"draw_index_2\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"index_address\":\"0x1234567889abcdef\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"index_type\":1") != std::string::npos);
    NYXORA_CHECK(json.find("\"vertex_shader\":null,\"pixel_shader\":null") !=
                 std::string::npos);
    NYXORA_CHECK(!json.empty() && json.back() == '\n');
}


NYXORA_TEST(pm4_trace_json_handles_all_normalized_commands) {
    using namespace nyxora::gpu::pm4;
    Submission submission{};
    const std::array<RegisterSpace, 6> spaces{
        RegisterSpace::type0, RegisterSpace::config, RegisterSpace::context,
        RegisterSpace::shader_graphics, RegisterSpace::shader_compute, RegisterSpace::uconfig,
    };
    for (const auto space : spaces) {
        submission.commands.emplace_back(RegisterWrite{space, 0x100, 19});
    }
    submission.commands.emplace_back(SetNumInstances{3});
    const ShaderProgram vertex{ShaderStage::vertex, 0x123456789ULL, 12, 34};
    const ShaderProgram pixel{ShaderStage::pixel, 0x1000ULL, std::nullopt, 99};
    const ShaderProgram compute{ShaderStage::compute, 0x8000ULL, 8, std::nullopt};
    submission.commands.emplace_back(DrawIndexAuto{6, 1, vertex, pixel});
    submission.commands.emplace_back(DrawIndex2{8, 0x987654321ULL, 8, 0, std::nullopt,
                                                 vertex, pixel});
    submission.commands.emplace_back(DispatchDirect{2, 3, 4, 1, compute});

    std::ostringstream out;
    write_submission_json(out, submission, QueueType::graphics);
    const auto json = out.str();
    NYXORA_CHECK(json.find("\"space\":\"type0\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"space\":\"config\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"space\":\"context\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"space\":\"shader_graphics\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"space\":\"shader_compute\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"space\":\"uconfig\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"num_instances\",\"count\":3") != std::string::npos);
    NYXORA_CHECK(json.find("\"kind\":\"draw_index_auto\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"index_type\":null") != std::string::npos);
    NYXORA_CHECK(json.find("\"stage\":\"vertex\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"stage\":\"pixel\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"stage\":\"compute\"") != std::string::npos);
    NYXORA_CHECK(json.find("\"resource1\":null") != std::string::npos);
    NYXORA_CHECK(json.find("\"resource2\":null") != std::string::npos);
    NYXORA_CHECK(json.find("\"compute_shader\":{") != std::string::npos);
}

NYXORA_TEST(pm4_trace_reader_rejects_invalid_stream_limit_and_failure) {
    std::istringstream empty("", std::ios::in | std::ios::binary);
    NYXORA_CHECK(fails([&] { (void)nyxora::gpu::pm4::read_binary_trace(empty, 0); }));

    const std::array<std::uint32_t, 1> one{1};
    std::istringstream failed(encode_le(one), std::ios::in | std::ios::binary);
    failed.setstate(std::ios::badbit);
    NYXORA_CHECK(fails([&] { (void)nyxora::gpu::pm4::read_binary_trace(failed); }));
}

NYXORA_TEST(pm4_trace_serializes_compute_dispatch_and_queue_id) {
    using namespace nyxora::gpu::pm4;
    const std::array<std::uint32_t, 5> stream{
        nyxora::test::pm4_packet3(Type3Opcode::dispatch_direct, 4, 2),
        4, 8, 16, 1,
    };
    Processor gpu(QueueType::compute);
    const auto submission = gpu.process(stream);
    std::ostringstream output;
    write_submission_json(output, submission, QueueType::compute, 7);
    const auto json = output.str();
    NYXORA_CHECK(json.find("\"queue\":\"compute\",\"queue_id\":7") != std::string::npos);
    NYXORA_CHECK(json.find("\"groups\":[4,8,16]") != std::string::npos);
    NYXORA_CHECK(json.find("\"compute_shader\":null") != std::string::npos);
}
