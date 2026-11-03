#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_patching.h"
#include "../utils/shader_compiler.hpp"

// Reading and rewriting whole DXBC containers: chunks, input/output signatures (ISGN/OSGN) and the SHEX/SHDR token stream.
// Core's "Patch::BuildPatchedContainer" (patch.hpp) only swaps the program chunk; patches that add inputs or outputs (the motion
// vector shaders, "motion_vector_patch.h") rewrite the signatures too, and the container hash after.
namespace DXBC
{
   constexpr uint32_t FourCC(const char (&name)[5])
   {
      return uint32_t(uint8_t(name[0])) | (uint32_t(uint8_t(name[1])) << 8) | (uint32_t(uint8_t(name[2])) << 16) | (uint32_t(uint8_t(name[3])) << 24);
   }

   struct Chunk
   {
      uint32_t fourcc;
      std::vector<uint8_t> data;
   };

   inline bool ReadChunks(const uint8_t* code, size_t size, std::vector<Chunk>* chunks)
   {
      if (size < sizeof(Shader::DXBCHeader) || std::memcmp(code, "DXBC", 4) != 0)
         return false;
      const auto* header = reinterpret_cast<const Shader::DXBCHeader*>(code);
      if (header->file_size != size || sizeof(Shader::DXBCHeader) + size_t(header->chunk_count) * sizeof(uint32_t) > size)
         return false;
      for (uint32_t i = 0; i < header->chunk_count; i++)
      {
         const size_t offset = header->chunk_offsets[i];
         if (offset + 8 > size)
            return false;
         uint32_t fourcc, chunk_size;
         std::memcpy(&fourcc, code + offset, 4);
         std::memcpy(&chunk_size, code + offset + 4, 4);
         if (offset + 8 + chunk_size > size)
            return false;
         chunks->push_back({fourcc, std::vector<uint8_t>(code + offset + 8, code + offset + 8 + chunk_size)});
      }
      return true;
   }

   // The first chunk with the code (or the alternative one: the program is SHEX or SHDR), or null
   inline Chunk* FindChunk(std::vector<Chunk>* chunks, uint32_t fourcc, uint32_t alternative = 0)
   {
      const auto chunk = std::ranges::find_if(*chunks, [&](const Chunk& chunk)
         { return chunk.fourcc == fourcc || chunk.fourcc == alternative; });
      return chunk != chunks->end() ? &*chunk : nullptr;
   }

   // A new container (sizes, offsets, checksum)
   inline std::vector<uint8_t> WriteChunks(const std::vector<Chunk>& chunks)
   {
      size_t size = sizeof(Shader::DXBCHeader) + chunks.size() * sizeof(uint32_t);
      for (const Chunk& chunk : chunks)
         size += 8 + chunk.data.size();
      std::vector<uint8_t> code(size);
      auto* header = reinterpret_cast<Shader::DXBCHeader*>(code.data());
      std::memcpy(header->format_name, "DXBC", 4);
      header->version = 1;
      header->file_size = uint32_t(size);
      header->chunk_count = uint32_t(chunks.size());
      size_t offset = sizeof(Shader::DXBCHeader) + chunks.size() * sizeof(uint32_t);
      for (size_t i = 0; i < chunks.size(); i++)
      {
         header->chunk_offsets[i] = uint32_t(offset);
         const uint32_t chunk_size = uint32_t(chunks[i].data.size());
         std::memcpy(code.data() + offset, &chunks[i].fourcc, 4);
         std::memcpy(code.data() + offset + 4, &chunk_size, 4);
         std::memcpy(code.data() + offset + 8, chunks[i].data.data(), chunk_size);
         offset += 8 + chunk_size;
      }
      const Hash::MD5::Digest digest = Shader::CalcDXBCHash(code.data(), code.size());
      std::memcpy(header->hash, &digest.data, Shader::DXBCHeader::hash_size);
      return code;
   }

   // An ISGN/OSGN element (24 bytes each, names after them)
   struct SignatureElement
   {
      std::string name;
      uint32_t semantic_index;
      uint32_t system_value;
      uint32_t component_type;
      uint32_t reg;
      uint8_t mask;
      uint8_t rw_mask; // Components used (inputs) or never written (outputs)
   };

