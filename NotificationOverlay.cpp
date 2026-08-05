// license:GPLv3+

#include "NotificationOverlay.h"

#include "common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#define SCORETRACKER_SDL_CALL __cdecl
#else
#include <dlfcn.h>
#define SCORETRACKER_SDL_CALL
#endif

struct SDL_Surface;
struct SDL_Rect
{
   int x;
   int y;
   int w;
   int h;
};
using SDL_WindowFlags = uint64_t;

namespace ScoreTracker
{

namespace
{

constexpr int kWidth = 360;
constexpr int kHeight = 96;
constexpr int kMargin = 24;
constexpr int kCornerRadius = 14;

constexpr SDL_WindowFlags kWindowHidden = 0x0000000000000008ULL;
constexpr SDL_WindowFlags kWindowTooltip = 0x0000000000040000ULL;
constexpr SDL_WindowFlags kWindowTransparent = 0x0000000040000000ULL;
constexpr SDL_WindowFlags kWindowNotFocusable = 0x0000000080000000ULL;

template <typename T>
T ResolveSDL(const char* name)
{
#if defined(_WIN32)
   HMODULE module = GetModuleHandleA("SDL364.dll");
   if (module == nullptr)
      module = GetModuleHandleA("SDL3.dll");
   return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
#else
   return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
#endif
}

struct SDLApi
{
   SDL_Window** (SCORETRACKER_SDL_CALL *GetWindows)(int*) = ResolveSDL<decltype(GetWindows)>("SDL_GetWindows");
   const char* (SCORETRACKER_SDL_CALL *GetWindowTitle)(SDL_Window*) = ResolveSDL<decltype(GetWindowTitle)>("SDL_GetWindowTitle");
   void (SCORETRACKER_SDL_CALL *Free)(void*) = ResolveSDL<decltype(Free)>("SDL_free");
   bool (SCORETRACKER_SDL_CALL *GetWindowSize)(SDL_Window*, int*, int*) = ResolveSDL<decltype(GetWindowSize)>("SDL_GetWindowSize");
   SDL_Window* (SCORETRACKER_SDL_CALL *CreatePopupWindow)(SDL_Window*, int, int, int, int, SDL_WindowFlags)
      = ResolveSDL<decltype(CreatePopupWindow)>("SDL_CreatePopupWindow");
   SDL_Surface* (SCORETRACKER_SDL_CALL *GetWindowSurface)(SDL_Window*) = ResolveSDL<decltype(GetWindowSurface)>("SDL_GetWindowSurface");
   uint32_t (SCORETRACKER_SDL_CALL *MapSurfaceRGBA)(SDL_Surface*, uint8_t, uint8_t, uint8_t, uint8_t)
      = ResolveSDL<decltype(MapSurfaceRGBA)>("SDL_MapSurfaceRGBA");
   bool (SCORETRACKER_SDL_CALL *FillSurfaceRect)(SDL_Surface*, const SDL_Rect*, uint32_t)
      = ResolveSDL<decltype(FillSurfaceRect)>("SDL_FillSurfaceRect");
   bool (SCORETRACKER_SDL_CALL *UpdateWindowSurface)(SDL_Window*) = ResolveSDL<decltype(UpdateWindowSurface)>("SDL_UpdateWindowSurface");
   bool (SCORETRACKER_SDL_CALL *ShowWindow)(SDL_Window*) = ResolveSDL<decltype(ShowWindow)>("SDL_ShowWindow");
   void (SCORETRACKER_SDL_CALL *DestroyWindow)(SDL_Window*) = ResolveSDL<decltype(DestroyWindow)>("SDL_DestroyWindow");

