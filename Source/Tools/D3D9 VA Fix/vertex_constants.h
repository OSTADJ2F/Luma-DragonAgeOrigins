#pragma once

#include <cstdint>

// D3D9 vertex shader constants as the D3D9 VA Fix proxy last passed them to the layer below, in dgVoodoo's vc4 layout (the cb4
// of its translated vertex shaders, 2.87.3): i# at rows 0-15, b# at rows 16-19 (component # % 4), c# at rows
// 20-275. Updated on the thread that calls the layer below (the CSMT worker) right after each call, so a D3D11 hook on that
// thread sees the constants dgVoodoo uploads for the draw it is translating; Luma reads them instead of copying every vc4
// upload back (Borderlands 2 motion vectors). Exported by the proxy's d3d9.dll as "LumaGetVertexConstants".
// An ABI between two DLLs built apart: append members only, and bump "VERSION" when a member's meaning changes.
struct VertexConstantMirror
{
   static constexpr uint32_t VERSION = 1;
   static constexpr uint32_t INT_ROW = 0;
   static constexpr uint32_t BOOL_ROW = 16;
   static constexpr uint32_t FLOAT_ROW = 20;
   static constexpr uint32_t ROWS = 276;

   uint32_t version = VERSION;
   // Kept only while the CSMT layer runs: it owns the calls the rows come from. Changes at device creation and destruction.
   uint32_t active = 0;
   // Rows not set since the layer started, a Reset or a state block's Apply: zero here, whatever the layer below has
   uint32_t unknown_rows = ROWS;
   uint32_t generation = 0; // Changes with every update
   uint32_t rows[ROWS][4] = {};
   uint8_t known[ROWS] = {};
};

using GetVertexConstantMirrorFn = const VertexConstantMirror*(__stdcall*)();
inline constexpr char kGetVertexConstantMirrorExport[] = "LumaGetVertexConstants";