   inline bool ReadSignature(const std::vector<uint8_t>& data, std::vector<SignatureElement>* elements)
   {
      if (data.size() < 8)
         return false;
      uint32_t count, first;
      std::memcpy(&count, data.data(), 4);
      std::memcpy(&first, data.data() + 4, 4);
      if (first != 8 || 8 + size_t(count) * 24 > data.size())
         return false;
      for (uint32_t i = 0; i < count; i++)
      {
         const uint8_t* entry = data.data() + 8 + i * 24;
         uint32_t values[5];
         std::memcpy(values, entry, sizeof(values));
         const auto name_end = std::find(data.begin() + std::min<size_t>(values[0], data.size()), data.end(), uint8_t(0));
         if (values[0] >= data.size() || name_end == data.end())
            return false;
         elements->push_back({std::string(data.begin() + values[0], name_end), values[1], values[2], values[3], values[4], entry[20], entry[21]});
      }
      return true;
   }

   inline std::vector<uint8_t> WriteSignature(const std::vector<SignatureElement>& elements)
   {
      std::vector<uint8_t> data(8 + elements.size() * 24);
      const uint32_t header[2] = {uint32_t(elements.size()), 8};
      std::memcpy(data.data(), header, sizeof(header));
      std::unordered_map<std::string, uint32_t> name_offsets;
      for (size_t i = 0; i < elements.size(); i++)
      {
         const SignatureElement& element = elements[i];
         auto [name, added] = name_offsets.try_emplace(element.name, uint32_t(data.size()));
         if (added)
            data.insert(data.end(), element.name.c_str(), element.name.c_str() + element.name.size() + 1);
         const uint32_t values[5] = {name->second, element.semantic_index, element.system_value, element.component_type, element.reg};
         std::memcpy(data.data() + 8 + i * 24, values, sizeof(values));
         data[8 + i * 24 + 20] = element.mask;
         data[8 + i * 24 + 21] = element.rw_mask;
      }
      data.resize((data.size() + 3) & ~size_t(3), 0xAB); // fxc pads the names with 0xAB
      return data;
   }

   struct Instruction
   {
      size_t begin;
      size_t length;
      D3D10_SB_OPCODE_TYPE opcode;
   };

   // Splits the SHEX/SHDR tokens after the version and length into instructions (length from the opcode token, or the second token
   // for custom data). False if the lengths don't add up.
   inline bool SplitInstructions(const std::vector<uint32_t>& tokens, std::vector<Instruction>* instructions)
   {
      if (tokens.size() < 2 || tokens[1] != tokens.size())
         return false;
      size_t i = 2;
      while (i < tokens.size())
      {
         const D3D10_SB_OPCODE_TYPE opcode = DECODE_D3D10_SB_OPCODE_TYPE(tokens[i]);
         const size_t length = opcode == D3D10_SB_OPCODE_CUSTOMDATA ? (i + 1 < tokens.size() ? tokens[i + 1] : 0) : DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(tokens[i]);
         if (length == 0 || i + length > tokens.size())
            return false;
         instructions->push_back({i, length, opcode});
         i += length;
      }
      return true;
   }

   // WalkOperand's "first_index_position" when the first index has no immediate part (or the operand no index)
   constexpr size_t no_index = SIZE_MAX;

