// Dragon Age Origins: the SR helper shim.
//
// Core's SR bridge runs the 64-bit helper as a dll through Windows' own rundll32, and for FSR Core's "FSR::Init" opens
// "amd_fidelityfx_loader_dx12.dll" by name with LOAD_LIBRARY_SEARCH_APPLICATION_DIR. That flag means the folder of the process that
// hosts the helper, which is rundll32 in System32 - not the game's folder - so the game's copy of the loader was never found and FSR
// failed to start ("FSR: amd_fidelityfx_loader_dx12.dll not found next to the game's executable", "FAIL FSR init or create"), leaving
// the frame with no upscaler at all. The clean fix belongs in the framework (the helper should anchor that load to its own folder), and
// this mod does not edit framework code. So this mod-owned dll takes the name the bridge looks for ("Luma-Upscaler.dll"), loads what
// the framework helper opens by name from the game's own folder before it runs, and forwards the entry point to that helper, unchanged
// and shipped beside it as "Luma-Upscaler-Core.dll". A module that is already loaded is matched by its name before any search, so
// Core's by-name opens return what was loaded here.
//
// It also does what a mod can only do from inside this process:
//   - "Luma-Upscaler.ini" (next to the game's exe, optional) points "VariantFolder" at a folder whose copies of "nvngx_dlss.dll",
//     "amd_fidelityfx_loader_dx12.dll" or "amd_fidelityfx_upscaler_dx12.dll" take precedence. That is how a DLSS or FSR version is
//     A/B tested without rebuilding anything: an FSR container that only carries 3.1.x makes Core's own default pick 3.1.5, so no
//     version override is needed anywhere.
//   - Every helper start logs what it resolved (versions and the pair's hashes) and names the missing files that keep an upscaler from
//     starting, instead of Core only greying the entry out.
//   - It refuses loudly when the helper pair doesn't match the hashes the package wrote into "Luma-Upscaler-Files.txt", which is what
//     happens when someone replaces only one of the two files.
// Everything else (DLSS, FSR, the whole bridge protocol) is the framework helper's own work: this file loads, reports and forwards.
#include <windows.h>
#include <winver.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
   using HelperEntry = void (CALLBACK*)(HWND, HINSTANCE, LPSTR, int);

   // The framework's helper, shipped next to this dll (renamed: Core only looks for the name this dll has)
   constexpr const wchar_t* kSelfName = L"Luma-Upscaler.dll";
   constexpr const wchar_t* kCoreHelperName = L"Luma-Upscaler-Core.dll";
   // What the game folder must carry: FSR's loader (what Core opens by name, and what FSR 4 needs) and the container it loads, plus
   // NVIDIA's dll for DLSS
   constexpr const wchar_t* kFsrLoaderName = L"amd_fidelityfx_loader_dx12.dll";
   constexpr const wchar_t* kFsrUpscalerName = L"amd_fidelityfx_upscaler_dx12.dll";
   constexpr const wchar_t* kDlssName = L"nvngx_dlss.dll";
   // Written by "Data/Dragon Age Origins/package-release.ps1": the FNV-1a 64 of the pair it packaged
   constexpr const wchar_t* kManifestName = L"Luma-Upscaler-Files.txt";
   // The mod's own settings, next to the game's exe (no file means the defaults)
   constexpr const wchar_t* kIniName = L"Luma-Upscaler.ini";
   constexpr const wchar_t* kIniSection = L"LumaUpscaler";

   void Log(const char* const format, ...)
   {
      va_list arguments;
      va_start(arguments, format);
      printf("[Luma Upscaler] ");
      vprintf(format, arguments);
      printf("\n");
      fflush(stdout);
      va_end(arguments);
   }

   bool IsFile(const std::wstring& path)
   {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
   }

   bool IsFolder(const std::wstring& path)
   {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
   }

   // The folder this module lives in: the game's, unlike the host process's (rundll32's) folder
   std::wstring ThisFolder()
   {
      HMODULE self = nullptr;
      wchar_t path[MAX_PATH] = {};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ThisFolder), &self) ||
         GetModuleFileNameW(self, path, MAX_PATH) == 0)
      {
         return {};
      }
      std::wstring folder(path);
      const size_t separator = folder.rfind(L'\\');
      return separator == std::wstring::npos ? std::wstring() : folder.substr(0, separator + 1);
   }

   std::wstring IniValue(const std::wstring& ini, const wchar_t* const key)
   {
      wchar_t value[MAX_PATH * 2] = {};
      GetPrivateProfileStringW(kIniSection, key, L"", value, DWORD(std::size(value)), ini.c_str());
      std::wstring text = value;
      // Hand edits: a trailing backslash, spaces, or quotes
      while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\\'))
      {
         text.pop_back();
      }
      if (text.size() >= 2 && ((text.front() == L'"' && text.back() == L'"') || (text.front() == L'\'' && text.back() == L'\'')))
      {
         text = text.substr(1, text.size() - 2);
      }
      return text;
   }

   // FNV-1a 64: a mismatch detector, not a security measure (the package script writes the same number)
   uint64_t HashFile(const std::wstring& path)
   {
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
      {
         return 0;
      }
      uint64_t hash = 14695981039346656037ull;
      std::vector<BYTE> buffer(64 * 1024);
      DWORD read = 0;
      while (ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read > 0)
      {
         for (DWORD i = 0; i < read; i++)
         {
            hash ^= buffer[i];
            hash *= 1099511628211ull;
         }
      }
      CloseHandle(file);
      return hash;
   }

   std::string HashText(const uint64_t hash)
   {
      char text[32] = {};
      sprintf_s(text, "%016llx", static_cast<unsigned long long>(hash));
      return text;
   }

   // The dll's own file version ("310.9.16.0"): what tells two DLSS or FSR builds apart
   std::string FileVersion(const std::wstring& path)
   {
      DWORD unused = 0;
      const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
      if (size == 0)
      {
         return {};
      }
      std::vector<BYTE> data(size);
      if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
      {
         return {};
      }
      VS_FIXEDFILEINFO* info = nullptr;
      UINT length = 0;
      if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr)
      {
         return {};
      }
      char text[48] = {};
      sprintf_s(text, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
      const std::string version(text);
      return version == "0.0.0.0" ? std::string() : version;
   }

   // "built 2026-11-03 15:06": for a dll without a version resource (this shim has none), so the pair is still comparable
   std::string FileTimeText(const std::wstring& path)
   {
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
      {
         return {};
      }
      FILETIME write = {};
      const bool read = GetFileTime(file, nullptr, nullptr, &write) != FALSE;
      CloseHandle(file);
      if (!read)
      {
         return {};
      }
      SYSTEMTIME local = {};
      FILETIME utc = {};
      if (!FileTimeToLocalFileTime(&write, &utc) || !FileTimeToSystemTime(&utc, &local))
      {
         return {};
      }
      char text[32] = {};
      sprintf_s(text, "built %04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
      return text;
   }

   // "<version, or when it was built>[ <fnv1a64>]"; the hash only for the small files this dll itself compares
   std::string FileText(const std::wstring& path, const bool hash)
   {
      if (!IsFile(path))
      {
         return "missing";
      }
      const std::string version = FileVersion(path);
      std::string text = version.empty() ? FileTimeText(path) : version;
      if (text.empty())
      {
         text = "unknown";
      }
      if (hash)
      {
         text += " " + HashText(HashFile(path));
      }
      return text;
   }

   // "Luma-Upscaler-Files.txt": one "<file name> <fnv1a64 hex>" per line, "#" starts a comment. Empty when the file isn't there (a dev
   // build has none, and the check is skipped).
   std::string ManifestHash(const std::wstring& manifest, const std::wstring& name)
   {
      const HANDLE file = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
      {
         return {};
      }
      std::string text;
      char buffer[4096];
      DWORD read = 0;
      while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
      {
         text.append(buffer, read);
      }
      CloseHandle(file);

      std::string wanted;
      for (const wchar_t* c = name.c_str(); *c; c++)
      {
         wanted.push_back(char(*c));
      }
      size_t position = 0;
      while (position < text.size())
      {
         size_t end = text.find('\n', position);
         if (end == std::string::npos)
         {
            end = text.size();
         }
         std::string line = text.substr(position, end - position);
         position = end + 1;
         while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
         {
            line.pop_back();
         }
         if (line.empty() || line.front() == '#')
         {
            continue;
         }
         const size_t separator = line.find_first_of(" \t");
         if (separator == std::string::npos)
         {
            continue;
         }
         const std::string listed_name = line.substr(0, separator);
         const size_t hash_start = line.find_first_not_of(" \t", separator + 1);
         if (listed_name == wanted && hash_start != std::string::npos)
         {
            return line.substr(hash_start);
         }
      }
      return {};
   }

   // A full path, so its own folder is searched for what it depends on (never the host's folder, never the current directory)
   HMODULE LoadByPath(const std::wstring& path)
   {
      return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
   }
} // namespace

