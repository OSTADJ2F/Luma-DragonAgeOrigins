#pragma once

// MinHook keeps one state per DLL, shared by Core (Reflex's sleep hook) and the game mod (its own engine hooks), compiled once
// through "Luma.Features.props" ("ENABLE_MINHOOK"). So each owner initializes it with "InitializeMinHook()" and only removes its
// own hooks ("MH_RemoveHook()"/"MH_DisableHook(target)"): "MH_ALL_HOOKS" and "MH_Uninitialize()" would also remove the other
// owner's: this header removes the first and deprecates the second (C4995). Core uninitializes it when the addon unloads.
#include <array>
#include <cstdint>
#include <cstring>

#include <Windows.h>

#include "../../External/reshade/deps/minhook/include/MinHook.h"

inline bool InitializeMinHook()
{
   const MH_STATUS status = MH_Initialize();
   return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}

inline void UninitializeMinHook()
{
   MH_Uninitialize();
}

namespace MinHook
{
   // A function hooked with MinHook. The owner locks around "Attach()" and "Detach()", the detour reads the rest without it as it only runs between them.
   template <typename Function>
   struct HookedFunction
   {
      void* target = nullptr;      // The function the hook went on, null if there is none
      Function original = nullptr; // The trampoline: the target as it was before the hook
      // The target's start right after our hook went in: different later means another module hooked over it
      std::array<uint8_t, 8> hooked_prologue = {};
   };

   // False if MinHook failed
   template <typename Function>
   bool Attach(HookedFunction<Function>* hooked, void* target, Function detour)
   {
      if (!InitializeMinHook() || MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&hooked->original)) != MH_OK)
      {
         return false;
      }
      if (MH_EnableHook(target) != MH_OK)
      {
         MH_RemoveHook(target);
         return false;
      }
      std::memcpy(hooked->hooked_prologue.data(), target, hooked->hooked_prologue.size());
      hooked->target = target;
      return true;
   }

   // Only Luma's hook (see above). False if another module hooked over it: Luma's then stays, as the other hook's trampoline jumps to our
   // detour, and the addon is pinned to keep that code loaded (too late if "DllMain" is unloading it). ponytail: a pinned addon isn't
   // initialized again if ReShade reloads it in the same process.
   template <typename Function>
   bool Detach(HookedFunction<Function>* hooked)
   {
      if (!hooked->target)
      {
         return true;
      }
      if (std::memcmp(hooked->target, hooked->hooked_prologue.data(), hooked->hooked_prologue.size()) != 0)
      {
         HMODULE module = nullptr;
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&InitializeMinHook), &module);
         return false;
      }
      MH_RemoveHook(hooked->target);
      hooked->target = nullptr;
      return true;
   }
} // namespace MinHook

#undef MH_ALL_HOOKS
#pragma deprecated(MH_Uninitialize)
