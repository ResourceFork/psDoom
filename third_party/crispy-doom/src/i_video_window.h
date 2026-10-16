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

// Inputs to the mouse grab decision, sampled once per frame.

typedef struct
{
    int screensaver_mode;
    int window_focused;
    int fullscreen;
    int mouse_enabled;   // usemouse && !nomouse
    int grab_enabled;    // grabmouse && !nograbmouse_override
    int game_wants_grab; // grab callback result (menu, pause, demo, state)
    int menu_open;       // [psDoom] menu callback result; 0 when unset
} grab_inputs_t;

static inline int I_MouseShouldBeGrabbed(const grab_inputs_t *in)
{
    // never grab the mouse when in screensaver mode

    if (in->screensaver_mode)
        return 0;

    // if the window doesn't have focus, never grab it

    if (!in->window_focused)
        return 0;

    // Don't grab the mouse if mouse input is disabled, except in full
    // screen (dont want to see the mouse pointer)

    if (!in->mouse_enabled)
        return in->fullscreen;

    // always grab the mouse when full screen, [psDoom] unless the
    // menu is open: it must release the mouse and show the cursor

    if (in->fullscreen)
        return !in->menu_open;

    // if we specify not to grab the mouse, never grab

    if (!in->grab_enabled)
        return 0;

    return in->game_wants_grab;
}

// Whether to warp the cursor as the grab is released, and where: a
// minimally distracting place, the bottom-right of the window. [psDoom]
// In full screen, the centre, so the cursor is not pinned against the
// screen edge (which stops mouse menu navigation down and right, and
// can reveal the Dock or trip a hot corner on macOS). Never move the
// mouse around while the window is unfocused.

static inline int I_GrabReleaseWarp(int was_grabbed, int grab,
                                    int fullscreen, int window_focused,
                                    int w, int h, int *x, int *y)
{
    if (!was_grabbed || grab || !window_focused)
        return 0;

    if (fullscreen)
    {
        *x = w / 2;
        *y = h / 2;
    }
    else
    {
        *x = w - 16;
        *y = h - 16;
    }

    return 1;
}

#endif