   bool IsAvailable() const
   {
      return GetWindows != nullptr && GetWindowTitle != nullptr && Free != nullptr
         && GetWindowSize != nullptr && CreatePopupWindow != nullptr
         && GetWindowSurface != nullptr && MapSurfaceRGBA != nullptr
         && FillSurfaceRect != nullptr && UpdateWindowSurface != nullptr
         && ShowWindow != nullptr && DestroyWindow != nullptr;
   }
};

SDLApi& GetSDL()
{
   static SDLApi api;
   return api;
}

struct Glyph
{
   char ch;
   std::array<uint8_t, 5> columns;
};

constexpr std::array<Glyph, 9> kGlyphs {{
   { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
   { 'a', { 0x20, 0x54, 0x54, 0x54, 0x78 } },
   { 'c', { 0x38, 0x44, 0x44, 0x44, 0x28 } },
   { 'd', { 0x38, 0x44, 0x44, 0x48, 0x7f } },
   { 'e', { 0x38, 0x54, 0x54, 0x54, 0x18 } },
   { 'o', { 0x38, 0x44, 0x44, 0x44, 0x38 } },
   { 'r', { 0x7c, 0x08, 0x04, 0x04, 0x08 } },
   { 'v', { 0x1c, 0x20, 0x40, 0x20, 0x1c } },
   { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
}};

const Glyph* FindGlyph(char ch)
{
   const auto it = std::find_if(kGlyphs.begin(), kGlyphs.end(),
      [ch](const Glyph& glyph) { return glyph.ch == ch; });
   return it == kGlyphs.end() ? nullptr : &*it;
}

void FillRoundedRect(SDLApi& api, SDL_Surface* surface, const SDL_Rect& bounds,
   int radius, uint32_t color)
{
   const int safeRadius = std::max(0, std::min({ radius, bounds.w / 2, bounds.h / 2 }));
   for (int row = 0; row < bounds.h; ++row)
   {
      int inset = 0;
      if (row < safeRadius)
      {
         const int dy = safeRadius - row - 1;
         inset = safeRadius - static_cast<int>(
            std::sqrt(static_cast<double>(safeRadius * safeRadius - dy * dy)));
      }
      else if (row >= bounds.h - safeRadius)
      {
         const int dy = row - (bounds.h - safeRadius);
         inset = safeRadius - static_cast<int>(
            std::sqrt(static_cast<double>(safeRadius * safeRadius - dy * dy)));
      }
      SDL_Rect scanline { bounds.x + inset, bounds.y + row, bounds.w - inset * 2, 1 };
      api.FillSurfaceRect(surface, &scanline, color);
   }
}

void DrawText(SDLApi& api, SDL_Surface* surface, std::string_view text, int x, int y,
   int scale, uint32_t color)
{
   for (const char ch : text)
   {
      const Glyph* glyph = FindGlyph(ch);
      if (glyph != nullptr)
      {
         for (int column = 0; column < 5; ++column)
         {
            for (int row = 0; row < 7; ++row)
            {
               if ((glyph->columns[column] & (1u << row)) == 0)
                  continue;
               SDL_Rect pixel { x + column * scale, y + row * scale, scale, scale };
               api.FillSurfaceRect(surface, &pixel, color);
            }
         }
      }
      x += 6 * scale;
   }
}

}

NotificationOverlay::~NotificationOverlay() { Hide(); }

SDL_Window* NotificationOverlay::FindPlayfieldWindow()
{
   SDLApi& api = GetSDL();
   if (!api.IsAvailable())
      return nullptr;

   int count = 0;
   SDL_Window** windows = api.GetWindows(&count);
   if (windows == nullptr)
      return nullptr;

   SDL_Window* playfield = nullptr;
   for (int i = 0; i < count; ++i)
   {
      const char* title = api.GetWindowTitle(windows[i]);
      if (title != nullptr && std::strcmp(title, "Visual Pinball Player") == 0)
      {
         playfield = windows[i];
         break;
      }
   }
   api.Free(windows);
   return playfield;
}

bool NotificationOverlay::ShowScoreSaved()
{
   Hide();

   SDLApi& api = GetSDL();
   if (!api.IsAvailable())
   {
      LOGW("Could not show score saved notification: SDL3 runtime API is unavailable");
      return false;
   }

   SDL_Window* parent = FindPlayfieldWindow();
   if (parent == nullptr)
   {
      LOGW("Could not show score saved notification: playfield window was not found");
      return false;
   }

   int parentWidth = 0;
   if (!api.GetWindowSize(parent, &parentWidth, nullptr))
      return false;

   const int x = std::max(kMargin, parentWidth - kWidth - kMargin);
   constexpr SDL_WindowFlags flags =
      kWindowTooltip | kWindowTransparent | kWindowNotFocusable | kWindowHidden;
   m_window = api.CreatePopupWindow(parent, x, kMargin, kWidth, kHeight, flags);
   if (m_window == nullptr)
   {
      m_window = api.CreatePopupWindow(parent, x, kMargin, kWidth, kHeight,
         kWindowTooltip | kWindowNotFocusable | kWindowHidden);
   }
   if (m_window == nullptr || !Draw() || !api.ShowWindow(m_window))
   {
      Hide();
      return false;
   }

   LOGI("Displayed SDL score saved notification");
   return true;
}

bool NotificationOverlay::Draw()
{
   SDLApi& api = GetSDL();
   SDL_Surface* surface = api.GetWindowSurface(m_window);
   if (surface == nullptr)
      return false;

   const uint32_t transparent = api.MapSurfaceRGBA(surface, 0, 0, 0, 0);
   const uint32_t green = api.MapSurfaceRGBA(surface, 52, 211, 153, 255);
   const uint32_t background = api.MapSurfaceRGBA(surface, 12, 22, 18, 245);
   const uint32_t text = api.MapSurfaceRGBA(surface, 236, 253, 245, 255);
   api.FillSurfaceRect(surface, nullptr, transparent);

   const SDL_Rect outer { 0, 0, kWidth, kHeight };
   const SDL_Rect inner { 3, 3, kWidth - 6, kHeight - 6 };
   FillRoundedRect(api, surface, outer, kCornerRadius, green);
   FillRoundedRect(api, surface, inner, kCornerRadius - 3, background);

   constexpr std::string_view message = "Score Saved";
   constexpr int scale = 3;
   constexpr int textWidth = static_cast<int>(message.size()) * 6 * scale - scale;
   constexpr int textHeight = 7 * scale;
   DrawText(api, surface, message, (kWidth - textWidth) / 2,
      (kHeight - textHeight) / 2, scale, text);
   return api.UpdateWindowSurface(m_window);
}

void NotificationOverlay::Hide()
{
   if (m_window == nullptr)
      return;
   SDLApi& api = GetSDL();
   if (api.DestroyWindow != nullptr)
      api.DestroyWindow(m_window);
   m_window = nullptr;
}

}

#undef SCORETRACKER_SDL_CALL
