// license:GPLv3+

#include "common.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <tchar.h>
#else
#include <dlfcn.h>
#include <climits>
#include <cstdlib>
#endif

namespace ScoreTracker
{

#ifdef _WIN32
template <typename T> static T GetModulePath(HMODULE hModule)
{
   DWORD size = MAX_PATH;
   while (true)
   {
      T path(size, 0);
      DWORD length;
      if constexpr (std::is_same_v<T, std::string>)
         length = ::GetModuleFileNameA(hModule, path.data(), size);
      else
         length = ::GetModuleFileNameW(hModule, path.data(), size);
      if (length == 0)
         return {};
      if (length < size)
      {
         path.resize(length); // Trim excess
         return path;
      }
      // length == size could both mean that it just did fit in, or it was truncated, so try again with a bigger buffer
      size *= 2;
   }
}
#endif

std::filesystem::path GetPluginPath()
{
#ifdef _WIN32
   HMODULE hm = nullptr;
   if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCTSTR>(&GetPluginPath), &hm) == 0)
      return std::filesystem::path();

#ifdef _UNICODE
   const std::wstring pathBuf = GetModulePath<std::wstring>(hm);
#else
   const string pathBuf = GetModulePath<string>(hm);
#endif
#else
   Dl_info info {};
   if (dladdr((void*)&GetPluginPath, &info) == 0 || !info.dli_fname)
      return string();

   char pathBuf[PATH_MAX];
   if (!realpath(info.dli_fname, pathBuf))
      return string();
#endif

   std::filesystem::path path(pathBuf);
   return path.empty() ? path : path.parent_path();
}

}
