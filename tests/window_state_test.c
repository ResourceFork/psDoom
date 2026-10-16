/*
 * psDoom -- fullscreen shortcut and mouse grab tests.
 *
 * Exercises the production key and mouse grab predicates without creating an
 * SDL window.
 */

#include "i_video_window.h"

#include <stdio.h>
#include <string.h>

static int checks_run;
static int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static SDL_KeyboardEvent key_event(SDL_Scancode scancode, SDL_Keymod modifiers,
                                   Uint8 repeat)
{
    SDL_KeyboardEvent event;

    memset(&event, 0, sizeof(event));
    event.keysym.scancode = scancode;
    event.keysym.mod = modifiers;
    event.repeat = repeat;

    return event;
}

static void check_initial_keydown(SDL_Scancode scancode, SDL_Keymod modifiers,
                                  int expected)
{
    SDL_KeyboardEvent event;

    event = key_event(scancode, modifiers, 0);
    CHECK(I_IsFullScreenKeyShortcut(&event) == expected);
    CHECK(I_ToggleFullScreenKeyShortcut(&event) == expected);
}

static void check_repeated_keydown(SDL_Scancode scancode,
                                   SDL_Keymod modifiers, int recognized)
{
    SDL_KeyboardEvent event;

    event = key_event(scancode, modifiers, 1);
    CHECK(I_IsFullScreenKeyShortcut(&event) == recognized);
    CHECK(!I_ToggleFullScreenKeyShortcut(&event));
}

static void test_f12_shortcut(void)
{
    check_initial_keydown(SDL_SCANCODE_F12, KMOD_NONE, 1);
    check_initial_keydown(SDL_SCANCODE_F12, KMOD_LALT, 1);
    check_initial_keydown(SDL_SCANCODE_F12,
                          (SDL_Keymod) (KMOD_RCTRL | KMOD_LSHIFT), 1);
    check_repeated_keydown(SDL_SCANCODE_F12, KMOD_NONE, 1);
}

static void test_command_f_shortcut(void)
{
    check_initial_keydown(SDL_SCANCODE_F, KMOD_NONE, 0);
    check_initial_keydown(SDL_SCANCODE_F, KMOD_LALT, 0);

#if defined(__MACOSX__)
    check_initial_keydown(SDL_SCANCODE_F, KMOD_LGUI, 1);
    check_initial_keydown(SDL_SCANCODE_F, KMOD_RGUI, 1);
    check_repeated_keydown(SDL_SCANCODE_F, KMOD_LGUI, 1);
#else
    check_initial_keydown(SDL_SCANCODE_F, KMOD_LGUI, 0);
    check_initial_keydown(SDL_SCANCODE_F, KMOD_RGUI, 0);
    check_repeated_keydown(SDL_SCANCODE_F, KMOD_LGUI, 0);
#endif

    check_initial_keydown(SDL_SCANCODE_G, KMOD_LGUI, 0);
}

static void test_return_shortcuts(void)
{
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_LALT, 1);
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_RALT, 1);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_LALT, 1);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RALT, 1);

#if defined(__MACOSX__)
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_LGUI, 1);
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_RGUI, 1);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_LGUI, 1);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RGUI, 1);
#else
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_LGUI, 0);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RGUI, 0);
#endif

    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_NONE, 0);
    check_initial_keydown(SDL_SCANCODE_RETURN, KMOD_LCTRL, 0);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_NONE, 0);
    check_initial_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RCTRL, 0);

    check_repeated_keydown(SDL_SCANCODE_RETURN, KMOD_LALT, 1);
    check_repeated_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RALT, 1);
#if defined(__MACOSX__)
    check_repeated_keydown(SDL_SCANCODE_RETURN, KMOD_LGUI, 1);
    check_repeated_keydown(SDL_SCANCODE_KP_ENTER, KMOD_RGUI, 1);
#endif
}

static void test_f11_is_not_a_shortcut(void)
{
    check_initial_keydown(SDL_SCANCODE_F11, KMOD_NONE, 0);
    check_initial_keydown(SDL_SCANCODE_F11, KMOD_LALT, 0);
    check_initial_keydown(SDL_SCANCODE_F11, KMOD_LGUI, 0);
    check_repeated_keydown(SDL_SCANCODE_F11, KMOD_NONE, 0);
}

static grab_inputs_t playing(int fullscreen)
{
    grab_inputs_t in;

    memset(&in, 0, sizeof(in));
    in.window_focused = 1;
    in.fullscreen = fullscreen;
    in.mouse_enabled = 1;
    in.grab_enabled = 1;
    in.game_wants_grab = 1;

    return in;
}