extern "C" __declspec(dllexport) void CALLBACK LumaUpscaler(HWND window, HINSTANCE instance, LPSTR command, int show)
{
   const std::wstring folder = ThisFolder();
   if (folder.empty())
   {
      Log("FAIL the SR helper shim can't find its own folder");
      return;
   }

   const std::wstring ini = folder + kIniName;
   const std::wstring variant_value = IniValue(ini, L"VariantFolder");
   const std::wstring variant = variant_value.find(L':') != std::wstring::npos ? variant_value : folder + variant_value;
   const bool variant_used = !variant_value.empty() && IsFolder(variant);
   if (!variant_value.empty() && !variant_used)
   {
      Log("WARN VariantFolder \"%ls\" (%ls) is not a folder: using the game's own dlls", variant_value.c_str(), kIniName);
   }
   const bool allow_mismatch = IniValue(ini, L"AllowMismatch") == L"1";

   // Per file: a copy in the variant folder wins, else the game's own (next to this dll)
   const auto Resolve = [&](const wchar_t* const name, bool& from_variant)
   {
      if (variant_used)
      {
         const std::wstring candidate = variant + L"\\" + name;
         if (IsFile(candidate))
         {
            from_variant = true;
            return candidate;
         }
      }
      from_variant = false;
      return folder + name;
   };
   bool loader_variant = false;
   bool upscaler_variant = false;
   bool dlss_variant = false;
   const std::wstring loader_path = Resolve(kFsrLoaderName, loader_variant);
   const std::wstring upscaler_path = Resolve(kFsrUpscalerName, upscaler_variant);
   const std::wstring dlss_path = Resolve(kDlssName, dlss_variant);
   const std::wstring core_path = folder + kCoreHelperName;

   // 2. The environment, on every start: what is here, which version, which file
   Log("SR environment: shim %s | core %s", FileText(folder + kSelfName, true).c_str(), FileText(core_path, true).c_str());
   Log("SR files: DLSS %ls %s%s | FSR %ls %s%s", kDlssName, FileText(dlss_path, false).c_str(), dlss_variant ? " (variant)" : "", kFsrLoaderName,
       FileText(loader_path, false).c_str(), loader_variant ? " (variant)" : "");
   Log("SR files: FSR upscaler %s%s | %s", FileText(upscaler_path, false).c_str(), upscaler_variant ? " (variant)" : "",
       variant_used ? "variant folder in use" : "no variant folder");

   // What keeps an upscaler from starting, named, instead of Core only greying the entry out
   if (!IsFile(dlss_path))
   {
      Log("DLSS will not start: %ls is not in %ls", kDlssName, folder.c_str());
   }
   if (!IsFile(loader_path))
   {
      Log("FSR will not start: %ls is not in %ls", kFsrLoaderName, folder.c_str());
   }
   else if (!IsFile(upscaler_path))
   {
      Log("FSR will not start: %ls is not in %ls", kFsrUpscalerName, folder.c_str());
   }
   if (IsFile(folder + L"Luma-Upscaler.exe"))
   {
      Log("WARN Luma-Upscaler.exe is next to the game and is ignored: the dll is what runs (delete the exe, it is from an older package)");
   }

   // 5. The pair: the package writes both hashes, so replacing only one of the two files is caught here, by name
   const std::string expected = ManifestHash(folder + kManifestName, kCoreHelperName);
   if (expected.empty())
   {
      Log("SR pair: no %ls here, not checked (the package writes one)", kManifestName);
   }
   else if (const std::string actual = HashText(HashFile(core_path)); actual != expected)
   {
      if (!allow_mismatch)
      {
         Log("FAIL %ls is %s, the package expected %s: only one of the pair was replaced", kCoreHelperName, actual.c_str(), expected.c_str());
         Log("FAIL copy Luma-Upscaler.dll and Luma-Upscaler-Core.dll from the same package, or set AllowMismatch=1 in %ls", kIniName);
         return;
      }
      Log("WARN %ls is %s, the package expected %s (AllowMismatch=1, continuing)", kCoreHelperName, actual.c_str(), expected.c_str());
   }
   else
   {
      Log("SR pair: matches %ls", kManifestName);
   }

   // 1. Preload what is opened by name, from the resolved paths. The loader always: Core opens it by name and would look in rundll32's
   // folder instead, which is the bug this dll exists for. The other two only when a variant folder asks for them, so a stock install
   // pays nothing for 28 MB of FidelityFX or a DLSS dll it doesn't override (the loader and NGX find the game's own copies by themselves).
   if (IsFile(loader_path) && !LoadByPath(loader_path))
   {
      Log("FAIL %ls couldn't be loaded (error %lu)", kFsrLoaderName, GetLastError());
   }
   if (upscaler_variant && !LoadByPath(upscaler_path))
   {
      Log("FAIL the FSR variant %ls couldn't be loaded (error %lu)", upscaler_path.c_str(), GetLastError());
   }
   if (dlss_variant && !LoadByPath(dlss_path))
   {
      Log("FAIL the DLSS variant %ls couldn't be loaded (error %lu)", dlss_path.c_str(), GetLastError());
   }

   const HMODULE core = LoadByPath(core_path);
   if (!core)
   {
      Log("FAIL %ls not found next to the game's exe (error %lu): the package must ship the framework helper renamed", kCoreHelperName, GetLastError());
      return;
   }
   const auto entry = reinterpret_cast<HelperEntry>(GetProcAddress(core, "LumaUpscaler"));
   if (!entry)
   {
      Log("FAIL %ls has no LumaUpscaler entry point", kCoreHelperName);
      return;
   }
   entry(window, instance, command, show);
}
