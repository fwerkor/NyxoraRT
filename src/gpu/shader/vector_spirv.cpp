#include "nyxora/gpu/shader/vector.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nyxora::gpu::shader {
namespace {

enum class Op : std::uint16_t {
    nop = 0, memory_model = 14, entry_point = 15, execution_mode = 16,
    capability = 17, type_void = 19, type_bool = 20, type_int = 21, type_array = 28,
    type_struct = 30, type_pointer = 32, type_function = 33,
    constant = 43, function = 54, function_end = 56, variable = 59,
    load = 61, store = 62, access_chain = 65, decorate = 71,
    member_decorate = 72, iadd = 128, isub = 130, select = 169,
    uless_than = 176, ugreater_than = 172,
    shift_right_logical = 194, shift_left_logical = 196,
    bitwise_or = 197, bitwise_xor = 198, bitwise_and = 199,
    bitwise_not = 200, label = 248, ret = 253,
};

void emit(std::vector<std::uint32_t>& out, Op op,
          std::initializer_list<std::uint32_t> operands) {
    out.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16U) |
                  static_cast<std::uint16_t>(op));
    out.insert(out.end(), operands.begin(), operands.end());
}

class VectorModule {
public:
    VectorModule() {
        void_type_ = id();
        uint_type_ = id();
        bool_type_ = id();
        array_length_ = constant(diagnostic_vgpr_count * diagnostic_wave_lanes);
        array_type_ = id();
        block_type_ = id();
        buffer_pointer_type_ = id();
        element_pointer_type_ = id();
        input_pointer_type_ = id();
        buffer_ = id();
        lane_input_ = id();
        function_type_ = id();
        function_ = id();
        label_ = id();
        lane_id_ = id();
        zero_ = constant(0);
        shift_mask_ = constant(31);
    }

    [[nodiscard]] std::uint32_t constant(std::uint32_t value) {
        if (const auto it = constants_by_value_.find(value); it != constants_by_value_.end()) {
            return it->second;
        }
        const auto result = id();
        constants_by_value_.emplace(value, result);
        emit(constants_, Op::constant, {uint_type_, result, value});
        return result;
    }

    [[nodiscard]] std::uint32_t source(VectorSource source) {
        if (source.kind == VectorSourceKind::constant) {
            return constant(source.value);
        }
        return load_reg(source.value);
    }

    [[nodiscard]] std::uint32_t unary(Op op, std::uint32_t value) {
        const auto result = id();
        emit(body_, op, {uint_type_, result, value});
        return result;
    }

    [[nodiscard]] std::uint32_t binary(Op op, std::uint32_t lhs, std::uint32_t rhs) {
        const auto result = id();
        emit(body_, op, {uint_type_, result, lhs, rhs});
        return result;
    }

    [[nodiscard]] std::uint32_t compare(Op op, std::uint32_t lhs, std::uint32_t rhs) {
        const auto result = id();
        emit(body_, op, {bool_type_, result, lhs, rhs});
        return result;
    }

    [[nodiscard]] std::uint32_t select(std::uint32_t cond, std::uint32_t lhs,
                                       std::uint32_t rhs) {
        const auto result = id();
        emit(body_, Op::select, {uint_type_, result, cond, lhs, rhs});
        return result;
    }

    void store_reg(std::uint32_t reg, std::uint32_t value) {
        const auto pointer = address(reg);
        emit(body_, Op::store, {pointer, value});
    }

    void nop() { emit(body_, Op::nop, {}); }

    [[nodiscard]] std::vector<std::uint32_t> finish() {
        std::vector<std::uint32_t> module{
            0x07230203U, 0x00010300U, 0, next_id_, 0,
        };
        emit(module, Op::capability, {1});       // Shader
        emit(module, Op::memory_model, {0, 1});  // Logical GLSL450
        emit(module, Op::entry_point, {5, function_, 0x6e69616dU, 0, lane_input_});
        emit(module, Op::execution_mode,
             {function_, 17, diagnostic_wave_lanes, 1, 1}); // LocalSize
        emit(module, Op::decorate, {array_type_, 6, 4}); // ArrayStride
        emit(module, Op::decorate, {block_type_, 2}); // Block
        emit(module, Op::member_decorate, {block_type_, 0, 35, 0}); // Offset
        emit(module, Op::decorate, {buffer_, 34, 0}); // DescriptorSet
        emit(module, Op::decorate, {buffer_, 33, 0}); // Binding
        emit(module, Op::decorate, {lane_input_, 11, 29}); // BuiltIn LocalInvocationIndex
        emit(module, Op::type_void, {void_type_});
        emit(module, Op::type_int, {uint_type_, 32, 0});
        emit(module, Op::type_bool, {bool_type_});
        module.insert(module.end(), constants_.begin(), constants_.end());
        emit(module, Op::type_array, {array_type_, uint_type_, array_length_});
        emit(module, Op::type_struct, {block_type_, array_type_});
        emit(module, Op::type_pointer, {buffer_pointer_type_, 12, block_type_});
        emit(module, Op::type_pointer, {element_pointer_type_, 12, uint_type_});
        emit(module, Op::type_pointer, {input_pointer_type_, 1, uint_type_});
        emit(module, Op::type_function, {function_type_, void_type_});
        emit(module, Op::variable, {buffer_pointer_type_, buffer_, 12});
        emit(module, Op::variable, {input_pointer_type_, lane_input_, 1});
        emit(module, Op::function, {void_type_, function_, 0, function_type_});
        emit(module, Op::label, {label_});
        emit(module, Op::load, {uint_type_, lane_id_, lane_input_});
        module.insert(module.end(), body_.begin(), body_.end());
        emit(module, Op::ret, {});
        emit(module, Op::function_end, {});
        return module;
    }

