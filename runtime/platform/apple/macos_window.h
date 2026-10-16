#pragma once

struct SDL_Window;

namespace platform::macos
{
    void ConfigureWindow(SDL_Window* window);
    void SetTrafficLightsVisible(SDL_Window* window, bool visible);
}
