// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Shader IR -> Mesa NIR for compute programs (phase 3 of the Ampere shader plan): the program goes
// to RADV, which this port links, as the NIR its own built-in shaders use, instead of SPIR-V that
// RADV would parse back into NIR (spirv_to_nir) before compiling. Same result, less compile time.
//
// The NIR mirrors what spirv_to_nir makes of the SPIR-V backend's output: same descriptor
// bindings, same memory accesses, same float controls. A program using anything not translated
// here is left to the SPIR-V backend (Attach returns false), so coverage can grow per opcode.
//
// This file is compiled against Mesa's headers and its Mesa symbols are renamed like the private
// RADV archive's (tools/isolate-radv.py), so it calls the copy of NIR that RADV uses.

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/opcodes.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/nir_handoff.h"

// Mesa's util/macros.h has its own UNREACHABLE.
#undef UNREACHABLE

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_control_flow.h"
#include "vk_shader_module.h"
#include "nir_integer_ops.h"
#include "nir_integer_flag_bindings.h"
#include "nir_compute_ops.h"

namespace {

using namespace Shader;

// Thrown for anything not translated; the program keeps the SPIR-V backend.
struct Unsupported {
    const char* what;
};

constexpr gl_access_qualifier kNoAccess{static_cast<gl_access_qualifier>(0)};

[[noreturn]] void Fail(const char* what) {
    throw Unsupported{what};
}

struct Shape {
    unsigned components;
    unsigned bits;
};

Shape ShapeOf(IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return {1, 1};
    case IR::Type::U8:
        return {1, 8};
    case IR::Type::U16:
    case IR::Type::F16:
        return {1, 16};
    case IR::Type::U32:
    case IR::Type::F32:
        return {1, 32};
    case IR::Type::U64:
    case IR::Type::F64:
        return {1, 64};
    case IR::Type::U32x2:
    case IR::Type::F32x2:
        return {2, 32};
    case IR::Type::U32x3:
    case IR::Type::F32x3:
        return {3, 32};
    case IR::Type::U32x4:
    case IR::Type::F32x4:
        return {4, 32};
    case IR::Type::F16x2:
        return {2, 16};
    case IR::Type::F16x3:
        return {3, 16};
    case IR::Type::F16x4:
        return {4, 16};
    default:
        Fail("value type");
    }
}

const glsl_type* GlslTypeOf(Shape shape) {
    if (shape.bits == 1) {
        if (shape.components != 1) {
            Fail("boolean vector");
        }
        return glsl_bool_type();
    }
    const glsl_type* const scalar{glsl_uintN_t_type(shape.bits)};
    if (shape.components == 1) {
        return scalar;
    }
    return glsl_vector_type(glsl_get_base_type(scalar), shape.components);
}

enum pipe_format PipeFormat(ImageFormat format) {
    switch (format) {
    case ImageFormat::Typeless:
        return PIPE_FORMAT_NONE;
    case ImageFormat::R8_UINT:
        return PIPE_FORMAT_R8_UINT;
    case ImageFormat::R8_SINT:
        return PIPE_FORMAT_R8_SINT;
    case ImageFormat::R16_UINT:
        return PIPE_FORMAT_R16_UINT;
    case ImageFormat::R16_SINT:
        return PIPE_FORMAT_R16_SINT;
    case ImageFormat::R32_UINT:
        return PIPE_FORMAT_R32_UINT;
    case ImageFormat::R32G32_UINT:
        return PIPE_FORMAT_R32G32_UINT;
    case ImageFormat::R32G32B32A32_UINT:
        return PIPE_FORMAT_R32G32B32A32_UINT;
    }
    Fail("image format");
}

struct Dimension {
    glsl_sampler_dim dim;
    bool array;
    unsigned coords; // coordinate components, the layer included
};

Dimension DimensionOf(TextureType type) {
    switch (type) {
    case TextureType::Color1D:
        return {GLSL_SAMPLER_DIM_1D, false, 1};
    case TextureType::ColorArray1D:
        return {GLSL_SAMPLER_DIM_1D, true, 2};
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        return {GLSL_SAMPLER_DIM_2D, false, 2};
    case TextureType::ColorArray2D:
        return {GLSL_SAMPLER_DIM_2D, true, 3};
    case TextureType::Color3D:
        return {GLSL_SAMPLER_DIM_3D, false, 3};
    case TextureType::Buffer:
        return {GLSL_SAMPLER_DIM_BUF, false, 1};
    default:
        Fail("texture type");
    }
}

class Translator {
public:
    Translator(const Profile& profile_, IR::Program& program_)
        : profile{profile_}, program{program_}, info{program_.info} {}

    nir_shader* Run() {
        Validate();
        static const nir_shader_compiler_options options{};
        b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "eden_nx2_compute");
        nir_shader* const shader{b.shader};
        try {
            SetupShader();
            DefineResources();
            CollectPhis();
            Traverse();
            // spirv_to_nir's own lowering, which RADV skips for a module carrying NIR.
            nir_lower_continue_constructs(shader);
            nir_lower_returns(shader);
            nir_lower_vars_to_ssa(shader);
            nir_opt_dce(shader);
        } catch (...) {
            ralloc_free(shader);
            throw;
        }
        return shader;
    }

