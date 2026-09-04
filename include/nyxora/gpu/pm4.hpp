#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nyxora::gpu::pm4 {

enum class PacketType : std::uint8_t { type0 = 0, type1 = 1, type2 = 2, type3 = 3 };

enum class Type3Opcode : std::uint8_t {
    nop = 0x10,
    dispatch_direct = 0x15,
    draw_index_auto = 0x2d,
    num_instances = 0x2f,
    set_config_reg = 0x68,
    set_context_reg = 0x69,
    set_sh_reg = 0x76,
    set_uconfig_reg = 0x79,
};

enum class QueueType : std::uint8_t { graphics, compute };

enum class ShaderStage : std::uint8_t { vertex, pixel, compute };

enum class RegisterSpace : std::uint8_t {
    type0,
    config,
    context,
    shader_graphics,
    shader_compute,
    uconfig,
};

struct PacketView {
    PacketType type{};
    std::uint32_t header{};
    std::uint16_t type0_base{};
    std::uint8_t type3_opcode{};
    bool predicate{};
    bool compute{};
    std::span<const std::uint32_t> payload;
};

class Decoder {
public:
    explicit Decoder(std::span<const std::uint32_t> stream) : stream_(stream) {}

    [[nodiscard]] bool done() const noexcept { return cursor_ == stream_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    PacketView next();

private:
    std::span<const std::uint32_t> stream_;
    std::size_t cursor_{};
};

std::vector<PacketView> decode_all(std::span<const std::uint32_t> stream);

struct RegisterWrite {
    RegisterSpace space{};
    std::uint32_t address{};
    std::uint32_t value{};
};

struct SetNumInstances {
    std::uint32_t count{};
};

struct ShaderProgram {
    ShaderStage stage{};
    std::uint64_t address{};
    std::optional<std::uint32_t> resource1;
    std::optional<std::uint32_t> resource2;
};

struct DrawIndexAuto {
    std::uint32_t index_count{};
    std::uint32_t initiator{};
    std::optional<ShaderProgram> vertex_shader;
    std::optional<ShaderProgram> pixel_shader;
};

struct DispatchDirect {
    std::uint32_t groups_x{};
    std::uint32_t groups_y{};
    std::uint32_t groups_z{};
    std::uint32_t initiator{};
    std::optional<ShaderProgram> compute_shader;
};

using Command = std::variant<RegisterWrite, SetNumInstances, DrawIndexAuto, DispatchDirect>;

struct Submission {
    std::size_t dwords_consumed{};
    std::size_t packets_decoded{};
    std::vector<Command> commands;
};

class State {
public:
    [[nodiscard]] std::optional<std::uint32_t> register_value(RegisterSpace space,
                                                              std::uint32_t address) const;
    [[nodiscard]] std::size_t register_count(RegisterSpace space) const noexcept;
    [[nodiscard]] std::optional<ShaderProgram> shader_program(ShaderStage stage) const;
    [[nodiscard]] std::optional<std::uint32_t> num_instances() const noexcept {
        return num_instances_;
    }

private:
    friend class Processor;

    static constexpr std::size_t register_space_count =
        static_cast<std::size_t>(RegisterSpace::uconfig) + 1U;
    using RegisterMap = std::unordered_map<std::uint32_t, std::uint32_t>;

    [[nodiscard]] static std::size_t index(RegisterSpace space) noexcept;
    void apply(const Command& command);

    std::array<RegisterMap, register_space_count> registers_;
    std::optional<std::uint32_t> num_instances_;
};

class Processor {
public:
    explicit Processor(QueueType queue_type) : queue_type_(queue_type) {}

    [[nodiscard]] const State& state() const noexcept { return state_; }
    Submission process(std::span<const std::uint32_t> stream);
    void reset() noexcept { state_ = {}; }

private:
    QueueType queue_type_;
    State state_;
};

} // namespace nyxora::gpu::pm4
