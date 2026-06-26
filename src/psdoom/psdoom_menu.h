/*
 * psDoom -- in-game options menu (entry point).
 *
 * The whole psDoom options page (its menu definition, drawing and navigation)
 * lives in psdoom_menu.c, isolated from the engine's own menus. The only thing
 * the engine needs is this entry point, wired to a single "psDoom" item in the
 * Options menu (see m_menu.c).
 */

#ifndef PSDOOM_MENU_H
#define PSDOOM_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Options-menu callback: open the psDoom options sub-menu. */
void M_PsDoom(int choice);

#ifdef __cplusplus
}
#endif

#endif /* PSDOOM_MENU_H */
