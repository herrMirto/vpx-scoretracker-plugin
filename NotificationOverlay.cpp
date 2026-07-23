// license:GPLv3+

#include "NotificationOverlay.h"

#include "common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

#include <SDL3/SDL.h>

namespace ScoreTracker
{

namespace
{

constexpr int kWidth = 360;
constexpr int kHeight = 96;
constexpr int kMargin = 24;
constexpr int kCornerRadius = 14;

struct Glyph
{
   char ch;
   std::array<uint8_t, 5> columns;
};

constexpr std::array<Glyph, 9> kGlyphs {{
   { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
   { 'a', { 0x20, 0x54, 0x54, 0x54, 0x78 } },
   { 'c', { 0x38, 0x44, 0x44, 0x44, 0x28 } },
   { 'd', { 0x38, 0x44, 0x44, 0x48, 0x7c } },
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

uint32_t MapColor(SDL_Surface* surface, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
   return SDL_MapRGBA(SDL_GetPixelFormatDetails(surface->format),
      SDL_GetSurfacePalette(surface), r, g, b, a);
}

void FillRoundedRect(SDL_Surface* surface, const SDL_Rect& bounds, int radius, uint32_t color)
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
      SDL_FillSurfaceRect(surface, &scanline, color);
   }
}

void DrawText(SDL_Surface* surface, std::string_view text, int x, int y, int scale,
   uint32_t color)
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
               SDL_FillSurfaceRect(surface, &pixel, color);
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
   int count = 0;
   SDL_Window** windows = SDL_GetWindows(&count);
   if (windows == nullptr)
      return nullptr;

   SDL_Window* playfield = nullptr;
   for (int i = 0; i < count; ++i)
   {
      const char* title = SDL_GetWindowTitle(windows[i]);
      if (title != nullptr && std::strcmp(title, "Visual Pinball Player") == 0)
      {
         playfield = windows[i];
         break;
      }
   }
   SDL_free(windows);
   return playfield;
}

bool NotificationOverlay::ShowScoreSaved()
{
   Hide();

   SDL_Window* parent = FindPlayfieldWindow();
   if (parent == nullptr)
   {
      LOGW("Could not show score saved notification: playfield window was not found");
      return false;
   }

   int parentWidth = 0;
   if (!SDL_GetWindowSize(parent, &parentWidth, nullptr))
      return false;

   const int x = std::max(kMargin, parentWidth - kWidth - kMargin);
   constexpr SDL_WindowFlags flags =
      SDL_WINDOW_TOOLTIP | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIDDEN;
   m_window = SDL_CreatePopupWindow(parent, x, kMargin, kWidth, kHeight, flags);
   if (m_window == nullptr)
   {
      m_window = SDL_CreatePopupWindow(parent, x, kMargin, kWidth, kHeight,
         SDL_WINDOW_TOOLTIP | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIDDEN);
   }
   if (m_window == nullptr || !Draw() || !SDL_ShowWindow(m_window))
   {
      Hide();
      return false;
   }

   LOGI("Displayed SDL score saved notification");
   return true;
}

bool NotificationOverlay::Draw()
{
   SDL_Surface* surface = SDL_GetWindowSurface(m_window);
   if (surface == nullptr)
      return false;

   const uint32_t transparent = MapColor(surface, 0, 0, 0, 0);
   const uint32_t green = MapColor(surface, 52, 211, 153, 255);
   const uint32_t background = MapColor(surface, 12, 22, 18, 245);
   const uint32_t text = MapColor(surface, 236, 253, 245, 255);
   SDL_FillSurfaceRect(surface, nullptr, transparent);

   const SDL_Rect outer { 0, 0, kWidth, kHeight };
   const SDL_Rect inner { 3, 3, kWidth - 6, kHeight - 6 };
   FillRoundedRect(surface, outer, kCornerRadius, green);
   FillRoundedRect(surface, inner, kCornerRadius - 3, background);

   constexpr std::string_view message = "Score Saved";
   constexpr int scale = 5;
   constexpr int textWidth = static_cast<int>(message.size()) * 6 * scale - scale;
   constexpr int textHeight = 7 * scale;
   DrawText(surface, message, (kWidth - textWidth) / 2, (kHeight - textHeight) / 2,
      scale, text);
   return SDL_UpdateWindowSurface(m_window);
}

void NotificationOverlay::Hide()
{
   if (m_window == nullptr)
      return;
   SDL_DestroyWindow(m_window);
   m_window = nullptr;
}

}