    [[nodiscard]] std::uint32_t clamp_shift(std::uint32_t amount) {
        return binary(Op::bitwise_and, amount, shift_mask_);
    }

private:
    [[nodiscard]] std::uint32_t id() { return next_id_++; }

    [[nodiscard]] std::uint32_t address(std::uint32_t reg) {
        // Register-major storage: index = reg * 32 + LocalInvocationIndex.
        // The fixed reg×32 offset is a module constant (not an integer mul).
        const auto offset = binary(Op::iadd,
                                   constant(reg * diagnostic_wave_lanes), lane_id_);
        const auto pointer = id();
        emit(body_, Op::access_chain, {element_pointer_type_, pointer,
                                       buffer_, zero_, offset});
        return pointer;
    }

    [[nodiscard]] std::uint32_t load_reg(std::uint32_t reg) {
        const auto pointer = address(reg);
        const auto value = id();
        emit(body_, Op::load, {uint_type_, value, pointer});
        return value;
    }

    std::uint32_t next_id_ = 1;
    std::uint32_t void_type_{}, uint_type_{}, bool_type_{};
    std::uint32_t array_length_{}, array_type_{}, block_type_{};
    std::uint32_t buffer_pointer_type_{}, element_pointer_type_{}, input_pointer_type_{};
    std::uint32_t buffer_{}, lane_input_{}, function_type_{}, function_{};
    std::uint32_t label_{}, lane_id_{}, zero_{}, shift_mask_{};
    std::unordered_map<std::uint32_t, std::uint32_t> constants_by_value_;
    std::vector<std::uint32_t> constants_, body_;
};

} // namespace

std::optional<std::vector<std::uint32_t>>
compile_vector_diagnostic_spirv(const VectorProgram& program) {
    if (program.instructions.empty() || program.instructions.size() > 4096 ||
        program.dword_count == 0 || program.dword_count > 65536 ||
        program.instructions.back().op != VectorOp::end_program) {
        return std::nullopt;
    }
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& ins = program.instructions[i];
        if (ins.offset_dwords != cursor || ins.word_count < 1 ||
            ins.word_count > 2 || ins.word_count > program.dword_count - cursor ||
            (ins.op == VectorOp::end_program && i + 1 != program.instructions.size())) {
            return std::nullopt;
        }
        const auto expected = ins.op == VectorOp::end_program || ins.op == VectorOp::nop ? 0U
                            : ins.op == VectorOp::mov_b32 || ins.op == VectorOp::not_b32 ? 1U
                            : 2U;
        if (ins.source_count != expected) {
            return std::nullopt;
        }
        for (std::uint8_t j = 0; j < ins.source_count; ++j) {
            const auto kind = ins.sources[j].kind;
            if (kind != VectorSourceKind::vgpr && kind != VectorSourceKind::constant) {
                return std::nullopt;
            }
            if (kind == VectorSourceKind::vgpr &&
                ins.sources[j].value >= diagnostic_vgpr_count) {
                return std::nullopt;
            }
        }
        cursor += ins.word_count;
    }
    if (cursor != program.dword_count) {
        return std::nullopt;
    }

    VectorModule module;
    for (const auto& ins : program.instructions) {
        if (ins.op == VectorOp::nop) {
            module.nop();
            continue;
        }
        if (ins.op == VectorOp::end_program) {
            continue;
        }
        const auto first = module.source(ins.sources[0]);
        std::uint32_t value = first;
        if (ins.op == VectorOp::not_b32) {
            value = module.unary(Op::bitwise_not, first);
        } else if (ins.source_count == 2) {
            const auto second = module.source(ins.sources[1]);
            switch (ins.op) {
            case VectorOp::add_nc_u32: value = module.binary(Op::iadd, first, second); break;
            case VectorOp::sub_nc_u32: value = module.binary(Op::isub, first, second); break;
            case VectorOp::and_b32:
                value = module.binary(Op::bitwise_and, first, second); break;
            case VectorOp::or_b32:
                value = module.binary(Op::bitwise_or, first, second); break;
            case VectorOp::xor_b32:
                value = module.binary(Op::bitwise_xor, first, second); break;
            case VectorOp::min_u32: {
                const auto cond = module.compare(Op::uless_than, first, second);
                value = module.select(cond, first, second);
                break;
            }
            case VectorOp::max_u32: {
                const auto cond = module.compare(Op::ugreater_than, first, second);
                value = module.select(cond, first, second);
                break;
            }
            case VectorOp::lshlrev_b32:
                value = module.binary(Op::shift_left_logical, second,
                                      module.clamp_shift(first));
                break;
            case VectorOp::lshrrev_b32:
                value = module.binary(Op::shift_right_logical, second,
                                      module.clamp_shift(first));
                break;
            default: return std::nullopt;
            }
        } else if (ins.op != VectorOp::mov_b32) {
            return std::nullopt;
        }
        module.store_reg(ins.destination, value);
    }
    return module.finish();
}

} // namespace nyxora::gpu::shader