   // Calls "visit(operand_token_position, first_index_position)" on the operand at "i" and on its relative index operands (which
   // follow it). Returns the position after the operand, or 0 for unhandled encodings (64 bit immediates and indices).
   template <typename Visit>
   size_t WalkOperand(const std::vector<uint32_t>& tokens, size_t i, size_t end, Visit&& visit)
   {
      if (i >= end)
         return 0;
      const size_t token_position = i;
      const uint32_t token = tokens[i++];
      // Extended operand tokens (modifiers, min precision) chain via their top bit
      for (uint32_t extended = token; DECODE_IS_D3D10_SB_OPERAND_EXTENDED(extended); extended = tokens[i++])
      {
         if (i >= end)
            return 0;
      }
      const D3D10_SB_OPERAND_TYPE type = DECODE_D3D10_SB_OPERAND_TYPE(token);
      if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64)
         return 0;
      if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
      {
         i += DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
         return i <= end ? i : 0;
      }
      size_t first_index_position = no_index;
      const uint32_t dimensions = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
      for (uint32_t dimension = 0; dimension < dimensions; dimension++)
      {
         const D3D10_SB_OPERAND_INDEX_REPRESENTATION representation = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(dimension, token);
         if (representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE)
         {
            if (dimension == 0)
               first_index_position = i;
            i++;
         }
         if (representation == D3D10_SB_OPERAND_INDEX_RELATIVE || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE)
         {
            i = WalkOperand(tokens, i, end, visit);
            if (i == 0)
               return 0;
         }
         else if (representation != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
         {
            return 0;
         }
         if (i > end)
            return 0;
      }
      if (!visit(token_position, first_index_position))
         return 0;
      return i;
   }

   // Calls "visit" on every operand of a body instruction. False if its operands don't exactly fill it.
   template <typename Visit>
   bool WalkOperands(const std::vector<uint32_t>& tokens, const Instruction& instruction, Visit&& visit)
   {
      const size_t end = instruction.begin + instruction.length;
      size_t i = instruction.begin + 1;
      // Extended opcode tokens (sample offsets, resource dimension/return type)
      for (uint32_t extended = tokens[instruction.begin]; DECODE_IS_D3D10_SB_OPCODE_EXTENDED(extended); extended = tokens[i++])
      {
         if (i >= end)
            return false;
      }
      while (i < end)
      {
         i = WalkOperand(tokens, i, end, visit);
         if (i == 0)
            return false;
      }
      return i == end;
   }

