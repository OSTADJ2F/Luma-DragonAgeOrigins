// Crash reports without a debugger for the test programs: the faulting address and the code addresses on the
// faulting thread's stack as module+offset, for llvm-symbolizer.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace
{
   // "module+offset" for an address, for crash reports without a debugger.
   void PrintAddress(const char* label, void* address)
   {
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             (LPCSTR)address, &module))
      {
         GetModuleFileNameA(module, name, MAX_PATH);
      }
      const char* file = strrchr(name, '\\');
      printf("%s %s+0x%IX\n", label, file ? file + 1 : name, (uintptr_t)address - (uintptr_t)module);
   }

   // Prints the faulting address and the code addresses found on the faulting thread's stack, then lets it crash.
   LONG WINAPI CrashReport(EXCEPTION_POINTERS* info)
   {
      printf("CRASH 0x%08lX thread %lu\n", info->ExceptionRecord->ExceptionCode, GetCurrentThreadId());
      PrintAddress("  at", info->ExceptionRecord->ExceptionAddress);
      if (info->ExceptionRecord->NumberParameters >= 2)
      {
         printf("  access %s 0x%IX\n", info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            info->ExceptionRecord->ExceptionInformation[1]);
      }
      auto* stack = (void**)info->ContextRecord->Esp;
      int found = 0;
      for (int i = 0; i < 2048 && found < 24; i++)
      {
         MEMORY_BASIC_INFORMATION mbi;
         if (!VirtualQuery(stack[i], &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            continue;
         PrintAddress("  stack", stack[i]);
         found++;
      }
      fflush(stdout);
      return EXCEPTION_CONTINUE_SEARCH;
   }
} // namespace
