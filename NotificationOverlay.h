// license:GPLv3+

#pragma once

struct SDL_Window;

namespace ScoreTracker
{

class NotificationOverlay final
{
public:
   ~NotificationOverlay();

   bool ShowScoreSaved();
   void Hide();

private:
   static SDL_Window* FindPlayfieldWindow();
   bool Draw();

   SDL_Window* m_window = nullptr;
};

}
