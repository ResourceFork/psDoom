/*
 * psDoom -- fullscreen shortcut tests.
 *
 * Exercises the production key predicates without creating an SDL window.
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

int main(void)
{
    test_f12_shortcut();
    test_command_f_shortcut();
    test_return_shortcuts();
    test_f11_is_not_a_shortcut();

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