private:
    struct Construct {
        enum class Kind { If, Loop } kind;
        nir_if* nif;
        nir_loop* loop;
        IR::Block* merge;
        IR::Block* continue_block;
        bool dead_at_open;
    };

    void Validate() const {
        if (program.stage != Stage::Compute) {
            Fail("stage");
        }
        if (!profile.SupportsSubgroupStage(Stage::Compute)) {
            Fail("subgroups");
        }
        if (profile.has_broken_spirv_subgroup_shuffle || profile.has_broken_signed_operations) {
            Fail("driver workaround");
        }
        if (program.local_memory_size != 0 || info.uses_global_memory || info.uses_cbuf_indirect) {
            Fail("memory kind");
        }
        if (!info.texture_buffer_descriptors.empty()) {
            for (const auto& desc : info.texture_buffer_descriptors) {
                if (desc.count != 1) {
                    Fail("texture buffer array");
                }
            }
        }
    }

    unsigned FpControl(unsigned bits) const {
        return bits == 16 ? fp16_control : bits == 32 ? fp32_control : bits == 64 ? fp64_control : 0;
    }

    void SetupShader() {
        nir_shader* const shader{b.shader};
        shader->info.workgroup_size[0] = static_cast<uint16_t>(program.workgroup_size[0]);
        shader->info.workgroup_size[1] = static_cast<uint16_t>(program.workgroup_size[1]);
        shader->info.workgroup_size[2] = static_cast<uint16_t>(program.workgroup_size[2]);
        shader->info.workgroup_size_variable = false;
        shader->info.shared_size = (program.shared_memory_size + 3U) & ~3U;

        // Float controls as the SPIR-V backend declares them (SetupDenormControl,
        // SetupSignedNanCapabilities) and spirv_to_nir applies them.
        if (!profile.support_float_controls) {
            return;
        }
        unsigned mode{};
        if (info.uses_fp32_denorms_flush && info.uses_fp32_denorms_preserve) {
        } else if (info.uses_fp32_denorms_flush) {
            if (profile.support_fp32_denorm_flush && !profile.has_broken_fp32_denorm_flush) {
                mode |= FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32;
            }
        } else if (info.uses_fp32_denorms_preserve) {
            if (profile.support_fp32_denorm_preserve) {
                mode |= FLOAT_CONTROLS_DENORM_PRESERVE_FP32;
            }
        }
        if (profile.support_separate_denorm_behavior && !profile.has_broken_fp16_float_controls) {
            if (info.uses_fp16_denorms_flush && info.uses_fp16_denorms_preserve) {
            } else if (info.uses_fp16_denorms_flush) {
                if (profile.support_fp16_denorm_flush) {
                    mode |= FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP16;
                }
            } else if (info.uses_fp16_denorms_preserve) {
                if (profile.support_fp16_denorm_preserve) {
                    mode |= FLOAT_CONTROLS_DENORM_PRESERVE_FP16;
                }
            }
        }
        shader->info.float_controls_execution_mode = mode;
        if (!(profile.has_broken_fp16_float_controls && info.uses_fp16)) {
            if (info.uses_fp16 && profile.support_fp16_signed_zero_nan_preserve) {
                fp16_control = nir_fp_preserve_sz_inf_nan;
            }
            if (profile.support_fp32_signed_zero_nan_preserve) {
                fp32_control = nir_fp_preserve_sz_inf_nan;
            }
            if (info.uses_fp64 && profile.support_fp64_signed_zero_nan_preserve) {
                fp64_control = nir_fp_preserve_sz_inf_nan;
            }
        }
    }

    // Bindings as EmitContext numbers them (DefineConstantBuffers ... DefineImages).
    void DefineResources() {
        if (!profile.unified_descriptor_binding) {
            Fail("descriptor binding model");
        }
        u32 binding{};
        for (const ConstantBufferDescriptor& desc : info.constant_buffer_descriptors) {
            for (u32 i = 0; i < desc.count; ++i) {
                cbuf_bindings[desc.index + i] = binding;
            }
            binding += desc.count;
        }
        u32 ssbo_index{};
        for (const StorageBufferDescriptor& desc : info.storage_buffers_descriptors) {
            for (u32 i = 0; i < desc.count; ++i) {
                ssbo_bindings[ssbo_index + i] = binding;
            }
            ssbo_index += desc.count;
            binding += desc.count;
        }
        nir_shader* const shader{b.shader};
        for (const TextureBufferDescriptor& desc : info.texture_buffer_descriptors) {
            (void)desc;
            nir_variable* const var{nir_variable_create(
                shader, nir_var_uniform,
                glsl_texture_type(GLSL_SAMPLER_DIM_BUF, false, GLSL_TYPE_FLOAT), "texbuf")};
            var->data.descriptor_set = 0;
            var->data.binding = binding++;
            var->data.resource_type = nir_resource_type_sampled_image;
            texture_buffer_vars.push_back(var);
        }
        // Image buffers are not translated; keep their bindings counted.
        binding += static_cast<u32>(info.image_buffer_descriptors.size());
        for (const TextureDescriptor& desc : info.texture_descriptors) {
            nir_variable* var{};
            if (desc.count == 1 && !desc.is_multisample && desc.type != TextureType::ColorCube &&
                desc.type != TextureType::ColorArrayCube && desc.type != TextureType::Buffer) {
                const Dimension dim{DimensionOf(desc.type)};
                var = nir_variable_create(
                    shader, nir_var_uniform,
                    glsl_sampler_type(dim.dim, false, dim.array,
                                      desc.is_integer ? GLSL_TYPE_UINT : GLSL_TYPE_FLOAT),
                    "tex");
                var->data.descriptor_set = 0;
                var->data.binding = binding;
                var->data.resource_type = nir_resource_type_combined_sampled_image;
            }
            // nullptr: declared by the SPIR-V backend, but not translated here.
            texture_vars.push_back(var);
            ++binding;
        }
        for (const ImageDescriptor& desc : info.image_descriptors) {
            nir_variable* var{};
            if (desc.count == 1 && desc.type != TextureType::Buffer &&
                desc.type != TextureType::ColorCube && desc.type != TextureType::ColorArrayCube) {
                const Dimension dim{DimensionOf(desc.type)};
                var = nir_variable_create(
                    shader, nir_var_image,
                    glsl_image_type(dim.dim, dim.array,
                                    desc.is_integer ? GLSL_TYPE_UINT : GLSL_TYPE_FLOAT),
                    "img");
                var->data.descriptor_set = 0;
                var->data.binding = binding;
                var->data.image.format = PipeFormat(desc.format);
                var->data.resource_type = nir_resource_type_read_write_image;
            }
            image_vars.push_back(var);
            ++binding;
        }
    }

    // Phis become function variables: each predecessor stores its incoming value at its end, the
    // phi loads it at the top of its block (lowered back to SSA by nir_lower_vars_to_ssa).
    void CollectPhis() {
        for (IR::Block* const block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::Phi) {
                    break;
                }
                const Shape shape{ShapeOf(inst.Flags<IR::Type>())};
                nir_variable* const var{
                    nir_local_variable_create(b.impl, GlslTypeOf(shape), "phi")};
                phi_vars[&inst] = var;
                for (size_t i = 0; i < inst.NumArgs(); ++i) {
                    phi_stores[inst.PhiBlock(i)].push_back({var, inst.Arg(i)});
                }
            }
        }
    }

    void StorePhiOperands(IR::Block* block) {
        const auto it{phi_stores.find(block)};
        if (it == phi_stores.end()) {
            return;
        }
        for (const auto& [var, value] : it->second) {
            if (value.IsEmpty()) {
                continue;
            }
            nir_store_var(&b, var, Def(value), 0x1);
        }
    }

    void Traverse() {
        const auto& list{program.syntax_list};
        bool dead{false};
        std::vector<Construct> constructs;
        const auto jump{[&](nir_jump_type type) {
            nir_jump(&b, type);
            dead = true;
        }};
        for (size_t index = 0; index < list.size(); ++index) {
            const IR::AbstractSyntaxNode& node{list[index]};
            switch (node.type) {
            case IR::AbstractSyntaxNode::Type::Block: {
                const bool opens_loop{index + 1 < list.size() &&
                                      list[index + 1].type == IR::AbstractSyntaxNode::Type::Loop};
                if (opens_loop) {
                    // The loop header (phis of loop-carried values) belongs inside the loop.
                    // spirv_to_nir's loop shape: the continue block is the loop's continue
                    // construct (lowered below by nir_lower_continue_constructs).
                    const IR::AbstractSyntaxNode& loop{list[index + 1]};
                    nir_loop* nloop{};
                    if (!dead) {
                        nloop = nir_push_loop(&b);
                        nir_loop_add_continue_construct(nloop);
                    }
                    constructs.push_back({Construct::Kind::Loop, nullptr, nloop,
                                          loop.data.loop.merge, loop.data.loop.continue_block,
                                          dead});
                } else if (!dead && !constructs.empty() &&
                           constructs.back().kind == Construct::Kind::Loop &&
                           constructs.back().continue_block == node.data.block) {
                    nir_push_continue(&b, constructs.back().loop);
                }
                if (!dead) {
                    EmitBlock(node.data.block);
                }
                if (opens_loop) {
                    ++index; // the Loop node itself
                }
                break;
            }
            case IR::AbstractSyntaxNode::Type::If: {
                nir_if* nif{};
                if (!dead) {
                    nif = nir_push_if(&b, Def(node.data.if_node.cond));
                }
                constructs.push_back(
                    {Construct::Kind::If, nif, nullptr, node.data.if_node.merge, nullptr, dead});
                break;
            }
            case IR::AbstractSyntaxNode::Type::EndIf: {
                if (constructs.empty() || constructs.back().kind != Construct::Kind::If ||
                    constructs.back().merge != node.data.end_if.merge) {
                    Fail("unbalanced if");
                }
                const Construct construct{constructs.back()};
                constructs.pop_back();
                if (!construct.dead_at_open) {
                    nir_pop_if(&b, construct.nif);
                }
                dead = construct.dead_at_open;
                break;
            }
            case IR::AbstractSyntaxNode::Type::Loop:
                Fail("loop without header");
            case IR::AbstractSyntaxNode::Type::Break: {
                if (dead) {
                    break;
                }
                const auto loop{std::find_if(constructs.rbegin(), constructs.rend(),
                                             [](const Construct& c) {
                                                 return c.kind == Construct::Kind::Loop;
                                             })};
                if (loop == constructs.rend() || loop->merge != node.data.break_node.merge) {
                    Fail("break target");
                }
                nir_break_if(&b, Def(node.data.break_node.cond));
                break;
            }
            case IR::AbstractSyntaxNode::Type::Repeat: {
                if (constructs.empty() || constructs.back().kind != Construct::Kind::Loop ||
                    constructs.back().merge != node.data.repeat.merge) {
                    Fail("unbalanced loop");
                }
                const Construct construct{constructs.back()};
                constructs.pop_back();
                if (construct.dead_at_open) {
                    dead = true;
                    break;
                }
                if (!dead) {
                    nir_def* cond{Def(node.data.repeat.cond)};
                    if (!Settings::values.disable_shader_loop_safety_checks) {
                        cond = nir_iand(&b, cond, SafetyCounter());
                    }
                    nir_break_if(&b, nir_inot(&b, cond));
                }
                nir_pop_loop(&b, construct.loop);
                dead = false;
                break;
            }
            case IR::AbstractSyntaxNode::Type::Return:
            case IR::AbstractSyntaxNode::Type::Unreachable:
                if (!dead) {
                    jump(nir_jump_return);
                }
                break;
            }
        }
        if (!constructs.empty()) {
            Fail("unclosed construct");
        }
    }

    // EmitContext's per-loop counter: a loop gives up after 0x2000 iterations.
    nir_def* SafetyCounter() {
        nir_variable* const counter{
            nir_local_variable_create(b.impl, glsl_uint_type(), "loop_safety")};
        const nir_cursor cursor{b.cursor};
        b.cursor = nir_before_impl(b.impl);
        nir_store_var(&b, counter, nir_imm_int(&b, 0x2000), 0x1);
        b.cursor = cursor;
        nir_def* const decremented{nir_iadd_imm(&b, nir_load_var(&b, counter), -1)};
        nir_store_var(&b, counter, decremented, 0x1);
        return nir_ige_imm(&b, decremented, 0);
    }

    void EmitBlock(IR::Block* block) {
        for (IR::Inst& inst : block->Instructions()) {
            EmitInst(inst);
        }
        StorePhiOperands(block);
    }

    nir_def* Imm(const IR::Value& value) {
        switch (value.Type()) {
        case IR::Type::U1:
            return nir_imm_bool(&b, value.U1());
        case IR::Type::U8:
            return nir_imm_intN_t(&b, value.U8(), 8);
        case IR::Type::U16:
            return nir_imm_intN_t(&b, value.U16(), 16);
        case IR::Type::U32:
            return nir_imm_int(&b, static_cast<int>(value.U32()));
        case IR::Type::F32:
            return nir_imm_int(&b, static_cast<int>(std::bit_cast<u32>(value.F32())));
        case IR::Type::U64:
            return nir_imm_int64(&b, static_cast<int64_t>(value.U64()));
        case IR::Type::F64:
            return nir_imm_int64(&b, static_cast<int64_t>(std::bit_cast<u64>(value.F64())));
        default:
            Fail("immediate type");
        }
    }

    nir_def* Def(const IR::Value& value) {
        if (value.IsImmediate()) {
            return Imm(value);
        }
        IR::Inst* const inst{value.InstRecursive()};
        const auto it{defs.find(inst)};
        if (it == defs.end()) {
            Fail("value not defined");
        }
        return it->second;
    }

    void Set(IR::Inst& inst, nir_def* def) {
        defs[&inst] = def;
    }

    // Float ALU control as spirv_to_nir sets it: the type's float controls, NoContraction as exact.
    void Fp(unsigned bits, unsigned extra = 0) {
        b.fp_math_ctrl = FpControl(bits) | extra;
    }
    void FpExact(const IR::Inst& inst, unsigned bits) {
        Fp(bits, inst.Flags<IR::FpControl>().no_contraction ? nir_fp_exact : 0U);
    }
    void IntOp() {
        b.fp_math_ctrl = nir_fp_fast_math;
    }

    nir_def* LaneId() {
        return nir_load_subgroup_invocation(&b);
    }

    nir_def* BufferDescriptor(u32 binding, nir_descriptor_type type, nir_resource_type resource) {
        nir_def* const index{_nir_build_vulkan_resource_index(
            &b, 2, 32, nir_imm_int(&b, 0),
            _nir_vulkan_resource_index_indices{0, 0, binding, type, resource})};
        return _nir_build_load_vulkan_descriptor(&b, 2, 32, index,
                                                 _nir_load_vulkan_descriptor_indices{0, type});
    }

    nir_def* CbufDescriptor(const IR::Value& index) {
        if (!index.IsImmediate()) {
            Fail("indirect constant buffer");
        }
        const auto it{cbuf_bindings.find(index.U32())};
        if (it == cbuf_bindings.end()) {
            Fail("constant buffer binding");
        }
        return BufferDescriptor(it->second, nir_descriptor_type_uniform_buffer,
                                nir_resource_type_uniform_buffer);
    }

    nir_def* SsboDescriptor(const IR::Value& index) {
        if (!index.IsImmediate()) {
            Fail("indirect storage buffer");
        }
        const auto it{ssbo_bindings.find(index.U32())};
        if (it == ssbo_bindings.end()) {
            Fail("storage buffer binding");
        }
        return BufferDescriptor(it->second, nir_descriptor_type_storage_buffer,
                                nir_resource_type_read_write_storage_buffer);
    }

    // Word-aligned byte offset (the SPIR-V backend indexes u32 arrays: offset / 4).
    nir_def* WordOffset(const IR::Value& offset) {
        if (offset.IsImmediate()) {
            return nir_imm_int(&b, static_cast<int>(offset.U32() & ~3U));
        }
        return nir_iand_imm(&b, Def(offset), ~3U);
    }

    // Shared memory is indexed with an arithmetic shift: (offset >> n) << n == offset & ~(2^n - 1).
    nir_def* SharedOffset(const IR::Value& offset, u32 size) {
        if (offset.IsImmediate()) {
            return nir_imm_int(&b, static_cast<int>(offset.U32() & ~(size - 1)));
        }
        return nir_iand_imm(&b, Def(offset), ~static_cast<u64>(size - 1));
    }
    nir_def* SharedWordOffset(const IR::Value& offset) {
        return SharedOffset(offset, 4);
    }

    // EmitContext::DefineSharedMemory: with VK_KHR_workgroup_memory_explicit_layout the SPIR-V
    // backend accesses shared memory through typed views (16-bit loads, 8-byte u32x2 accesses).
    bool ExplicitWorkgroupLayout() const {
        return profile.support_explicit_workgroup_layout &&
               (!info.uses_int8 || profile.support_workgroup_layout_8bit_access) &&
               (!info.uses_int16 || profile.support_workgroup_layout_16bit_access);
    }

    nir_def* LoadShared(nir_def* offset, unsigned components, unsigned bits, unsigned align) {
        return _nir_build_load_shared(&b, components, bits, offset,
                                      _nir_load_shared_indices{0, 0, kNoAccess, align, 0});
    }

    void StoreShared(nir_def* offset, nir_def* value, unsigned align = 4) {
        _nir_build_store_shared(
            &b, value, offset,
            _nir_store_shared_indices{0, 0, kNoAccess,
                                      static_cast<unsigned>((1U << value->num_components) - 1),
                                      align, 0});
    }

    nir_def* Shuffle(IR::Inst& inst, nir_def* value) {
        // emit_spirv_warp.cpp for a warp of the guest's size (the pipeline requires subgroups of 32).
        nir_def* const index{Def(inst.Arg(1))};
        nir_def* const clamp{Def(inst.Arg(2))};
        nir_def* const segmentation_mask{Def(inst.Arg(3))};
        nir_def* const not_seg_mask{nir_inot(&b, segmentation_mask)};
        nir_def* const thread_id{LaneId()};
        nir_def* const min_thread_id{nir_iand(&b, thread_id, segmentation_mask)};
        nir_def* const max_thread_id{nir_ior(&b, min_thread_id, nir_iand(&b, clamp, not_seg_mask))};
        const IR::Opcode opcode{inst.GetOpcode()};
        nir_def* in_range{};
        nir_def* shuffled{};
        if (opcode == IR::Opcode::ShuffleIndex) {
            nir_def* const lhs{nir_iand(&b, nir_iand_imm(&b, index, 31), not_seg_mask)};
            nir_def* const src_thread_id{nir_ior(&b, lhs, min_thread_id)};
            in_range = nir_ige(&b, max_thread_id, src_thread_id);
            const IR::Value lane{inst.Arg(1).Resolve()};
            const IR::Value segment{inst.Arg(3).Resolve()};
            if (lane.IsImmediate() && segment.IsImmediate() && segment.U32() == 0) {
                shuffled = nir_read_invocation(&b, value, nir_imm_int(&b, lane.U32() & 31));
            } else {
                shuffled = nir_shuffle(&b, value, src_thread_id);
            }
        } else {
            nir_def* const delta{nir_iand_imm(&b, index, 31)};
            nir_def* src_thread_id{};
            switch (opcode) {
            case IR::Opcode::ShuffleUp:
                src_thread_id = nir_isub(&b, thread_id, delta);
                in_range = nir_ige(&b, src_thread_id, max_thread_id);
                shuffled = profile.support_shuffle_relative ? nir_shuffle_up(&b, value, delta)
                                                            : nir_shuffle(&b, value, src_thread_id);
                break;
            case IR::Opcode::ShuffleDown:
                src_thread_id = nir_iadd(&b, thread_id, delta);
                in_range = nir_ige(&b, max_thread_id, src_thread_id);
                shuffled = profile.support_shuffle_relative ? nir_shuffle_down(&b, value, delta)
                                                            : nir_shuffle(&b, value, src_thread_id);
                break;
            case IR::Opcode::ShuffleButterfly:
                src_thread_id = nir_ixor(&b, thread_id, delta);
                in_range = nir_ige(&b, max_thread_id, src_thread_id);
                shuffled = nir_shuffle_xor(&b, value, delta);
                break;
            default:
                Fail("shuffle");
            }
        }
        if (IR::Inst* const in_bounds{inst.GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)}) {
            defs[in_bounds] = in_range;
        }
        return nir_bcsel(&b, in_range, shuffled, value);
    }

    nir_def* ImageFetch(IR::Inst& inst) {
        const auto tex_info{inst.Flags<IR::TextureInstInfo>()};
        if (!inst.Arg(4).IsEmpty()) {
            Fail("multisample fetch");
        }
        const TextureType type{tex_info.type.Value()};
        const u32 descriptor{tex_info.descriptor_index.Value()};
        nir_variable* var{};
        bool is_integer{};
        if (type == TextureType::Buffer) {
            if (descriptor >= texture_buffer_vars.size()) {
                Fail("texture buffer");
            }
            var = texture_buffer_vars[descriptor];
        } else {
            if (descriptor >= texture_vars.size() || texture_vars[descriptor] == nullptr) {
                Fail("texture");
            }
            var = texture_vars[descriptor];
            is_integer = info.texture_descriptors[descriptor].is_integer;
        }
        const Dimension dim{DimensionOf(type)};
        nir_def* coords{Def(inst.Arg(1))};
        if (coords->num_components != dim.coords) {
            Fail("fetch coordinates");
        }
        if (!inst.Arg(2).IsEmpty()) {
            nir_def* offset{Def(inst.Arg(2))};
            switch (type) {
            case TextureType::ColorArray1D:
                offset = nir_vec2(&b, nir_channel(&b, offset, 0), nir_imm_int(&b, 0));
                break;
            case TextureType::ColorArray2D:
                offset = nir_vec3(&b, nir_channel(&b, offset, 0), nir_channel(&b, offset, 1),
                                  nir_imm_int(&b, 0));
                break;
            default:
                break;
            }
            if (offset->num_components != coords->num_components) {
                Fail("fetch offset");
            }
            coords = nir_iadd(&b, coords, offset);
        }
        nir_deref_instr* const deref{nir_build_deref_var(&b, var)};
        const bool has_lod{type != TextureType::Buffer};
        nir_tex_instr* const tex{nir_tex_instr_create(b.shader, has_lod ? 3 : 2)};
        tex->op = nir_texop_txf;
        tex->sampler_dim = dim.dim;
        tex->is_array = dim.array;
        tex->is_shadow = false;
        tex->coord_components = dim.coords;
        tex->dest_type = is_integer ? nir_type_uint32 : nir_type_float32;
        tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
        tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_coord, coords);
        if (has_lod) {
            nir_def* const lod{inst.Arg(3).IsEmpty() ? nir_imm_int(&b, 0) : Def(inst.Arg(3))};
            tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
        }
        nir_def_init(&tex->instr, &tex->def, 4, 32);
        nir_builder_instr_insert(&b, &tex->instr);
        return &tex->def;
    }

    nir_def* ImageSampleExplicitLod(IR::Inst& inst) {
        const auto tex_info{inst.Flags<IR::TextureInstInfo>()};
        const u32 descriptor{tex_info.descriptor_index.Value()};
        if (inst.HasAssociatedPseudoOperation() || tex_info.is_depth != 0 ||
            descriptor >= texture_vars.size() || texture_vars[descriptor] == nullptr) {
            Fail("unsupported explicit LOD sample");
        }
        const TextureDescriptor& desc{info.texture_descriptors[descriptor]};
        const TextureType type{tex_info.type.Value()};
        if (desc.type != type) Fail("sample texture type");
        const Dimension dim{DimensionOf(type)};
        nir_def* const coords{Def(inst.Arg(1))};
        nir_def* const lod{Def(inst.Arg(2))};
        if (coords->num_components != dim.coords || lod->num_components != 1) {
            Fail("sample coordinates or LOD");
        }
        nir_def* offset{};
        if (!inst.Arg(3).IsEmpty()) {
            const IR::Value value{inst.Arg(3).Resolve()};
            if (!value.IsImmediate() && !value.InstRecursive()->AreAllArgsImmediates()) {
                Fail("dynamic sample offset");
            }
            offset = Def(inst.Arg(3));
            if (offset->num_components != dim.coords - unsigned(dim.array)) {
                Fail("sample offset shape");
            }
        }
        return &Eden::Nir::EmitSampleExplicitLod(&b, texture_vars[descriptor], dim.dim,
                                                dim.array, desc.is_integer, coords, lod,
                                                offset)->def;
    }

    void ImageWrite(IR::Inst& inst) {
        const auto tex_info{inst.Flags<IR::TextureInstInfo>()};
        const TextureType type{tex_info.type.Value()};
        const u32 descriptor{tex_info.descriptor_index.Value()};
        if (type == TextureType::Buffer || descriptor >= image_vars.size() ||
            image_vars[descriptor] == nullptr) {
            Fail("image");
        }
        nir_variable* const var{image_vars[descriptor]};
        const ImageDescriptor& desc{info.image_descriptors[descriptor]};
        const Dimension dim{DimensionOf(desc.type)};
        nir_def* const coords{Def(inst.Arg(1))};
        if (coords->num_components != dim.coords) {
            Fail("image coordinates");
        }
        nir_deref_instr* const deref{nir_build_deref_var(&b, var)};
        _nir_build_image_deref_store(
            &b, &deref->def, nir_pad_vector(&b, coords, 4), nir_undef(&b, 1, 32),
            Def(inst.Arg(2)), nir_imm_int(&b, 0),
            _nir_image_deref_store_indices{0, dim.dim, dim.array, var->data.image.format,
                                           kNoAccess,
                                           desc.is_integer ? nir_type_uint32 : nir_type_float32});
    }

    nir_def* Undef(unsigned bits) {
        static const bool zero{[] {
            const char* const value{std::getenv("EDEN_NX2_UNDEF_ZERO")};
            return value == nullptr || value[0] != '0';
        }()};
        if (!zero) {
            return nir_undef(&b, 1, bits);
        }
        return bits == 1 ? nir_imm_false(&b) : nir_imm_intN_t(&b, 0, bits);
    }

    void IntegerResultFlags(IR::Inst& inst, nir_def* result, nir_def* lhs = nullptr,
                            nir_def* rhs = nullptr) {
        using Eden::Nir::IntegerFlag;
        Eden::Nir::VisitIntegerFlags(inst, [&](IR::Inst& pseudo, IntegerFlag flag) {
            if (lhs == nullptr && (flag == IntegerFlag::Carry || flag == IntegerFlag::Overflow)) {
                Fail("unsupported integer flag");
            }
            Set(pseudo, Eden::Nir::EmitIntegerFlag(&b, flag, result, lhs, rhs));
        });
    }

    // Keep expanded coverage opt-in until the console A/B qualifies the new programs.
    void RequireExtendedCompute() const {
        static const bool enabled{[] {
            const char* const value{std::getenv("EDEN_NX2_NIR_EXTENDED_COMPUTE")};
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }()};
        if (!enabled) {
            Fail("extended compute coverage disabled");
        }
    }

    void EmitInst(IR::Inst& inst) {
        const auto arg{[&](size_t i) { return Def(inst.Arg(i)); }};
        IntOp();
        using IR::Opcode;
        switch (inst.GetOpcode()) {
        case Opcode::Phi:
            Set(inst, nir_load_var(&b, phi_vars.at(&inst)));
            return;
        case Opcode::Identity:
        case Opcode::Void:
        case Opcode::Reference:
        case Opcode::Prologue:
        case Opcode::Epilogue:
            return;
        case Opcode::GetZeroFromOp:
        case Opcode::GetSignFromOp:
        case Opcode::GetCarryFromOp:
        case Opcode::GetOverflowFromOp:
        case Opcode::GetInBoundsFromOp:
            if (!defs.contains(&inst)) {
                Fail("unresolved pseudo-operation");
            }
            return;
        case Opcode::ConditionRef:
            Set(inst, arg(0));
            return;
        case Opcode::Barrier:
            _nir_build_barrier(&b, _nir_barrier_indices{0, SCOPE_WORKGROUP, SCOPE_WORKGROUP,
                                                        NIR_MEMORY_ACQ_REL, nir_var_mem_shared});
            return;
        case Opcode::WorkgroupId:
            Set(inst, nir_load_workgroup_id(&b));
            return;
        case Opcode::LocalInvocationId:
            Set(inst, nir_load_local_invocation_id(&b));
            return;
        case Opcode::LaneId:
            Set(inst, LaneId());
            return;
        case Opcode::UndefU1:
            Set(inst, Undef(1));
            return;
        case Opcode::UndefU32:
            Set(inst, Undef(32));
            return;
        case Opcode::GetCbufF32:
            RequireExtendedCompute();
            [[fallthrough]];
        case Opcode::GetCbufU32: {
            nir_def* const offset{WordOffset(inst.Arg(1))};
            nir_def* value{Eden::Nir::EmitCbufWord(
                &b, CbufDescriptor(inst.Arg(0)), offset,
                inst.Arg(1).IsImmediate() ? (inst.Arg(1).U32() & ~3U) : 0U,
                inst.Arg(1).IsImmediate() ? 4U : 65536U)};
            if (!inst.Arg(1).IsImmediate() && profile.has_broken_robust) {
                value = nir_bcsel(&b, nir_ult_imm(&b, nir_ushr_imm(&b, offset, 2), 0x10000), value,
                                  nir_imm_int(&b, 0));
            }
            Set(inst, value);
            return;
        }
        case Opcode::GetCbufU32x2: {
            RequireExtendedCompute();
            // The SPIR-V alias is an array of uvec2: discard all three low address bits.
            if (!profile.support_descriptor_aliasing) {
                Fail("vector constant buffer without descriptor aliasing");
            }
            nir_def* const offset{nir_iand_imm(&b, Def(inst.Arg(1)), ~7U)};
            nir_def* value{_nir_build_load_ubo(
                &b, 2, 32, CbufDescriptor(inst.Arg(0)), offset,
                _nir_load_ubo_indices{0, ACCESS_NON_WRITEABLE, 8, 0,
                                      inst.Arg(1).IsImmediate() ? (inst.Arg(1).U32() & ~7U) : 0U,
                                      inst.Arg(1).IsImmediate() ? 8U : 65536U})};
            if (!inst.Arg(1).IsImmediate() && profile.has_broken_robust) {
                value = nir_bcsel(&b, nir_ult_imm(&b, nir_ushr_imm(&b, offset, 3), 0x10000), value,
                                  nir_imm_ivec2(&b, 0, 0));
            }
            Set(inst, value);
            return;
        }
        case Opcode::LoadStorage64:
            RequireExtendedCompute();
            if (!profile.support_descriptor_aliasing) {
                Fail("vector storage buffer without descriptor aliasing");
            }
            // LoadStorage64 returns two u32 lanes, not a scalar u64.
            Set(inst, _nir_build_load_ssbo(&b, 2, 32, SsboDescriptor(inst.Arg(0)),
                                           nir_iand_imm(&b, Def(inst.Arg(1)), ~7U),
                                           _nir_load_ssbo_indices{0, kNoAccess, 8, 0, 0}));
            return;
        case Opcode::LoadStorage32:
            Set(inst, _nir_build_load_ssbo(&b, 1, 32, SsboDescriptor(inst.Arg(0)),
                                           WordOffset(inst.Arg(1)),
                                           _nir_load_ssbo_indices{0, kNoAccess, 4, 0, 0}));
            return;
        case Opcode::WriteStorage128:
            RequireExtendedCompute();
            if (!profile.support_descriptor_aliasing) {
                Fail("vector storage buffer without descriptor aliasing");
            }
            Eden::Nir::EmitWriteStorage128(&b, SsboDescriptor(inst.Arg(0)), arg(1), arg(2));
            return;
        case Opcode::WriteStorage32:
            _nir_build_store_ssbo(&b, arg(2), SsboDescriptor(inst.Arg(0)), WordOffset(inst.Arg(1)),
                                  _nir_store_ssbo_indices{0, 0x1, kNoAccess, 4, 0, 0});
            return;
        case Opcode::LoadSharedU16: {
            if (ExplicitWorkgroupLayout() && info.uses_int16 && profile.support_int16) {
                Set(inst, nir_u2u32(&b, LoadShared(SharedOffset(inst.Arg(0), 2), 1, 16, 2)));
                return;
            }
            // The containing word's half, as BitFieldUExtract(word, (offset * 8) & 16, 16).
            nir_def* const offset{Def(inst.Arg(0))};
            nir_def* const word{LoadShared(SharedWordOffset(inst.Arg(0)), 1, 32, 4)};
            nir_def* const bit{nir_iand_imm(&b, nir_ishl_imm(&b, offset, 3), 16)};
            Set(inst, nir_ubitfield_extract(&b, word, bit, nir_imm_int(&b, 16)));
            return;
        }
        case Opcode::LoadSharedU32:
            Set(inst, LoadShared(SharedWordOffset(inst.Arg(0)), 1, 32, 4));
            return;
        case Opcode::LoadSharedU64:
            if (ExplicitWorkgroupLayout()) {
                Set(inst, LoadShared(SharedOffset(inst.Arg(0), 8), 2, 32, 8));
            } else {
                Set(inst, LoadShared(SharedWordOffset(inst.Arg(0)), 2, 32, 4));
            }
            return;
        case Opcode::WriteSharedU32:
            StoreShared(SharedWordOffset(inst.Arg(0)), arg(1));
            return;
        case Opcode::WriteSharedU64:
            if (ExplicitWorkgroupLayout()) {
                StoreShared(SharedOffset(inst.Arg(0), 8), arg(1), 8);
            } else {
                StoreShared(SharedWordOffset(inst.Arg(0)), arg(1));
            }
            return;
        case Opcode::CompositeConstructU32x2:
        case Opcode::CompositeConstructF16x2:
        case Opcode::CompositeConstructF32x2:
            Set(inst, nir_vec2(&b, arg(0), arg(1)));
            return;
        case Opcode::CompositeConstructU32x3:
        case Opcode::CompositeConstructF16x3:
        case Opcode::CompositeConstructF32x3:
            Set(inst, nir_vec3(&b, arg(0), arg(1), arg(2)));
            return;
        case Opcode::CompositeConstructU32x4:
        case Opcode::CompositeConstructF16x4:
        case Opcode::CompositeConstructF32x4:
            Set(inst, nir_vec4(&b, arg(0), arg(1), arg(2), arg(3)));
            return;
        case Opcode::CompositeExtractU32x2:
        case Opcode::CompositeExtractU32x3:
        case Opcode::CompositeExtractU32x4:
        case Opcode::CompositeExtractF16x2:
        case Opcode::CompositeExtractF16x3:
        case Opcode::CompositeExtractF16x4:
        case Opcode::CompositeExtractF32x2:
        case Opcode::CompositeExtractF32x3:
        case Opcode::CompositeExtractF32x4: {
            const IR::Value element{inst.Arg(1)};
            if (!element.IsImmediate()) {
                Fail("dynamic composite index");
            }
            nir_def* const composite{arg(0)};
            if (element.U32() >= composite->num_components) {
                Fail("composite index");
            }
            Set(inst, nir_channel(&b, composite, element.U32()));
            return;
        }
        case Opcode::SelectU1:
        case Opcode::SelectU32:
        case Opcode::SelectF32:
            Set(inst, nir_bcsel(&b, arg(0), arg(1), arg(2)));
            return;
        case Opcode::BitCastU16F16:
        case Opcode::BitCastF16U16:
        case Opcode::BitCastU32F32:
        case Opcode::BitCastF32U32:
        case Opcode::BitCastU64F64:
        case Opcode::BitCastF64U64:
            Set(inst, arg(0));
            return;
        case Opcode::PackUint2x32:
            RequireExtendedCompute();
            Set(inst, Eden::Nir::EmitPackUint2x32(&b, arg(0)));
            return;
        case Opcode::PackFloat2x16:
            Set(inst, nir_pack_32_2x16(&b, arg(0)));
            return;
        case Opcode::UnpackFloat2x16:
            Set(inst, nir_unpack_32_2x16(&b, arg(0)));
            return;
        case Opcode::PackHalf2x16:
            Fp(32);
            Set(inst, nir_pack_half_2x16(&b, arg(0)));
            return;
        case Opcode::UnpackHalf2x16:
            Fp(32);
            Set(inst, nir_unpack_half_2x16(&b, arg(0)));
            return;
        case Opcode::FPAbs32:
            Fp(32);
            Set(inst, nir_fabs(&b, arg(0)));
            return;
        case Opcode::FPNeg32:
            Fp(32);
            Set(inst, nir_fneg(&b, arg(0)));
            return;
        case Opcode::FPAdd32:
            FpExact(inst, 32);
            Set(inst, nir_fadd(&b, arg(0), arg(1)));
            return;
        case Opcode::FPMul32:
            FpExact(inst, 32);
            Set(inst, nir_fmul(&b, arg(0), arg(1)));
            return;
        case Opcode::FPFma32:
            FpExact(inst, 32);
            Set(inst, nir_ffma(&b, arg(0), arg(1), arg(2)));
            return;
        case Opcode::FPMax32:
            Fp(32, nir_fp_preserve_inf);
            Set(inst, nir_fmax(&b, arg(0), arg(1)));
            return;
        case Opcode::FPMin32:
            Fp(32, nir_fp_preserve_inf);
            Set(inst, nir_fmin(&b, arg(0), arg(1)));
            return;
        case Opcode::FPClamp32:
            b.fp_math_ctrl = nir_fp_preserve_inf;
            Set(inst, nir_fclamp(&b, arg(0), arg(1), arg(2)));
            return;
        case Opcode::FPSaturate32:
            b.fp_math_ctrl = nir_fp_preserve_inf;
            Set(inst, nir_fclamp(&b, arg(0), nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)));
            return;
        case Opcode::FPRoundEven32:
            Fp(32);
            Set(inst, nir_fround_even(&b, arg(0)));
            return;
        case Opcode::FPFloor32:
            Fp(32);
            Set(inst, nir_ffloor(&b, arg(0)));
            return;
        case Opcode::FPCeil32:
            Fp(32);
            Set(inst, nir_fceil(&b, arg(0)));
            return;
        case Opcode::FPTrunc32:
            Fp(32);
            Set(inst, nir_ftrunc(&b, arg(0)));
            return;
        case Opcode::FPRecip32:
            RequireExtendedCompute();
            Fp(32);
            // Match SPIR-V OpFDiv(1.0, value), including the shader's float controls.
            Set(inst, nir_fdiv(&b, nir_imm_float(&b, 1.0f), arg(0)));
            return;
        case Opcode::FPExp2:
            Fp(32);
            Set(inst, nir_fexp2(&b, arg(0)));
            return;
        case Opcode::FPLog2:
            Fp(32);
            Set(inst, nir_flog2(&b, arg(0)));
            return;
        case Opcode::FPIsNan32:
            Fp(32, nir_fp_preserve_nan | nir_fp_preserve_inf);
            Set(inst, nir_fneu(&b, arg(0), arg(0)));
            return;
        case Opcode::FPOrdEqual16:
        case Opcode::FPOrdEqual32:
        case Opcode::FPOrdLessThan16:
        case Opcode::FPOrdLessThan32:
        case Opcode::FPOrdGreaterThan16:
        case Opcode::FPOrdGreaterThan32:
        case Opcode::FPOrdLessThanEqual16:
        case Opcode::FPOrdLessThanEqual32:
        case Opcode::FPOrdGreaterThanEqual16:
        case Opcode::FPOrdGreaterThanEqual32:
        case Opcode::FPUnordLessThan16:
        case Opcode::FPUnordLessThan32:
        case Opcode::FPUnordGreaterThan16:
        case Opcode::FPUnordGreaterThan32:
        case Opcode::FPUnordLessThanEqual16:
        case Opcode::FPUnordLessThanEqual32:
        case Opcode::FPUnordGreaterThanEqual16:
        case Opcode::FPUnordGreaterThanEqual32:
            Set(inst, Compare(inst.GetOpcode(), arg(0), arg(1)));
            return;
        case Opcode::ConvertF16F32: {
            // f32 -> f16 with NaN as 0 (EmitConvertF16F32).
            Fp(16, FpControl(32));
            nir_def* const result{nir_f2f16(&b, arg(0))};
            Fp(16, nir_fp_preserve_nan | nir_fp_preserve_inf);
            nir_def* const is_nan{nir_fneu(&b, result, result)};
            IntOp();
            Set(inst, nir_bcsel(&b, is_nan, nir_imm_intN_t(&b, 0, 16), result));
            return;
        }
        case Opcode::ConvertF32F16:
            Fp(32, FpControl(16));
            Set(inst, nir_f2f32(&b, arg(0)));
            return;
        case Opcode::ConvertU32F32:
            Fp(32);
            Set(inst, nir_f2u32(&b, arg(0)));
            return;
        case Opcode::ConvertS32F32:
            Fp(32);
            Set(inst, nir_f2i32(&b, arg(0)));
            return;
        case Opcode::ConvertF32U32:
            Fp(32);
            Set(inst, nir_u2f32(&b, arg(0)));
            return;
        case Opcode::ConvertF32S32:
            Fp(32);
            Set(inst, nir_i2f32(&b, arg(0)));
            return;
        case Opcode::IAdd32: {
            if (inst.HasAssociatedPseudoOperation()) RequireExtendedCompute();
            nir_def* const lhs{arg(0)};
            nir_def* const rhs{arg(1)};
            nir_def* const result{nir_iadd(&b, lhs, rhs)};
            Set(inst, result);
            IntegerResultFlags(inst, result, lhs, rhs);
            return;
        }
        case Opcode::ISub32:
            Set(inst, nir_isub(&b, arg(0), arg(1)));
            return;
        case Opcode::IMul32:
            Set(inst, nir_imul(&b, arg(0), arg(1)));
            return;
        case Opcode::INeg32:
            Set(inst, nir_ineg(&b, arg(0)));
            return;
        case Opcode::IAbs32:
            Set(inst, nir_iabs(&b, arg(0)));
            return;
        case Opcode::ShiftLeftLogical32:
            Set(inst, nir_ishl(&b, arg(0), arg(1)));
            return;
        case Opcode::ShiftRightLogical32:
            Set(inst, nir_ushr(&b, arg(0), arg(1)));
            return;
        case Opcode::ShiftRightArithmetic32:
            Set(inst, nir_ishr(&b, arg(0), arg(1)));
            return;
        case Opcode::BitwiseAnd32:
        case Opcode::BitwiseOr32:
        case Opcode::BitwiseXor32:
        case Opcode::BitFieldUExtract:
        case Opcode::BitFieldSExtract:
            if (inst.HasAssociatedPseudoOperation()) RequireExtendedCompute();
            switch (inst.GetOpcode()) {
            case Opcode::BitwiseAnd32:
                Set(inst, nir_iand(&b, arg(0), arg(1)));
                break;
            case Opcode::BitwiseOr32:
                Set(inst, nir_ior(&b, arg(0), arg(1)));
                break;
            case Opcode::BitwiseXor32:
                Set(inst, nir_ixor(&b, arg(0), arg(1)));
                break;
            case Opcode::BitFieldUExtract:
                Set(inst, nir_ubitfield_extract(&b, arg(0), arg(1), arg(2)));
                break;
            default:
                Set(inst, nir_ibitfield_extract(&b, arg(0), arg(1), arg(2)));
                break;
            }
            IntegerResultFlags(inst, defs.at(&inst));
            return;
        case Opcode::BitwiseNot32:
            Set(inst, nir_inot(&b, arg(0)));
            return;
        case Opcode::FindUMsb32:
            RequireExtendedCompute();
            Set(inst, Eden::Nir::EmitFindUMsb32(&b, arg(0)));
            return;
        case Opcode::SMin32:
            Set(inst, nir_imin(&b, arg(0), arg(1)));
            return;
        case Opcode::UMin32:
            Set(inst, nir_umin(&b, arg(0), arg(1)));
            return;
        case Opcode::SMax32:
            Set(inst, nir_imax(&b, arg(0), arg(1)));
            return;
        case Opcode::UMax32:
            Set(inst, nir_umax(&b, arg(0), arg(1)));
            return;
        case Opcode::SLessThan:
            Set(inst, nir_ilt(&b, arg(0), arg(1)));
            return;
        case Opcode::ULessThan:
            Set(inst, nir_ult(&b, arg(0), arg(1)));
            return;
        case Opcode::IEqual:
            Set(inst, nir_ieq(&b, arg(0), arg(1)));
            return;
        case Opcode::SLessThanEqual:
            Set(inst, nir_ige(&b, arg(1), arg(0)));
            return;
        case Opcode::ULessThanEqual:
            Set(inst, nir_uge(&b, arg(1), arg(0)));
            return;
        case Opcode::SGreaterThan:
            Set(inst, nir_ilt(&b, arg(1), arg(0)));
            return;
        case Opcode::UGreaterThan:
            Set(inst, nir_ult(&b, arg(1), arg(0)));
            return;
        case Opcode::INotEqual:
            Set(inst, nir_ine(&b, arg(0), arg(1)));
            return;
        case Opcode::SGreaterThanEqual:
            Set(inst, nir_ige(&b, arg(0), arg(1)));
            return;
        case Opcode::UGreaterThanEqual:
            Set(inst, nir_uge(&b, arg(0), arg(1)));
            return;
        case Opcode::LogicalOr:
            Set(inst, nir_ior(&b, arg(0), arg(1)));
            return;
        case Opcode::LogicalAnd:
            Set(inst, nir_iand(&b, arg(0), arg(1)));
            return;
        case Opcode::LogicalXor:
            Set(inst, nir_ine(&b, arg(0), arg(1)));
            return;
        case Opcode::LogicalNot:
            Set(inst, nir_inot(&b, arg(0)));
            return;
        case Opcode::VoteAll:
            Set(inst, nir_vote_all(&b, 1, arg(0)));
            return;
        case Opcode::VoteAny:
            Set(inst, nir_vote_any(&b, 1, arg(0)));
            return;
        case Opcode::ShuffleIndex:
        case Opcode::ShuffleUp:
        case Opcode::ShuffleDown:
        case Opcode::ShuffleButterfly:
            Set(inst, Shuffle(inst, arg(0)));
            return;
        case Opcode::ImageSampleExplicitLod:
            RequireExtendedCompute();
            Set(inst, ImageSampleExplicitLod(inst));
            return;
        case Opcode::ImageFetch:
            if (inst.HasAssociatedPseudoOperation()) {
                Fail("sparse fetch");
            }
            Set(inst, ImageFetch(inst));
            return;
        case Opcode::ImageWrite:
            ImageWrite(inst);
            return;
        default:
            unsupported_opcode = inst.GetOpcode();
            Fail("opcode");
        }
    }

    nir_def* Compare(IR::Opcode opcode, nir_def* lhs, nir_def* rhs) {
        using IR::Opcode;
        Fp(lhs->bit_size, nir_fp_preserve_nan | nir_fp_preserve_inf);
        switch (opcode) {
        case Opcode::FPOrdEqual16:
        case Opcode::FPOrdEqual32:
            return nir_feq(&b, lhs, rhs);
        case Opcode::FPOrdLessThan16:
        case Opcode::FPOrdLessThan32:
            return nir_flt(&b, lhs, rhs);
        case Opcode::FPOrdGreaterThan16:
        case Opcode::FPOrdGreaterThan32:
            return nir_flt(&b, rhs, lhs);
        case Opcode::FPOrdLessThanEqual16:
        case Opcode::FPOrdLessThanEqual32:
            return nir_fge(&b, rhs, lhs);
        case Opcode::FPOrdGreaterThanEqual16:
        case Opcode::FPOrdGreaterThanEqual32:
            return nir_fge(&b, lhs, rhs);
        // spirv_to_nir: FUnordX(a, b) is !FOrd(inverse of X)(a, b).
        case Opcode::FPUnordLessThan16:
        case Opcode::FPUnordLessThan32:
            return nir_inot(&b, nir_fge(&b, lhs, rhs));
        case Opcode::FPUnordGreaterThan16:
        case Opcode::FPUnordGreaterThan32:
            return nir_inot(&b, nir_fge(&b, rhs, lhs));
        case Opcode::FPUnordLessThanEqual16:
        case Opcode::FPUnordLessThanEqual32:
            return nir_inot(&b, nir_flt(&b, rhs, lhs));
        case Opcode::FPUnordGreaterThanEqual16:
        case Opcode::FPUnordGreaterThanEqual32:
            return nir_inot(&b, nir_flt(&b, lhs, rhs));
        default:
            Fail("comparison");
        }
    }

