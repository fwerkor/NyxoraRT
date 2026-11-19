#include "nyxora/gpu/shader/spirv.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nyxora::gpu::shader {
namespace {

// Core SPIR-V 1.3 opcodes, defined by the Khronos unified SPIR-V grammar.
// Production builds do not depend on a local Vulkan SDK.
enum class Op : std::uint16_t {
    nop = 0, memory_model = 14, entry_point = 15, execution_mode = 16,
    capability = 17, type_void = 19, type_bool = 20, type_int = 21,
    type_array = 28, type_struct = 30, type_pointer = 32,
    type_function = 33, constant = 43, function = 54, function_end = 56,
    variable = 59, load = 61, store = 62, access_chain = 65,
    decorate = 71, member_decorate = 72,
    iadd = 128, isub = 130, select = 169, iequal = 170,
    inot_equal = 171, ugreater_equal = 174, uless_than = 176,
    bitwise_or = 197, bitwise_xor = 198, bitwise_and = 199,
    bitwise_not = 200, label = 248, ret = 253,
};

void append(std::vector<std::uint32_t>& out, Op opcode,
            std::initializer_list<std::uint32_t> operands) {
    out.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16U) |
                  static_cast<std::uint16_t>(opcode));
    out.insert(out.end(), operands.begin(), operands.end());
}

class Module {
public:
    Module() {
        void_type_ = new_id();
        uint_type_ = new_id();
        bool_type_ = new_id();
        const_length_ = constant(103);
        array_type_ = new_id();
        block_type_ = new_id();
        block_pointer_ = new_id();
        scalar_pointer_ = new_id();
        buffer_ = new_id();
        function_type_ = new_id();
        function_ = new_id();
        entry_ = new_id();
        zero_ = constant(0);
        one_ = constant(1);
    }

    [[nodiscard]] std::uint32_t constant(std::uint32_t value) {
        if (const auto it = constant_ids_.find(value); it != constant_ids_.end()) {
            return it->second;
        }
        const auto id = new_id();
        constant_ids_.emplace(value, id);
        append(constants_, Op::constant, {uint_type_, id, value});
        return id;
    }

    [[nodiscard]] std::uint32_t source(ScalarSource src) {
        if (src.kind == ScalarSourceKind::constant) {
            return constant(src.value);
        }
        return load_index(src.value);
    }

    void store_sgpr(std::uint32_t index, std::uint32_t value) {
        const auto pointer = access(index);
        append(body_, Op::store, {pointer, value});
    }

    void store_scc(std::uint32_t condition) {
        const auto value = new_id();
        append(body_, Op::select, {uint_type_, value, condition, one_, zero_});
        const auto pointer = access(102);
        append(body_, Op::store, {pointer, value});
    }

    [[nodiscard]] std::uint32_t unary(Op opcode, std::uint32_t source_value) {
        const auto result = new_id();
        append(body_, opcode, {uint_type_, result, source_value});
        return result;
    }

    [[nodiscard]] std::uint32_t binary(Op opcode, std::uint32_t lhs, std::uint32_t rhs) {
        const auto result = new_id();
        append(body_, opcode, {uint_type_, result, lhs, rhs});
        return result;
    }

    [[nodiscard]] std::uint32_t compare(Op opcode, std::uint32_t lhs, std::uint32_t rhs) {
        const auto result = new_id();
        append(body_, opcode, {bool_type_, result, lhs, rhs});
        return result;
    }

    void emit_nop() { append(body_, Op::nop, {}); }

    [[nodiscard]] std::vector<std::uint32_t> finalize() {
        std::vector<std::uint32_t> result{
            0x07230203U, 0x00010300U, 0U, next_id_, 0U,
        };
        append(result, Op::capability, {1});             // Shader
        append(result, Op::memory_model, {0, 1});        // Logical, GLSL450
        // Null-terminated "main" padded to the next dword.
        append(result, Op::entry_point, {5, function_, 0x6e69616dU, 0});
        append(result, Op::execution_mode, {function_, 17, 1, 1, 1}); // LocalSize 1,1,1
        append(result, Op::decorate, {array_type_, 6, 4});            // ArrayStride
        append(result, Op::decorate, {block_type_, 2});               // Block
        append(result, Op::member_decorate, {block_type_, 0, 35, 0}); // Offset
        append(result, Op::decorate, {buffer_, 34, 0});               // DescriptorSet
        append(result, Op::decorate, {buffer_, 33, 0});               // Binding

        append(result, Op::type_void, {void_type_});
        append(result, Op::type_int, {uint_type_, 32, 0});
        append(result, Op::type_bool, {bool_type_});
        result.insert(result.end(), constants_.begin(), constants_.end());
        append(result, Op::type_array, {array_type_, uint_type_, const_length_});
        append(result, Op::type_struct, {block_type_, array_type_});
        append(result, Op::type_pointer, {block_pointer_, 12, block_type_}); // StorageBuffer
        append(result, Op::type_pointer, {scalar_pointer_, 12, uint_type_});
        append(result, Op::type_function, {function_type_, void_type_});
        append(result, Op::variable, {block_pointer_, buffer_, 12});
        append(result, Op::function, {void_type_, function_, 0, function_type_});
        append(result, Op::label, {entry_});
        result.insert(result.end(), body_.begin(), body_.end());
        append(result, Op::ret, {});
        append(result, Op::function_end, {});
        return result;
    }

private:
    [[nodiscard]] std::uint32_t new_id() { return next_id_++; }