   // Operand tokens for the instructions added here
   constexpr uint32_t RegisterOperand(D3D10_SB_OPERAND_TYPE type)
   {
      return ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) | ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
   }
   constexpr uint32_t Destination(D3D10_SB_OPERAND_TYPE type, uint32_t mask)
   {
      return RegisterOperand(type) | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(mask);
   }
   constexpr uint32_t Source(D3D10_SB_OPERAND_TYPE type, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
   {
      return RegisterOperand(type) | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(x, y, z, w);
   }
   constexpr uint32_t Opcode(D3D10_SB_OPCODE_TYPE type, uint32_t length, bool saturate = false)
   {
      return ENCODE_D3D10_SB_OPCODE_TYPE(type) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(length) | ENCODE_D3D10_SB_INSTRUCTION_SATURATE(saturate);
   }
   constexpr uint32_t mask_xy = D3D10_SB_OPERAND_4_COMPONENT_MASK_X | D3D10_SB_OPERAND_4_COMPONENT_MASK_Y;
   constexpr uint32_t mask_xyw = mask_xy | D3D10_SB_OPERAND_4_COMPONENT_MASK_W;

   // Copies the version, length and declaration tokens with "added_temps" more temps ("first_temp" gets the first; a temps
   // declaration is appended if none), inserting "after(index)"'s tokens after each declaration so new ones join their group, as
   // fxc orders them.
   template <typename After>
   std::vector<uint32_t> CopyDeclarations(const std::vector<uint32_t>& tokens, const std::vector<Instruction>& instructions, size_t first_body, uint32_t added_temps, uint32_t* first_temp, After&& after)
   {
      std::vector<uint32_t> declarations(tokens.begin(), tokens.begin() + 2);
      bool has_temps = false;
      *first_temp = 0;
      for (size_t i = 0; i < first_body; i++)
      {
         const Instruction& instruction = instructions[i];
         const size_t position = declarations.size();
         declarations.insert(declarations.end(), tokens.begin() + instruction.begin, tokens.begin() + instruction.begin + instruction.length);
         if (instruction.opcode == D3D10_SB_OPCODE_DCL_TEMPS)
         {
            *first_temp = declarations[position + 1];
            declarations[position + 1] += added_temps;
            has_temps = true;
         }
         const std::vector<uint32_t> added = after(i);
         declarations.insert(declarations.end(), added.begin(), added.end());
      }
      if (!has_temps)
         declarations.insert(declarations.end(), {ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(2), added_temps});
      return declarations;
   }

   // Index of the last declaration with one of the opcodes, or "first_body" if none
   inline size_t FindLastDeclaration(const std::vector<Instruction>& instructions, size_t first_body, std::initializer_list<D3D10_SB_OPCODE_TYPE> opcodes)
   {
      size_t last = first_body;
      for (size_t i = 0; i < first_body; i++)
      {
         if (std::ranges::find(opcodes, instructions[i].opcode) != opcodes.end())
            last = i;
      }
      return last;
   }

   // The program's tokens, split into instructions, and the index of the first one past the declarations; false on a broken length
   inline bool ReadProgram(const Chunk& program, std::vector<uint32_t>* tokens, std::vector<Instruction>* instructions, size_t* first_body)
   {
      if (program.data.size() % 4 != 0)
         return false;
      tokens->resize(program.data.size() / 4);
      std::memcpy(tokens->data(), program.data.data(), program.data.size());
      if (!SplitInstructions(*tokens, instructions))
         return false;
      *first_body = size_t(std::ranges::find_if(*instructions, [](const Instruction& instruction)
                              { return instruction.opcode != D3D10_SB_OPCODE_CUSTOMDATA && !ShaderPatching::opcodes_dcl.contains(instruction.opcode); }) -
                           instructions->begin());
      return true;
   }

   // Bytes of constant buffer "slot" the shader reads: up to its highest row read if it's only indexed by immediates, else its
   // declaration's size (in float4s, which covers indexed reads; also what some translators, e.g. dgVoodoo's, declare for every
   // buffer). 0 if it isn't declared (or the bytecode can't be read).
   inline uint32_t ConstantBufferBytes(const uint8_t* code, size_t size, uint32_t slot)
   {
      std::vector<Chunk> chunks;
      if (!ReadChunks(code, size, &chunks))
         return 0;
      const Chunk* const program = FindChunk(&chunks, FourCC("SHEX"), FourCC("SHDR"));
      std::vector<uint32_t> tokens;
      std::vector<Instruction> instructions;
      size_t first_body;
      if (!program || !ReadProgram(*program, &tokens, &instructions, &first_body))
         return 0;
      // Operand token, slot, size (SM5's 3D form, with the space, isn't used below SM5.1)
      uint32_t declared = 0;
      bool immediate_indexed = false;
      for (size_t i = 0; i < first_body; i++)
      {
         const Instruction& instruction = instructions[i];
         if (instruction.opcode == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER && instruction.length == 4 && tokens[instruction.begin + 2] == slot)
         {
            declared = tokens[instruction.begin + 3] * 16;
            immediate_indexed = DECODE_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(tokens[instruction.begin]) == D3D10_SB_CONSTANT_BUFFER_IMMEDIATE_INDEXED;
         }
      }
      if (declared == 0 || !immediate_indexed)
         return declared;
      uint32_t rows = 1;
      bool relative = false;
      for (size_t i = first_body; i < instructions.size() && !relative; i++)
      {
         const bool walked = WalkOperands(tokens, instructions[i], [&](size_t token_position, size_t index_position)
            {
               const uint32_t token = tokens[token_position];
               if (DECODE_D3D10_SB_OPERAND_TYPE(token) != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER || index_position == no_index || tokens[index_position] != slot)
                  return true;
               if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token) != D3D10_SB_OPERAND_INDEX_2D || DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, token) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                  relative = true;
               else
                  rows = (std::max)(rows, tokens[index_position + 1] + 1);
               return true; });
         relative |= !walked;
      }
      return relative ? declared : (std::min)(declared, rows * 16);
   }

   // Whether the shader reads row "row" of constant buffer "slot" by an immediate index (unlike "ConstantBufferBytes", it tells which
   // rows a shader declaring the whole buffer uses, e.g. a skinned vertex shader's world matrix past its relatively indexed bones).
   // "relative_base": also as the base of a relative index ("cb4[r0.x + row]", an array starting there, e.g. a bone palette).
   inline bool ReadsConstantRow(const uint8_t* code, size_t size, uint32_t slot, uint32_t row, bool relative_base = false)
   {
      std::vector<Chunk> chunks;
      if (!ReadChunks(code, size, &chunks))
         return false;
      const Chunk* const program = FindChunk(&chunks, FourCC("SHEX"), FourCC("SHDR"));
      std::vector<uint32_t> tokens;
      std::vector<Instruction> instructions;
      size_t first_body;
      if (!program || !ReadProgram(*program, &tokens, &instructions, &first_body))
         return false;
      bool reads = false;
      for (size_t i = first_body; i < instructions.size() && !reads; i++)
         WalkOperands(tokens, instructions[i], [&](size_t token_position, size_t index_position)
            {
               const uint32_t token = tokens[token_position];
               const auto representation = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, token);
               reads |= DECODE_D3D10_SB_OPERAND_TYPE(token) == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER && index_position != no_index && tokens[index_position] == slot &&
                        DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token) == D3D10_SB_OPERAND_INDEX_2D &&
                        (representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 || (relative_base && representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE)) &&
                        tokens[index_position + 1] == row;
               return true; });
      return reads;
   }

   // Whether the vertex shader's SV_Position depends on rows "first_row" to "last_row" of constant buffer "slot" and on no other row
   // of it (e.g. projects through the view projection alone, its vertices already in camera space). Data flow through temps,
   // each written component taking every source operand's dependencies (control flow ignored, so it errs on "other rows"). False
   // if the bytecode can't be read.
   inline bool PositionDependsOnlyOnConstantRows(const uint8_t* code, size_t size, uint32_t slot, uint32_t first_row, uint32_t last_row)
   {
      std::vector<Chunk> chunks;
      if (!ReadChunks(code, size, &chunks))
         return false;
      const Chunk* const program = FindChunk(&chunks, FourCC("SHEX"), FourCC("SHDR"));
      const Chunk* const output_signature = FindChunk(&chunks, FourCC("OSGN"));
      std::vector<SignatureElement> outputs;
      std::vector<uint32_t> tokens;
      std::vector<Instruction> instructions;
      size_t first_body;
      if (!program || !output_signature || !ReadSignature(output_signature->data, &outputs) || !ReadProgram(*program, &tokens, &instructions, &first_body))
         return false;
      const auto position = std::ranges::find_if(outputs, [](const SignatureElement& element)
         { return element.system_value == 1; }); // D3D_NAME_POSITION
      if (position == outputs.end())
         return false;

      // Per temp and output component: bit 0 the rows, bit 1 other rows of the buffer
      constexpr uint8_t in_rows = 1, other_rows = 2;
      std::unordered_map<uint64_t, std::array<uint8_t, 4>> dependencies;
      const auto key = [](D3D10_SB_OPERAND_TYPE type, uint32_t index)
      { return (uint64_t(type) << 32) | index; };
      for (size_t i = first_body; i < instructions.size(); i++)
      {
         uint8_t sources = 0;
         std::vector<std::pair<uint64_t, uint32_t>> destinations; // Register and its written components mask
         bool readable = WalkOperands(tokens, instructions[i], [&](size_t token_position, size_t index_position)
            {
               const uint32_t token = tokens[token_position];
               const D3D10_SB_OPERAND_TYPE type = DECODE_D3D10_SB_OPERAND_TYPE(token);
               if (type == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER)
               {
                  if (index_position != no_index && tokens[index_position] == slot)
                  {
                     const bool immediate_row = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token) == D3D10_SB_OPERAND_INDEX_2D && DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, token) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
                     const uint32_t row = tokens[index_position + 1];
                     sources |= (immediate_row && row >= first_row && row <= last_row) ? in_rows : other_rows;
                  }
                  return true;
               }
               // Not tracked (indexable temps): they could hold anything
               if (type == D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP)
               {
                  sources |= other_rows;
                  return true;
               }
               if (type != D3D10_SB_OPERAND_TYPE_TEMP && type != D3D10_SB_OPERAND_TYPE_OUTPUT)
                  return true;
               if (index_position == no_index)
                  return false;
               const uint64_t reg = key(type, tokens[index_position]);
               const auto mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token);
               if (mode == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE)
               {
                  destinations.emplace_back(reg, DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token) >> 4);
                  return true;
               }
               const auto found = dependencies.find(reg);
               if (found == dependencies.end())
                  return true;
               for (uint32_t component = 0; component < 4; component++)
               {
                  const uint32_t selected = (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE ? DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(token) : DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(token, component));
                  sources |= found->second[selected];
               }
               return true; });
         if (!readable)
            return false;
         for (const auto& [reg, mask] : destinations)
         {
            auto& components = dependencies[reg];
            for (uint32_t component = 0; component < 4; component++)
            {
               if (mask & (1u << component))
                  components[component] = sources;
            }
         }
      }
      const auto found = dependencies.find(key(D3D10_SB_OPERAND_TYPE_OUTPUT, position->reg));
      if (found == dependencies.end())
         return false;
      uint8_t position_dependencies = 0;
      for (const uint8_t component : found->second)
         position_dependencies |= component;
      return position_dependencies == in_rows;
   }

   // The patched tokens as the program, with their length token set
   inline void WriteProgram(std::vector<uint32_t>* tokens, Chunk* program)
   {
      (*tokens)[1] = uint32_t(tokens->size());
      program->data.resize(tokens->size() * 4);
      std::memcpy(program->data.data(), tokens->data(), program->data.size());
   }

   // The clamp a pixel shader's UNORM render target 0 applied, put back for when that target is upgraded to float. SATURATE:
   // saturate(o0). UNORM8 also rounds to the 8-bit target's 1/255 steps: consumers that decode around 128/255 (an encoded 0.5 stores
   // as that) read a written 0.5 as a small offset otherwise. BLEND_INPUT clamps only what blending needs, rgb >= 0 and alpha in
   // [0, 1] (an alpha above 1 makes INV_SRC_ALPHA negative), and keeps colour above 1 for HDR. FLOOR only clamps negatives, alpha
   // included, for passes over an HDR scene whose upper clamp would clip it.
   enum class Output0Clamp : uint8_t
   {
      SATURATE,
      UNORM8,
      BLEND_INPUT,
      FLOOR,
   };

   // For "Game::PatchShaderBytecodeSync" ("code": the program after its version and length tokens, "size" its size in and out).
   // o0's writes go to an added temp, copied to o0 clamped before the final ret: reading an output ("mov_sat o0, o0") isn't something
   // fxc emits, and WARP's shader JIT crashes on it. "components" are the target's signature mask (x = 1). Null for a program with
   // another return, a relatively indexed output or nothing to clamp.
   inline std::unique_ptr<std::byte[]> ClampOutput0(const std::byte* code, size_t* size, Output0Clamp clamp = Output0Clamp::SATURATE, uint32_t components = 0xF)
   {
      if (*size % sizeof(uint32_t) != 0)
         return nullptr;
      Chunk program{.fourcc = FourCC("SHEX"), .data = std::vector<uint8_t>(8 + *size)};
      const uint32_t header[2] = {0, uint32_t(program.data.size() / sizeof(uint32_t))};
      std::memcpy(program.data.data(), header, sizeof(header));
      std::memcpy(program.data.data() + sizeof(header), code, *size);
      std::vector<uint32_t> tokens;
      std::vector<Instruction> instructions;
      size_t first_body = 0;
      if (!ReadProgram(program, &tokens, &instructions, &first_body) || first_body >= instructions.size() || instructions.back().opcode != D3D10_SB_OPCODE_RET ||
          std::ranges::any_of(instructions.begin() + first_body, instructions.end() - 1, [](const Instruction& instruction)
             { return instruction.opcode == D3D10_SB_OPCODE_RET || instruction.opcode == D3D10_SB_OPCODE_RETC; }))
         return nullptr;
      // BLEND_INPUT: "mask" (rgb) gets the max, alpha the saturate
      const uint32_t mask = (clamp == Output0Clamp::BLEND_INPUT ? (components & 0x7) : (components & 0xF));
      const bool alpha = clamp == Output0Clamp::BLEND_INPUT && (components & 0x8) != 0;
      if (mask == 0 && !alpha)
         return nullptr;

      uint32_t color;
      std::vector<uint32_t> patched = CopyDeclarations(tokens, instructions, first_body, 1, &color, [](size_t)
         { return std::vector<uint32_t>(); });
      for (size_t i = first_body; i + 1 < instructions.size(); i++)
      {
         const Instruction& instruction = instructions[i];
         const size_t offset = patched.size();
         patched.insert(patched.end(), tokens.begin() + instruction.begin, tokens.begin() + instruction.begin + instruction.length);
         bool supported = true;
         const bool walked = WalkOperands(tokens, instruction, [&](size_t token_position, size_t index_position)
            {
               uint32_t& token = patched[offset + token_position - instruction.begin];
               if (DECODE_D3D10_SB_OPERAND_TYPE(token) != D3D10_SB_OPERAND_TYPE_OUTPUT)
                  return true;
               if (index_position == no_index || DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, token) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                  return supported = false;
               if (tokens[index_position] == 0)
               {
                  token = (token & ~D3D10_SB_OPERAND_TYPE_MASK) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_TEMP);
                  patched[offset + index_position - instruction.begin] = color;
               }
               return true; });
         if (!walked || !supported)
            return nullptr;
      }

      // Signature masks are plain component bits (x = 1), the operand token masks the same shifted
      const auto output = [](uint32_t mask)
      { return Destination(D3D10_SB_OPERAND_TYPE_OUTPUT, mask << 4); };
      constexpr uint32_t color_source = Source(D3D10_SB_OPERAND_TYPE_TEMP, 0, 1, 2, 3);
      constexpr uint32_t immediate = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32);
      switch (clamp)
      {
      case Output0Clamp::SATURATE:
         patched.insert(patched.end(), {Opcode(D3D10_SB_OPCODE_MOV, 5, true), output(mask), 0, color_source, color});
         break;
      case Output0Clamp::UNORM8:
      {
         // mov_sat | mad 255, 0.5 | round_ni | mul 1 / 255: UNORM's round to nearest
         constexpr uint32_t all = Destination(D3D10_SB_OPERAND_TYPE_TEMP, D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL);
         constexpr uint32_t scale = std::bit_cast<uint32_t>(255.f);
         constexpr uint32_t half = std::bit_cast<uint32_t>(0.5f);
         constexpr uint32_t inverse_scale = std::bit_cast<uint32_t>(1.f / 255.f);
         patched.insert(patched.end(), {Opcode(D3D10_SB_OPCODE_MOV, 5, true), all, color, color_source, color,
                                          Opcode(D3D10_SB_OPCODE_MAD, 15), all, color, color_source, color, immediate, scale, scale, scale, scale, immediate, half, half, half, half,
                                          Opcode(D3D10_SB_OPCODE_ROUND_NI, 5), all, color, color_source, color,
                                          Opcode(D3D10_SB_OPCODE_MUL, 10), output(mask), 0, color_source, color, immediate, inverse_scale, inverse_scale, inverse_scale, inverse_scale});
         break;
      }
      case Output0Clamp::FLOOR:
         patched.insert(patched.end(), {Opcode(D3D10_SB_OPCODE_MAX, 10), output(mask), 0, color_source, color, immediate, 0, 0, 0, 0});
         break;
      case Output0Clamp::BLEND_INPUT:
         if (mask != 0)
         {
            patched.insert(patched.end(), {Opcode(D3D10_SB_OPCODE_MAX, 10), output(mask), 0, color_source, color, immediate, 0, 0, 0, 0});
         }
         if (alpha)
         {
            patched.insert(patched.end(), {Opcode(D3D10_SB_OPCODE_MOV, 5, true), output(0x8), 0, color_source, color});
         }
         break;
      }
      patched.insert(patched.end(), tokens.begin() + instructions.back().begin, tokens.end());

      *size = (patched.size() - 2) * sizeof(uint32_t);
      auto new_code = std::make_unique<std::byte[]>(*size);
      std::memcpy(new_code.get(), patched.data() + 2, *size);
      return new_code;
   }

   // For "Game::PatchShaderBytecodeSync" ("code": the program after its version and length tokens, "size" its size in and out).
   // Every immediately indexed read of row "from_row" of constant buffer "from_slot" reads row "to_row" of "to_slot" instead, a buffer
   // the game mod binds (declared here, immediately indexed): a value the game computes on the CPU, swapped for the mod's. Null if the
   // shader doesn't read that row, may read it through a relative index, or already declares "to_slot".
   inline std::unique_ptr<std::byte[]> RedirectConstantRow(const std::byte* code, size_t* size, uint32_t from_slot, uint32_t from_row, uint32_t to_slot, uint32_t to_row)
   {
      if (*size % sizeof(uint32_t) != 0)
         return nullptr;
      Chunk program{.fourcc = FourCC("SHEX"), .data = std::vector<uint8_t>(8 + *size)};
      const uint32_t header[2] = {0, uint32_t(program.data.size() / sizeof(uint32_t))};
      std::memcpy(program.data.data(), header, sizeof(header));
      std::memcpy(program.data.data() + sizeof(header), code, *size);
      std::vector<uint32_t> tokens;
      std::vector<Instruction> instructions;
      size_t first_body = 0;
      if (!ReadProgram(program, &tokens, &instructions, &first_body))
         return nullptr;
      // The new declaration joins the source's (operand token, slot, size: SM5's 3D form isn't used below SM5.1)
      const auto constant_buffer_slot = [&](const Instruction& instruction)
      {
         return (instruction.opcode == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER && instruction.length == 4) ? tokens[instruction.begin + 2] : UINT32_MAX;
      };
      size_t source_declaration = first_body;
      for (size_t i = 0; i < first_body; i++)
      {
         const uint32_t slot = constant_buffer_slot(instructions[i]);
         if (slot == to_slot)
            return nullptr;
         if (slot == from_slot)
            source_declaration = i;
      }
      if (source_declaration == first_body)
         return nullptr;

      uint32_t unused_temp;
      std::vector<uint32_t> patched = CopyDeclarations(tokens, instructions, first_body, 0, &unused_temp, [&](size_t i)
         {
            if (i != source_declaration)
               return std::vector<uint32_t>();
            const Instruction& declaration = instructions[i];
            const uint32_t opcode = (tokens[declaration.begin] & ~D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN_MASK) | ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_IMMEDIATE_INDEXED);
            return std::vector<uint32_t>{opcode, tokens[declaration.begin + 1], to_slot, to_row + 1}; });
      bool redirected = false;
      bool supported = true;
      for (size_t i = first_body; i < instructions.size(); i++)
      {
         const Instruction& instruction = instructions[i];
         const size_t offset = patched.size();
         patched.insert(patched.end(), tokens.begin() + instruction.begin, tokens.begin() + instruction.begin + instruction.length);
         if (instruction.opcode == D3D10_SB_OPCODE_CUSTOMDATA)
            continue;
         const bool walked = WalkOperands(tokens, instruction, [&](size_t token_position, size_t index_position)
            {
               const uint32_t token = tokens[token_position];
               if (DECODE_D3D10_SB_OPERAND_TYPE(token) != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER || index_position == no_index || tokens[index_position] != from_slot)
                  return true;
               if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token) != D3D10_SB_OPERAND_INDEX_2D || DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, token) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                  return supported = false;
               if (tokens[index_position + 1] == from_row)
               {
                  patched[offset + index_position - instruction.begin] = to_slot;
                  patched[offset + index_position + 1 - instruction.begin] = to_row;
                  redirected = true;
               }
               return true; });
         if (!walked || !supported)
            return nullptr;
      }
      if (!redirected)
         return nullptr;

      *size = (patched.size() - 2) * sizeof(uint32_t);
      auto new_code = std::make_unique<std::byte[]>(*size);
      std::memcpy(new_code.get(), patched.data() + 2, *size);
      return new_code;
   }
} // namespace DXBC
