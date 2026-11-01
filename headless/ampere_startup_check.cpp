// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
using Shader::Ampere::Instruction;
static auto Lower(Instruction instruction) {
    const std::array<Instruction, 2> code{instruction, Instruction{{0x794d, 0, 0x03800000, 0}}};
    return Shader::Ampere::LowerToMaxwell(code);
}
// Execute the small Maxwell subset used by the signed-byte conversion, checking
// saturation and NaN handling rather than merely accepting the native opcode.
static std::int32_t ByteConversion(float input) {
    const auto result = Lower({{0x00157305, 3, 0x0021e100, 0x0001e400}});
    assert(result);
    std::array<std::uint32_t, 256> r{};
    r[3] = std::bit_cast<std::uint32_t>(input);
    for (const auto op : result.instructions) {
        const auto dst = op & 255;
        const auto a = (op >> 8) & 255;
        const auto b = (op >> 20) & 255;
        if ((op >> 56) == 1) {
            r[dst] = static_cast<std::uint32_t>(op >> 20);
        } else if ((op & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL) {
            assert(((op >> 8) & 3) == 1 && ((op >> 10) & 3) == 2);
            assert(((op >> 39) & 3) == 3 && ((op >> 44) & 1) == 1);
            float f = std::bit_cast<float>(r[b]);
            if (std::isnan(f) || std::fpclassify(f) == FP_SUBNORMAL) f = 0;
            f = std::clamp(std::trunc(f), -32768.0f, 32767.0f);
            r[dst] = static_cast<std::uint32_t>(static_cast<std::int32_t>(f));
        } else if ((op & 0xfff0000000000000ULL) == 0x5c20000000000000ULL ||
                   (op & 0xfff0000000000000ULL) == 0x3820000000000000ULL) {
            assert((op >> 48) & 1);
            const auto left = static_cast<std::int32_t>(r[a]);
            const auto right = (op >> 56) == 0x38 ?
                static_cast<std::int32_t>((op >> 20) & 0x7ffff) : static_cast<std::int32_t>(r[b]);
            const bool minimum = ((op >> 39) & 15) == 7;
            r[dst] = static_cast<std::uint32_t>(minimum ? std::min(left, right) : std::max(left, right));
        }
    }
    return static_cast<std::int32_t>(r[21]);
}
int main() {
    // Body exits must visit OUT.FINAL and clear ATEXIT_PC before terminating.
    const std::array<Instruction, 9> cleanup{{
        {{0x0000734e, 0, 0, 0}},
        {{0x00037802, 0x60, 0x00000f00, 0}},
        {{0x00007210, 3, 0x07f1e0ff, 0}},
        {{0xff017224, 0xff, 0x000e0401, 0}},
        {{0x00007357, 0, 0, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
        {{0x00ff7324, 0xff, 0, 0}},
        {{0x00007957, 0, 0, 0}},
        {{0x0000794d, 0, 0x03800000, 0}},
    }};
    Shader::Ampere::ProgramCaptureState capture;
    for (unsigned i = 0; i < cleanup.size(); ++i)
        assert(capture.Observe(cleanup[i], i) ==
               (i + 1 == cleanup.size() ? Shader::Ampere::ProgramCaptureState::Result::Complete :
                                         Shader::Ampere::ProgramCaptureState::Result::Continue));
    const auto cull = Shader::Ampere::LowerToMaxwell(cleanup, 0x21fffffff0);
    assert(cull && cull.source_size == sizeof(cleanup));
    assert((cull.instructions[cull.source_instruction_offsets[5]] >> 52) == 0xe24);
    assert((cull.instructions.back() >> 52) == 0xe30);
    assert(!Shader::Ampere::LowerToMaxwell(std::span{cleanup}.first(6)));
    auto malformed = cleanup;
    malformed[1].words[1] = 0x61;
    assert(!Shader::Ampere::LowerToMaxwell(malformed));
    const std::array<Instruction, 5> pc_code{{
        {{0x7918, 0, 0, 0}}, {{0x7918, 0, 0, 0}}, {{0x7918, 0, 0, 0}},
        {{0x734e, 0, 0, 0}}, {{0x794d, 0, 0x03800000, 0}},
    }};
    const auto pc = Shader::Ampere::LowerToMaxwell(pc_code, 0x21fffffff0);
    assert(pc);
    const auto pc_output = pc.source_instruction_offsets[3];
    assert(((pc.instructions[pc_output] >> 20) & 0xffffffff) == 0x20);
    assert(((pc.instructions[pc_output + 1] >> 20) & 0xffffffff) == 0x22);
    const auto stage_map = Lower({{0x020c7923, 0, 0, 0}});
    assert(stage_map && (stage_map.instructions[0] & 255) == 12);
    assert(((stage_map.instructions[0] >> 8) & 255) == 2);
    assert(Lower({{0x00007919, 0, 0x00001d00, 0}}));
    assert(!Lower({{0x020c7923, 1, 0, 0}}));
    const auto short_trunc = Lower({{0x00027305, 2, 0x0021e800, 0}});
    assert(short_trunc);
    bool has_short_trunc = false;
    for (const auto op : short_trunc.instructions)
        if ((op & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL)
            has_short_trunc = ((op >> 8) & 3) == 1 && ((op >> 39) & 3) == 3;
    assert(has_short_trunc);
    const auto carry_add = Lower({{0xff017224, 0xff, 0x000e0401, 0}});
    assert(carry_add);
    for (const auto value : {0u, 17u, 0xffffffffu}) {
        for (const bool carry : {false, true}) {
            std::array<std::uint32_t, 256> r{};
            r[1] = value;
            for (const auto op : carry_add.instructions) {
                const auto dst = op & 255, a = (op >> 8) & 255, b = (op >> 20) & 255;
                if ((op >> 56) == 1) r[dst] = static_cast<std::uint32_t>(op >> 20);
                else if ((op & 0xfff0000000000000ULL) == 0x5ca0000000000000ULL) {
                    assert(((op >> 39) & 15) == 0); // carry predicate P0
                    r[dst] = carry ? r[a] : r[b];
                } else if ((op & 0xfff0000000000000ULL) == 0x5c10000000000000ULL) {
                    r[dst] = r[a] + r[b];
                }
            }
            assert(r[1] == value + static_cast<unsigned>(carry));
        }
    }
    const auto vote = Lower({{0x00107886, 0, 0x038e0100, 0x000fe400}});
    assert(vote && (vote.instructions[0] & 255) == 232);
    assert(((vote.instructions[0] >> 48) & 3) == 1); // ANY, UR16
    const auto compare = Lower({{0x34007234, 0x30000034, 0x00702800, 0x000fe400}});
    assert(compare);
    const auto h = compare.instructions[0];
    assert((h & 7) == 7 && ((h >> 3) & 7) == 0);
    assert(((h >> 35) & 15) == 2); // EQ
    assert(((h >> 47) & 3) == 2 && ((h >> 28) & 3) == 3); // H0/H1 broadcasts
    const auto mufu = Lower({{0x000c7d08, 6, 0x08001000, 0x000e2200}});
    assert(mufu && ((mufu.instructions[0] >> 8) & 255) == 222);
    assert(((mufu.instructions[0] >> 20) & 15) == 4); // RCP
    const auto ldc = Lower({{0xff139b82, 0x03c00800, 0, 0x000e6200}});
    assert(ldc && ((ldc.instructions[0] >> 48) & 7) == 0); // U8
    assert(((ldc.instructions[0] >> 36) & 31) == 15);
    assert(((ldc.instructions[0] >> 20) & 0xffff) == 0x20);
    const auto ceil = Lower({{0x00027305, 2, 0x0020b000, 0x000e3000}});
    assert(ceil);
    bool has_ceil = false;
    for (const auto op : ceil.instructions)
        if ((op & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL)
            has_ceil = ((op >> 39) & 3) == 2;
    assert(has_ceil);
    for (const float value : {-std::numeric_limits<float>::infinity(), -1000.0f, -128.9f,
                             -127.9f, -0.9f, 0.0f, 0.9f, 126.9f, 127.9f, 1000.0f,
                             std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        const auto expected = std::isnan(value) ? 0 :
            static_cast<int>(std::clamp(std::trunc(value), -128.0f, 127.0f));
        assert(ByteConversion(value) == expected);
    }
    for (const auto operation : {1u, 2u, 3u}) {
        const auto out = Lower({{0x00007324, 0, operation << 14, 0}});
        assert(out && ((out.instructions[0] >> 39) & 3) == operation);
    }
    assert(!Lower({{0xff139b82, 0x03c00800, 6u << 9, 0x000e6200}}));
    assert(!Lower({{0x000c7d08, 30, 0x08001000, 0x000e2200}}));
}