    [[nodiscard]] std::uint32_t access(std::uint32_t index) {
        const auto ptr = new_id();
        append(body_, Op::access_chain,
               {scalar_pointer_, ptr, buffer_, zero_, constant(index)});
        return ptr;
    }

    [[nodiscard]] std::uint32_t load_index(std::uint32_t index) {
        const auto ptr = access(index);
        const auto value = new_id();
        append(body_, Op::load, {uint_type_, value, ptr});
        return value;
    }

    std::uint32_t next_id_ = 1;
    std::uint32_t void_type_{};
    std::uint32_t uint_type_{};
    std::uint32_t bool_type_{};
    std::uint32_t const_length_{};
    std::uint32_t array_type_{};
    std::uint32_t block_type_{};
    std::uint32_t block_pointer_{};
    std::uint32_t scalar_pointer_{};
    std::uint32_t buffer_{};
    std::uint32_t function_type_{};
    std::uint32_t function_{};
    std::uint32_t entry_{};
    std::uint32_t zero_{};
    std::uint32_t one_{};
    std::unordered_map<std::uint32_t, std::uint32_t> constant_ids_;
    std::vector<std::uint32_t> constants_;
    std::vector<std::uint32_t> body_;
};

} // namespace

std::optional<std::vector<std::uint32_t>>
compile_scalar_diagnostic_spirv(const ScalarControlFlowGraph& cfg) {
    const auto& insns = cfg.program.instructions;
    if (insns.empty() || insns.size() > 4096 ||
        insns.back().opcode != ScalarOp::end_program ||
        cfg.blocks.size() != 1 || !cfg.blocks.front().reachable ||
        cfg.blocks.front().instruction_count != insns.size()) {
        return std::nullopt;
    }
    // Reject every unsupported semantic before generating any SPIR-V, even
    // when the unsupported instruction is unreachable in the original ISA.
    for (const auto& ins : insns) {
        switch (ins.opcode) {
        case ScalarOp::nop:
        case ScalarOp::end_program:
        case ScalarOp::mov_b32:
        case ScalarOp::not_b32:
        case ScalarOp::add_u32:
        case ScalarOp::sub_u32:
        case ScalarOp::and_b32:
        case ScalarOp::or_b32:
        case ScalarOp::xor_b32:
        case ScalarOp::cmp_eq_u32:
        case ScalarOp::movk_i32:
            break;
        default:
            return std::nullopt;
        }
        if (ins.source_count > 2 ||
            (ins.dst_sgpr && *ins.dst_sgpr > 101U)) {
            return std::nullopt;
        }
        for (std::uint8_t i = 0; i < ins.source_count; ++i) {
            if (ins.sources[i].kind == ScalarSourceKind::sgpr &&
                ins.sources[i].value > 101U) {
                return std::nullopt;
            }
        }
    }

    Module module;
    for (const auto& ins : insns) {
        if (ins.opcode == ScalarOp::nop) {
            module.emit_nop();
            continue;
        }
        if (ins.opcode == ScalarOp::end_program) {
            continue;
        }
        if (ins.opcode == ScalarOp::mov_b32 || ins.opcode == ScalarOp::movk_i32) {
            if (!ins.dst_sgpr || ins.source_count != 1) {
                return std::nullopt;
            }
            module.store_sgpr(*ins.dst_sgpr, module.source(ins.sources[0]));
            continue;
        }
        if (ins.opcode == ScalarOp::not_b32) {
            if (!ins.dst_sgpr || ins.source_count != 1) {
                return std::nullopt;
            }
            const auto value = module.unary(Op::bitwise_not, module.source(ins.sources[0]));
            module.store_sgpr(*ins.dst_sgpr, value);
            module.store_scc(module.compare(Op::inot_equal, value, module.constant(0)));
            continue;
        }
        if (ins.source_count != 2) {
            return std::nullopt;
        }
        const auto lhs = module.source(ins.sources[0]);
        const auto rhs = module.source(ins.sources[1]);
        if (ins.opcode == ScalarOp::cmp_eq_u32) {
            module.store_scc(module.compare(Op::iequal, lhs, rhs));
            continue;
        }
        if (!ins.dst_sgpr) {
            return std::nullopt;
        }
        Op opcode = Op::iadd;
        switch (ins.opcode) {
        case ScalarOp::add_u32: opcode = Op::iadd; break;
        case ScalarOp::sub_u32: opcode = Op::isub; break;
        case ScalarOp::and_b32: opcode = Op::bitwise_and; break;
        case ScalarOp::or_b32: opcode = Op::bitwise_or; break;
        case ScalarOp::xor_b32: opcode = Op::bitwise_xor; break;
        default: return std::nullopt;
        }
        const auto result = module.binary(opcode, lhs, rhs);
        module.store_sgpr(*ins.dst_sgpr, result);
        if (ins.opcode == ScalarOp::add_u32) {
            module.store_scc(module.compare(Op::uless_than, result, lhs));
        } else if (ins.opcode == ScalarOp::sub_u32) {
            module.store_scc(module.compare(Op::ugreater_equal, lhs, rhs));
        } else {
            module.store_scc(module.compare(Op::inot_equal, result, module.constant(0)));
        }
    }
    return module.finalize();
}

} // namespace nyxora::gpu::shader