/* The menu is open: the game's grab callback also says release. */
static grab_inputs_t menu_open(int fullscreen)
{
    grab_inputs_t in = playing(fullscreen);

    in.game_wants_grab = 0;
    in.menu_open = 1;

    return in;
}

/* Pause, demo, intermission or title screen: release, but no menu. */
static grab_inputs_t not_playing(int fullscreen)
{
    grab_inputs_t in = playing(fullscreen);

    in.game_wants_grab = 0;

    return in;
}

static void check_menu_open_close(int fullscreen)
{
    grab_inputs_t in;

    in = playing(fullscreen);
    CHECK(I_MouseShouldBeGrabbed(&in));
    in = menu_open(fullscreen);
    CHECK(!I_MouseShouldBeGrabbed(&in));
    in = playing(fullscreen);
    CHECK(I_MouseShouldBeGrabbed(&in));
}

static void test_windowed_grab_follows_menu(void)
{
    check_menu_open_close(0);
}

static void test_fullscreen_grab_follows_menu(void)
{
    check_menu_open_close(1);
}

/* Outside gameplay, windowed releases; full screen keeps the pointer
 * hidden, as upstream does, unless the menu is open. */
static void test_not_playing_without_menu(void)
{
    grab_inputs_t in;

    in = not_playing(0);
    CHECK(!I_MouseShouldBeGrabbed(&in));
    in = not_playing(1);
    CHECK(I_MouseShouldBeGrabbed(&in));
}

static void test_unfocused_or_screensaver_never_grabs(void)
{
    int fullscreen;

    for (fullscreen = 0; fullscreen <= 1; fullscreen++)
    {
        grab_inputs_t in = playing(fullscreen);

        in.window_focused = 0;
        CHECK(!I_MouseShouldBeGrabbed(&in));

        in = playing(fullscreen);
        in.screensaver_mode = 1;
        CHECK(!I_MouseShouldBeGrabbed(&in));
    }
}

/* With mouse input turned off, windowed mode leaves the pointer alone and
 * full screen grabs to keep it hidden, whatever the game or menu says. */
static void test_mouse_disabled(void)
{
    int fullscreen, wants, menu;

    for (fullscreen = 0; fullscreen <= 1; fullscreen++)
    for (wants = 0; wants <= 1; wants++)
    for (menu = 0; menu <= 1; menu++)
    {
        grab_inputs_t in = playing(fullscreen);

        in.game_wants_grab = wants;
        in.menu_open = menu;
        in.mouse_enabled = 0;
        CHECK(I_MouseShouldBeGrabbed(&in) == fullscreen);
    }
}

/* With grabbing turned off, windowed mode never grabs; full screen still
 * hides the pointer during play but releases it for the menu. */
static void test_grab_disabled(void)
{
    int wants, menu;

    for (wants = 0; wants <= 1; wants++)
    for (menu = 0; menu <= 1; menu++)
    {
        grab_inputs_t in = playing(0);

        in.game_wants_grab = wants;
        in.menu_open = menu;
        in.grab_enabled = 0;
        CHECK(!I_MouseShouldBeGrabbed(&in));

        in.fullscreen = 1;
        CHECK(I_MouseShouldBeGrabbed(&in) == !menu);
    }
}

static int warp(int was_grabbed, int grab, int fullscreen, int focused,
                int *x, int *y)
{
    int w = fullscreen ? 2560 : 640;
    int h = fullscreen ? 1440 : 480;

    *x = *y = -1;
    return I_GrabReleaseWarp(was_grabbed, grab, fullscreen, focused,
                             w, h, x, y);
}

static void test_release_warp(void)
{
    int x, y;

    /* Releasing: windowed bottom-right, full screen centre. */
    CHECK(warp(1, 0, 0, 1, &x, &y) && x == 624 && y == 464);
    CHECK(warp(1, 0, 1, 1, &x, &y) && x == 1280 && y == 720);

    /* Not a release edge: no warp. */
    CHECK(!warp(0, 0, 0, 1, &x, &y));
    CHECK(!warp(0, 0, 1, 1, &x, &y));
    CHECK(!warp(1, 1, 1, 1, &x, &y));
    CHECK(!warp(0, 1, 0, 1, &x, &y));

    /* Released because focus was lost: leave the cursor alone. */
    CHECK(!warp(1, 0, 1, 0, &x, &y));
    CHECK(!warp(1, 0, 0, 0, &x, &y));
}

int main(void)
{
    test_f12_shortcut();
    test_command_f_shortcut();
    test_return_shortcuts();
    test_f11_is_not_a_shortcut();
    test_windowed_grab_follows_menu();
    test_fullscreen_grab_follows_menu();
    test_not_playing_without_menu();
    test_unfocused_or_screensaver_never_grabs();
    test_mouse_disabled();
    test_grab_disabled();
    test_release_warp();

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
