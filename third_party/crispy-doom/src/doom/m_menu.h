//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//   Menu widget stuff, episode selection and such.
//    


#ifndef __M_MENU__
#define __M_MENU__



#include "d_event.h"

//
// [crispy] menu item / menu definitions. Exposed here (rather than kept private
// to m_menu.c) so the psDoom options menu (src/psdoom/psdoom_menu.c) can define
// its own self-contained menu page without living in m_menu.c.
//
typedef struct
{
    // 0 = no cursor here, 1 = ok, 2 = arrows ok
    // [crispy] 3 = arrows ok, no mouse x
    // [crispy] 4 = arrows ok, enter for numeric entry, no mouse x
    short	status;

    char	name[10];

    // choice = menu item #.
    // if status = 2 or 3,
    //   choice=0:leftarrow,1:rightarrow
    // [crispy] if status = 4,
    //   choice=0:leftarrow,1:rightarrow,2:enter
    void	(*routine)(int choice);

    // hotkey in menu
    char	alphaKey;
    const char	*alttext; // [crispy] alternative text for menu items
} menuitem_t;

typedef struct menu_s
{
    short		numitems;	// # of menu items
    struct menu_s*	prevMenu;	// previous menu
    menuitem_t*		menuitems;	// menu items
    void		(*routine)();	// draw routine
    short		x;
    short		y;		// x,y of menu
    short		lastOn;		// last item user was on in menu
    short		lumps_missing;	// [crispy] indicate missing menu graphics lumps
} menu_t;

// Switch to a different menu page.
void M_SetupNextMenu(menu_t *menudef);

// Draw a string using the HUD font (supports color escapes).
void M_WriteText(int x, int y, const char *string);

//
// MENUS
//
// Called by main loop,
// saves config file and calls I_Quit when user exits.
// Even when the menu is not displayed,
// this can resize the view and change game parameters.
// Does all the real work of the menu interaction.
boolean M_Responder (event_t *ev);


// Called by main loop,
// only used for menu (skull cursor) animation.
void M_Ticker (void);

// Called by main loop,
// draws the menus directly into the screen buffer.
void M_Drawer (void);

// Called by D_DoomMain,
// loads the config file.
void M_Init (void);

// Called by intro code to force menu up upon a keypress,
// does nothing if menu is already up.
void M_StartControlPanel (void);

// [crispy] Propagate default difficulty setting change
void M_SetDefaultDifficulty (void);

extern int detailLevel;
extern int screenblocks;

extern boolean inhelpscreens;
extern int showMessages;

// [crispy] Numeric entry
extern boolean numeric_enter;
extern int numeric_entry;

#endif
