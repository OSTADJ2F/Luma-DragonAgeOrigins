#pragma once

#include <algorithm>
#include <bit>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dxbc.h"

// MSAA alpha to coverage for alpha tested materials, by patching their pixel shaders' clip. First written for Mirror's Edge, also
// used by Bayonetta and MGR:R: all run through dgVoodoo, whose translation of a D3D9 clip(alpha - k) (or texkill) is
// "add rD, rS.c, l(-k, -k, -k, -k)" (MGR:R: "mad rD, a, b, l(-k, ...)") -> lt -> discard_nz.
namespace CoveragePatch
{
   // The clip value's sharpened alpha c = saturate(d / fwidth(d) + 0.5) (Golus; the plain alpha would thin cut-outs out), right after
   // the add, and oMask = the first n = floor(c * samples + 0.5) samples. oMask rather than A2C: the alpha output may carry other
   // data (Mirror's Edge's scene alpha is the linear depth). The clip's value is then replaced by n - 0.5, so only an empty mask
   // discards and coverage ramps 0..1 across the vanilla edge (thin wires keep their weight). When something else reads that value
   // the discard is left as it was ("inward only": c >= 0.5 where a pixel is kept, edges soften inward). A material's passes that
   // share the clip get the same mask, which keeps a light pass off the samples its base pass left out.
   // Only thresholds k in ("min_threshold", 1) count as a clip: a near-zero one skips invisible texels of blended draws rather than
   // cutting out a shape. Empty, with "error", for any other shader ("no clip..." for the expected refusals).
   inline std::vector<uint8_t> PatchPixelShader(const uint8_t* code, size_t size, std::string* error, float min_threshold = 0.f)
   {
      std::vector<DXBC::Chunk> chunks;
      std::vector<DXBC::SignatureElement> outputs;
      std::vector<uint32_t> tokens;
      std::vector<DXBC::Instruction> instructions;
      size_t first_body = 0;
      DXBC::Chunk* program = nullptr;
      DXBC::Chunk* output_signature = nullptr;
      if (DXBC::ReadChunks(code, size, &chunks))
      {
         program = DXBC::FindChunk(&chunks, DXBC::FourCC("SHEX"), DXBC::FourCC("SHDR"));
         output_signature = DXBC::FindChunk(&chunks, DXBC::FourCC("OSGN"));
      }
      if (!program || !output_signature || !DXBC::ReadSignature(output_signature->data, &outputs) || !DXBC::ReadProgram(*program, &tokens, &instructions, &first_body))
      {
         *error = "unreadable";
         return {};
      }
      // sampleinfo and oMask are SM 4.1 (dgVoodoo 2.81.3's ps_4_0 has neither)
      if (DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(tokens[0]) * 10 + DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(tokens[0]) < 41)
      {
         *error = "no clip (below SM 4.1)";
         return {};
      }

      // Flow control nesting: 1 opens a block, -1 closes one
      const auto nesting = [](auto opcode)
      {
         switch (opcode)
         {
         case D3D10_SB_OPCODE_IF:
         case D3D10_SB_OPCODE_LOOP:
         case D3D10_SB_OPCODE_SWITCH:
            return 1;
         case D3D10_SB_OPCODE_ENDIF:
         case D3D10_SB_OPCODE_ENDLOOP:
         case D3D10_SB_OPCODE_ENDSWITCH:
            return -1;
         default:
            return 0;
         }
      };
      // Texture and buffer fetches: their address operands don't flow into the fetched value
      const auto fetch = [](auto opcode)
      {
         switch (opcode)
         {
         case D3D10_SB_OPCODE_SAMPLE:
         case D3D10_SB_OPCODE_SAMPLE_L:
         case D3D10_SB_OPCODE_SAMPLE_D:
         case D3D10_SB_OPCODE_SAMPLE_B:
         case D3D10_SB_OPCODE_SAMPLE_C:
         case D3D10_SB_OPCODE_SAMPLE_C_LZ:
         case D3D10_SB_OPCODE_LD:
         case D3D10_SB_OPCODE_LD_MS:
         case D3D10_1_SB_OPCODE_GATHER4:
         case D3D11_SB_OPCODE_GATHER4_C:
         case D3D11_SB_OPCODE_GATHER4_PO:
         case D3D11_SB_OPCODE_GATHER4_PO_C:
         case D3D11_SB_OPCODE_LD_UAV_TYPED:
         case D3D11_SB_OPCODE_LD_RAW:
         case D3D11_SB_OPCODE_LD_STRUCTURED:
            return true;
         default:
            return false;
         }
      };
      // The clip's add (or mad, see below), outside flow control (the derivatives need all the quad's lanes): a temp destination, for an
      // add a temp source without a modifier, and the threshold as a replicated immediate, whose value reaches a discard's condition
      // (data flow through temp components). Each such instruction is tried in order: other subtractions of a constant (a distortion map's UV offset, Bayonetta's
      // translucent Purgatorio humans 0xB7CD0392) come before the alpha test's and never reach the discard.
      // "symmetric": nothing else reads what the clip computed (no output written from it before the discard, no read of it after),
      // so the patch can replace it with the coverage's own test (see below).
      size_t clip = SIZE_MAX;
      uint32_t clip_register = 0;
      uint32_t clip_component = 0;
      uint32_t clip_mask = 0; // Operand token form (x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X)
      bool symmetric = true;
      const auto follow = [&](size_t candidate, uint32_t candidate_register, uint32_t candidate_mask)
      {
         bool reaches = false;
         // Components (bits xyzw) of each temp holding the clip's value or values computed from it
         std::unordered_map<uint32_t, uint8_t> tainted = {{candidate_register, uint8_t(candidate_mask >> 4)}};
         int depth = 0;
         for (size_t i = candidate + 1; i < instructions.size(); i++)
         {
            const DXBC::Instruction& instruction = instructions[i];
            depth += nesting(instruction.opcode);
            // The first operand is the destination, except for these; two destinations (sincos, udiv...) aren't followed
            bool has_destination = true;
            bool two_destinations = false;
            switch (instruction.opcode)
            {
            case D3D10_SB_OPCODE_IF:
            case D3D10_SB_OPCODE_BREAKC:
            case D3D10_SB_OPCODE_CONTINUEC:
            case D3D10_SB_OPCODE_RETC:
            case D3D10_SB_OPCODE_CALLC:
            case D3D10_SB_OPCODE_DISCARD:
            case D3D10_SB_OPCODE_SWITCH:
            case D3D10_SB_OPCODE_CASE:
               has_destination = false;
               break;
            case D3D10_SB_OPCODE_SINCOS:
            case D3D10_SB_OPCODE_UDIV:
            case D3D10_SB_OPCODE_UMUL:
            case D3D10_SB_OPCODE_IMUL:
            case D3D11_SB_OPCODE_SWAPC:
            case D3D11_SB_OPCODE_UADDC:
            case D3D11_SB_OPCODE_USUBB:
               two_destinations = true;
               break;
            default:
               break;
            }
            bool reads = false;
            bool first_operand = true;
            bool temp_destination = false;
            uint32_t written_register = 0;
            uint8_t written_mask = 0;
            const bool walked = DXBC::WalkOperands(tokens, instruction, [&](size_t token_position, size_t index_position)
               {
                  const uint32_t token = tokens[token_position];
                  const D3D10_SB_OPERAND_TYPE type = DECODE_D3D10_SB_OPERAND_TYPE(token);
                  const auto mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token);
                  if (std::exchange(first_operand, false) && has_destination)
                  {
                     // Any destination's mask (outputs too, for the swizzle positions below); a 1 component one (oMask, oDepth) is x
                     const bool masked = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) == D3D10_SB_OPERAND_4_COMPONENT && mode == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE;
                     written_mask = (masked ? uint8_t(DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token) >> 4) : uint8_t(0x1));
                     temp_destination = type == D3D10_SB_OPERAND_TYPE_TEMP && index_position != DXBC::no_index && masked;
                     if (temp_destination)
                     {
                        written_register = tokens[index_position];
                     }
                     return true;
                  }
                  if (type != D3D10_SB_OPERAND_TYPE_TEMP || index_position == DXBC::no_index)
                     return true;
                  const auto found = tainted.find(tokens[index_position]);
                  if (found == tainted.end())
                     return true;
                  uint8_t components = 0;
                  if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
                  {
                     // The swizzle positions the instruction uses: the dot products' first 2 to 4, a component wise operation's
                     // written ones, all of them otherwise (texture operations, no destination)
                     uint8_t positions = 0xF;
                     switch (instruction.opcode)
                     {
                     case D3D10_SB_OPCODE_DP2:
                        positions = 0x3;
                        break;
                     case D3D10_SB_OPCODE_DP3:
                        positions = 0x7;
                        break;
                     case D3D10_SB_OPCODE_DP4:
                     case D3D10_SB_OPCODE_RESINFO:
                     case D3D10_1_SB_OPCODE_LOD:
                     case D3D10_1_SB_OPCODE_SAMPLE_POS:
                     case D3D10_1_SB_OPCODE_SAMPLE_INFO:
                     case D3D11_SB_OPCODE_BUFINFO:
                        break;
                     default:
                        if (!fetch(instruction.opcode) && has_destination && !two_destinations)
                        {
                           positions = written_mask;
                        }
                        break;
                     }
                     for (uint32_t component = 0; component < 4; component++)
                     {
                        if (positions & (1u << component))
                        {
                           components |= uint8_t(1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(token, component));
                        }
                     }
                  }
                  else if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE)
                  {
                     components = uint8_t(1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(token));
                  }
                  else
                  {
                     components = 0xF;
                  }
                  reads |= (found->second & components) != 0;
                  return true; });
            if (!walked)
            {
               symmetric = false;
               break;
            }
            if (reads)
            {
               // Read after the discard: the patch must keep the value
               if (reaches)
               {
                  symmetric = false;
                  break;
               }
               if (instruction.opcode == D3D10_SB_OPCODE_DISCARD)
               {
                  reaches = true;
                  continue;
               }
               // Up to the discard the clip's value may flow through temps, not out of the shader (an escaping read keeps the walk
               // going: the value can still reach a discard through another temp). Texture coordinates don't flow into what's fetched:
               // a distortion map's UV offset (Bayonetta's 0xB7CD0392, 0xBB5CACB5) reaches the alpha test only through the fetch.
               if (fetch(instruction.opcode) || two_destinations || !temp_destination)
               {
                  symmetric = false;
               }
               else
               {
                  tainted[written_register] |= written_mask;
                  continue;
               }
            }
            // Overwritten (only where it surely runs)
            if (temp_destination && !two_destinations && depth == 0)
            {
               if (const auto found = tainted.find(written_register); found != tainted.end())
               {
                  found->second &= uint8_t(~written_mask);
               }
            }
         }
         return reaches;
      };
      int outer_depth = 0;
      for (size_t i = first_body; i < instructions.size() && clip == SIZE_MAX; i++)
      {
         const DXBC::Instruction& instruction = instructions[i];
         if (const int step = nesting(instruction.opcode); step != 0)
         {
            outer_depth += step;
            continue;
         }
         const uint32_t* const t = &tokens[instruction.begin];
         if (outer_depth != 0 || DECODE_IS_D3D10_SB_OPCODE_EXTENDED(t[0]))
            continue;
         // The threshold's immediate: dgVoodoo's carry an xyzw swizzle (0x00004E46), fxc's don't (0x00004002), the type and width only
         const uint32_t* immediate = nullptr;
         if (instruction.opcode == D3D10_SB_OPCODE_ADD && instruction.length == 10)
         {
            if (DECODE_D3D10_SB_OPERAND_TYPE(t[3]) != D3D10_SB_OPERAND_TYPE_TEMP || DECODE_IS_D3D10_SB_OPERAND_EXTENDED(t[3]))
               continue;
            immediate = &t[5];
         }
         // "mad rD, a, b, l(-k)": the clip of an alpha scaled first (MGR:R's texkill of alpha * c - k). Its sources can be any operand
         // form, so the immediate is the last operand found by walking them.
         else if (instruction.opcode == D3D10_SB_OPCODE_MAD)
         {
            size_t last_operand = 0;
            int operands = 0;
            if (!DXBC::WalkOperands(tokens, instruction, [&](size_t token_position, size_t)
                   {
                      last_operand = token_position;
                      operands++;
                      return true; }) ||
                operands != 4 || last_operand + 5 != instruction.begin + instruction.length)
               continue;
            immediate = &tokens[last_operand];
         }
         else
         {
            continue;
         }
         if (DECODE_D3D10_SB_OPERAND_TYPE(t[1]) != D3D10_SB_OPERAND_TYPE_TEMP || DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(t[1]) != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE || DECODE_IS_D3D10_SB_OPERAND_EXTENDED(t[1]) ||
             DECODE_D3D10_SB_OPERAND_TYPE(immediate[0]) != D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(immediate[0]) != D3D10_SB_OPERAND_4_COMPONENT)
            continue;
         const float threshold = -std::bit_cast<float>(immediate[1]);
         if (!(threshold > min_threshold && threshold < 1.f) || immediate[2] != immediate[1] || immediate[3] != immediate[1] || immediate[4] != immediate[1])
            continue;
         const uint32_t mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(t[1]);
         symmetric = true;
         if (!follow(i, t[2], mask))
            continue;
         clip = i;
         clip_register = t[2];
         clip_mask = mask;
         clip_component = std::countr_zero(clip_mask >> 4);
      }
      if (clip == SIZE_MAX)
      {
         *error = "no clip";
         return {};
      }
      if (std::ranges::any_of(outputs, [](const DXBC::SignatureElement& element)
             { return element.name == "SV_Coverage"; }))
      {
         *error = "writes oMask";
         return {};
      }
      const size_t last_output = DXBC::FindLastDeclaration(instructions, first_body, {D3D10_SB_OPCODE_DCL_OUTPUT, D3D10_SB_OPCODE_DCL_OUTPUT_SGV, D3D10_SB_OPCODE_DCL_OUTPUT_SIV});
      if (last_output == first_body)
      {
         *error = "no outputs";
         return {};
      }

      // dcl_output oMask, as fxc encodes it, after the other outputs
      constexpr uint32_t omask = ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK);
      uint32_t temp = 0;
      std::vector<uint32_t> patched = DXBC::CopyDeclarations(tokens, instructions, first_body, 1, &temp, [&](size_t i)
         { return i == last_output ? std::vector<uint32_t>{DXBC::Opcode(D3D10_SB_OPCODE_DCL_OUTPUT, 2), omask} : std::vector<uint32_t>{}; });
      constexpr uint32_t immediate_1 = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32);
      constexpr uint32_t x_dst = DXBC::Destination(D3D10_SB_OPERAND_TYPE_TEMP, D3D10_SB_OPERAND_4_COMPONENT_MASK_X);
      constexpr uint32_t y_dst = DXBC::Destination(D3D10_SB_OPERAND_TYPE_TEMP, D3D10_SB_OPERAND_4_COMPONENT_MASK_Y);
      const uint32_t t_x = DXBC::Source(D3D10_SB_OPERAND_TYPE_TEMP, 0, 0, 0, 0);
      const uint32_t t_y = DXBC::Source(D3D10_SB_OPERAND_TYPE_TEMP, 1, 1, 1, 1);
      const uint32_t d = DXBC::Source(D3D10_SB_OPERAND_TYPE_TEMP, clip_component, clip_component, clip_component, clip_component);
      constexpr uint32_t abs_modifier = ENCODE_D3D10_SB_EXTENDED_OPERAND_TYPE(D3D10_SB_EXTENDED_OPERAND_MODIFIER) | ENCODE_D3D10_SB_EXTENDED_OPERAND_MODIFIER(D3D10_SB_OPERAND_MODIFIER_ABS);
      // sampleinfo's float form: the render target's sample count
      constexpr uint32_t rasterizer_x = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_RASTERIZER) | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE);
      static_assert(omask == 0x0000F000 && rasterizer_x == 0x0000E00A); // fxc's "dcl_output oMask" and "sampleinfo r1.y, rasterizer.x"
      std::vector<uint32_t> coverage = {
         // deriv_rtx t.x, d | deriv_rty t.y, d | add t.x, |t.x|, |t.y| | max t.x, t.x, l(0.0001)
         DXBC::Opcode(D3D10_SB_OPCODE_DERIV_RTX, 5), x_dst, temp, d, clip_register,
         DXBC::Opcode(D3D10_SB_OPCODE_DERIV_RTY, 5), y_dst, temp, d, clip_register,
         DXBC::Opcode(D3D10_SB_OPCODE_ADD, 9), x_dst, temp, t_x | ENCODE_D3D10_SB_OPERAND_EXTENDED(1), abs_modifier, temp, t_y | ENCODE_D3D10_SB_OPERAND_EXTENDED(1), abs_modifier, temp,
         DXBC::Opcode(D3D10_SB_OPCODE_MAX, 7), x_dst, temp, t_x, temp, immediate_1, std::bit_cast<uint32_t>(0.0001f),
         // div t.x, d, t.x | add_sat t.x, t.x, l(0.5): the sharpened alpha
         DXBC::Opcode(D3D10_SB_OPCODE_DIV, 7), x_dst, temp, d, clip_register, t_x, temp,
         DXBC::Opcode(D3D10_SB_OPCODE_ADD, 7, true), x_dst, temp, t_x, temp, immediate_1, std::bit_cast<uint32_t>(0.5f),
         // sampleinfo t.y, rasterizer.x | mad t.x, t.x, t.y, l(0.5) | ftou t.x, t.x: the covered sample count, rounded
         DXBC::Opcode(D3D10_1_SB_OPCODE_SAMPLE_INFO, 4), y_dst, temp, rasterizer_x,
         DXBC::Opcode(D3D10_SB_OPCODE_MAD, 9), x_dst, temp, t_x, temp, t_y, temp, immediate_1, std::bit_cast<uint32_t>(0.5f),
         DXBC::Opcode(D3D10_SB_OPCODE_FTOU, 5), x_dst, temp, t_x, temp};
      if (symmetric)
      {
         // utof t.y, t.x | add rD.mask, t.y, l(-0.5): the clip now discards only an empty mask, so coverage ramps 0..1 across the
         // vanilla edge (the alpha test's line at half coverage) instead of 0.5..1 inside it
         coverage.insert(coverage.end(), {DXBC::Opcode(D3D10_SB_OPCODE_UTOF, 5), y_dst, temp, t_x, temp,
                                            DXBC::Opcode(D3D10_SB_OPCODE_ADD, 7), DXBC::Destination(D3D10_SB_OPERAND_TYPE_TEMP, clip_mask), clip_register, t_y, temp, immediate_1, std::bit_cast<uint32_t>(-0.5f)});
      }
      // ishl t.x, l(1), t.x | iadd oMask, t.x, l(-1)
      coverage.insert(coverage.end(), {DXBC::Opcode(D3D10_SB_OPCODE_ISHL, 7), x_dst, temp, immediate_1, 1, t_x, temp,
                                         DXBC::Opcode(D3D10_SB_OPCODE_IADD, 6), omask | ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT), t_x, temp, immediate_1, 0xFFFFFFFF});
      for (size_t i = first_body; i < instructions.size(); i++)
      {
         patched.insert(patched.end(), tokens.begin() + instructions[i].begin, tokens.begin() + instructions[i].begin + instructions[i].length);
         if (i == clip)
         {
            patched.insert(patched.end(), coverage.begin(), coverage.end());
         }
      }
      DXBC::WriteProgram(&patched, program);
      // fxc's element for it: uint, no register, x written
      outputs.push_back({.name = "SV_Coverage", .semantic_index = 0, .system_value = 0, .component_type = 1, .reg = 0xFFFFFFFF, .mask = 0x1, .rw_mask = 0xE});
      output_signature->data = DXBC::WriteSignature(outputs);
      *error = (symmetric ? "" : "inward only (the clip's value is read elsewhere)");
      return DXBC::WriteChunks(chunks);
   }
} // namespace CoveragePatch
