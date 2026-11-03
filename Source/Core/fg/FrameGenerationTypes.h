#pragma once

#include <cstdint>

// What the D3D11 side (Core's frame generation glue, "sr_bridge/D3D12SRLocal.h") and the D3D12 generators ("FrameGeneration.h") share.
// No D3D12 types, so a D3D11 translation unit can include it.
namespace FrameGeneration
{
   enum Kind
   {
      kFsr,  // FidelityFX API frame generation (FSR 4 on RDNA4, else 3.1)
      kDlss, // DLSS-G through Streamline
      kXess, // XeSS FG (XeFG) with XeLL
   };

   // Two of each, used in turns
   enum Slot
   {
      kColor,   // The frame as presented (after the UI and ReShade)
      kHudless, // The same frame without the UI
      kSlotCount
   };
   constexpr int kSlots = 2;

   enum LatencyMarker
   {
      kSimulationStart,
      kSimulationEnd,
      kRenderSubmitStart,
      kRenderSubmitEnd,
   };

   // Values of the "consumed" fence: the game's GPU may reuse a present's slot and the super resolution inputs once it reaches that present
   constexpr uint64_t kReady = 1;                  // The generator exists and presents (the first present is 2)
   constexpr uint64_t kFailed = UINT64_MAX - 1;    // It couldn't start
} // namespace FrameGeneration
