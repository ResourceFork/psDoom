//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#ifndef __I_VIDEO_WINDOW__
#define __I_VIDEO_WINDOW__

#include "SDL.h"

static inline int I_IsFullScreenKeyShortcut(
    const SDL_KeyboardEvent *event)
{
    SDL_Scancode scancode = event->keysym.scancode;
    Uint16 modifiers = event->keysym.mod;

    if (scancode == SDL_SCANCODE_F12)
    {
        return 1;
    }

    if ((scancode == SDL_SCANCODE_RETURN
      || scancode == SDL_SCANCODE_KP_ENTER)
     && (modifiers & KMOD_ALT) != 0)
    {
        return 1;
    }

#if defined(__MACOSX__)
    if ((scancode == SDL_SCANCODE_RETURN
      || scancode == SDL_SCANCODE_KP_ENTER
      || scancode == SDL_SCANCODE_F)
     && (modifiers & KMOD_GUI) != 0)
    {
        return 1;
    }
#endif

    return 0;
}

static inline int I_ToggleFullScreenKeyShortcut(
    const SDL_KeyboardEvent *event)
{
    return event->repeat == 0 && I_IsFullScreenKeyShortcut(event);
}

#endif
