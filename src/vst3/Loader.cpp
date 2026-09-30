// This module deliberately has no Qt or SDK binary dependency.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
HMODULE selfModule = nullptr, engine = nullptr;
unsigned references = 0;
std::recursive_mutex mutex;
using Switch = bool (*)();
using Factory = void *(*)();
Switch engineExit = nullptr, canUnload = nullptr;
Factory factory = nullptr;
std::wstring lastError;
void fail(const std::wstring &message) {
  lastError = L"AirPlayQt loader: " + message;
  OutputDebugStringW((lastError + L"\n").c_str());
}
std::filesystem::path modulePath(HMODULE module) {
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!size || size == path.size())
    throw std::runtime_error("GetModuleFileNameW failed");
  path.resize(size);
  return path;
}
std::string hashFile(const std::filesystem::path &path) {
  struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    ~Hash() {
      if (value)
        BCryptDestroyHash(value);
      if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
  } hash;
  if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) <
          0)
    throw std::runtime_error("SHA256 initialization failed");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Cannot open runtime file");
  std::array<char, 65536> buffer;
  while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
    if (BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
                       ULONG(file.gcount()), 0) < 0)
      throw std::runtime_error("SHA256 read failed");
  }
  if (!file.eof())
    throw std::runtime_error("Runtime file read failed");
  std::array<unsigned char, 32> digest{};
  if (BCryptFinishHash(hash.value, digest.data(), ULONG(digest.size()), 0) < 0)
    throw std::runtime_error("SHA256 finalization failed");
  std::string result;
  for (auto byte : digest) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
bool systemRuntime(const std::wstring &name) {
  return _wcsnicmp(name.c_str(), L"msvcp", 5) == 0 ||
         _wcsnicmp(name.c_str(), L"vcruntime", 9) == 0 ||
         _wcsicmp(name.c_str(), L"ucrtbase.dll") == 0;
}
bool verify(const std::filesystem::path &directory) {
  std::ifstream manifest(directory / L"runtime-sha256.txt");
  if (!manifest) {
    fail(L"Missing runtime-sha256.txt");
    return false;
  }
  std::vector<HMODULE> modules(256);
  DWORD needed = 0;
  for (;;) {
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                            DWORD(modules.size() * sizeof(HMODULE)), &needed))
      throw std::runtime_error("EnumProcessModules failed");
    if (needed <= modules.size() * sizeof(HMODULE))
      break;
    modules.resize(needed / sizeof(HMODULE) + 32);
  }
  modules.resize(needed / sizeof(HMODULE));
  std::string line;
  bool engineFound = false, platformFound = false;
  while (std::getline(manifest, line)) {
    if (line.size() < 67 || line.substr(64, 2) != "  ")
      throw std::runtime_error("Invalid runtime manifest");
    const auto name = line.substr(66);
    if (name.find("..") != std::string::npos ||
        name.find(':') != std::string::npos ||
        name.find('\\') != std::string::npos || name.front() == '/')
      throw std::runtime_error("Invalid runtime manifest path");
    const auto path = directory / std::filesystem::path(name);
    if (!std::filesystem::is_regular_file(path)) {
      fail(L"Missing runtime file: " + path.wstring());
      return false;
    }
    const auto expected = line.substr(0, 64);
    if (hashFile(path) != expected) {
      fail(L"Runtime checksum mismatch: " + path.wstring());
      return false;
    }
    engineFound |= name == "AirPlayQtEngine.dll";
    platformFound |= name == "platforms/qwindows.dll";
    const auto basename = path.filename().wstring();
    if (systemRuntime(basename))
      continue;
    for (auto module : modules) {
      const auto loaded = modulePath(module);
      if (_wcsicmp(loaded.filename().c_str(), basename.c_str()) == 0 &&
          hashFile(loaded) != expected) {
        fail(L"Conflicting loaded DLL: " + loaded.wstring() + L"; expected " +
             path.wstring());
        return false;
      }
    }
  }
  if (!manifest.eof() || !engineFound || !platformFound)
    throw std::runtime_error("Incomplete runtime manifest");
  return true;
}
} // namespace

extern "C" __declspec(dllexport) bool InitDll() noexcept {
  std::lock_guard lock(mutex);
  try {
    if (references) {
      if (references == std::numeric_limits<unsigned>::max())
        return false;
      ++references;
      return true;
    }
    lastError.clear();
    const auto directory = modulePath(selfModule).parent_path() / L"runtime";
    if (!verify(directory))
      return false;
    HMODULE candidate = LoadLibraryExW(
        (directory / L"AirPlayQtEngine.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!candidate) {
      fail(L"LoadLibraryExW failed, Win32 error " +
           std::to_wstring(GetLastError()));
      return false;
    }
    // Recheck modules loaded by Windows while resolving Engine's imports.
    // Never initialize the business runtime after resolving a different DLL.
    try {
      if (!verify(directory)) {
        FreeLibrary(candidate);
        return false;
      }
    } catch (...) {
      FreeLibrary(candidate);
      throw;
    }
    auto init = reinterpret_cast<Switch>(GetProcAddress(candidate, "InitDll"));
    auto done = reinterpret_cast<Switch>(GetProcAddress(candidate, "ExitDll"));
    auto ready = reinterpret_cast<Switch>(
        GetProcAddress(candidate, "AirPlayQtCanUnload"));
    auto getFactory = reinterpret_cast<Factory>(
        GetProcAddress(candidate, "GetPluginFactory"));
    if (!init || !done || !ready || !getFactory || !init()) {
      fail(L"Engine entry points missing or initialization failed");
      FreeLibrary(candidate);
      return false;
    }
    engine = candidate;
    engineExit = done;
    canUnload = ready;
    factory = getFactory;
    references = 1;
    return true;
  } catch (const std::exception &error) {
    const std::string text = error.what();
    fail(std::wstring(text.begin(), text.end()));
    return false;
  }
}
extern "C" __declspec(dllexport) void *GetPluginFactory() noexcept {
  std::lock_guard lock(mutex);
  return references && factory ? factory() : nullptr;
}
extern "C" __declspec(dllexport) bool ExitDll() noexcept {
  std::lock_guard lock(mutex);
  if (!references) {
    fail(L"Unpaired ExitDll");
    return false;
  }
  if (references > 1) {
    --references;
    return true;
  }
  if (!canUnload()) {
    fail(L"Engine still has live components or pending UI cleanup; retained");
    return false;
  }
  if (!engineExit()) {
    fail(L"Engine shutdown failed; retained");
    return false;
  }
  if (!FreeLibrary(engine)) {
    fail(L"FreeLibrary failed; retained");
    return false;
  }
  engine = nullptr;
  references = 0;
  factory = nullptr;
  engineExit = canUnload = nullptr;
  return true;
}
// Caller-owned buffer: no allocation ownership crosses the static CRT boundary.
extern "C" __declspec(dllexport) unsigned
AirPlayQtLoaderError(wchar_t *buffer, unsigned capacity) noexcept {
  std::lock_guard lock(mutex);
  if (buffer && capacity)
    wcsncpy_s(buffer, capacity, lastError.c_str(), _TRUNCATE);
  return unsigned(lastError.size());
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    selfModule = module;
  return TRUE;
}