public:
    std::optional<IR::Opcode> unsupported_opcode;

private:
    const Profile& profile;
    IR::Program& program;
    const Info& info;
    nir_builder b{};
    unsigned fp16_control{};
    unsigned fp32_control{};
    unsigned fp64_control{};
    std::unordered_map<const IR::Inst*, nir_def*> defs;
    std::unordered_map<const IR::Inst*, nir_variable*> phi_vars;
    std::unordered_map<const IR::Block*, std::vector<std::pair<nir_variable*, IR::Value>>> phi_stores;
    std::unordered_map<u32, u32> cbuf_bindings;
    std::unordered_map<u32, u32> ssbo_bindings;
    std::vector<nir_variable*> texture_buffer_vars;
    std::vector<nir_variable*> texture_vars;
    std::vector<nir_variable*> image_vars;
};

bool Attach(const Profile& profile, IR::Program& program, void* module) {
    Translator translator{profile, program};
    nir_shader* shader{};
    try {
        shader = translator.Run();
    } catch (const Unsupported& unsupported) {
        static const bool log{std::getenv("EDEN_NX2_COMPILE_TIMES") != nullptr};
        if (log) {
            LOG_WARNING(Render_Vulkan, "NX2_NIR fallback reason={} opcode={}", unsupported.what,
                        translator.unsupported_opcode
                            ? std::string{IR::NameOf(*translator.unsupported_opcode)}
                            : std::string{"-"});
        }
        return false;
    }
    reinterpret_cast<vk_shader_module*>(module)->nir = shader;
    return true;
}

void Detach(void* module) {
    vk_shader_module* const shader_module{reinterpret_cast<vk_shader_module*>(module)};
    if (shader_module->nir != nullptr) {
        ralloc_free(shader_module->nir);
        shader_module->nir = nullptr;
    }
}

} // Anonymous namespace

// Registers the NIR backend with the Vulkan renderer (headless/main.cpp, RADV builds).
void EdenRegisterNirBackend() {
    Vulkan::NirHandoff::attach.store(&Attach, std::memory_order_release);
    Vulkan::NirHandoff::detach.store(&Detach, std::memory_order_release);
}
