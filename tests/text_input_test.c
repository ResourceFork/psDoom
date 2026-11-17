/*
 * psDoom -- SDL text input follows the game's text fields.
 *
 * While SDL text input is active, macOS routes key presses through its input
 * method, so holding a vowel opens the accent popup instead of repeating the
 * key. These checks drive the production i_input.c (with the dummy SDL video
 * driver and a hidden window: nothing is shown) and require text input to be
 * off in play and on only while a save name, chat line or numeric entry takes
 * typed text, without losing the typed characters.
 */

#include "SDL.h"

#include "crispy.h"
#include "d_event.h"
#include "doomkeys.h"
#include "i_input.h"

#include <stdio.h>
#include <string.h>

extern int vanilla_keyboard_mapping;

/* The checks counters live in window_state_test.c, which runs these. */
extern int checks_run;
extern int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* Stubs for the engine symbols i_input.c links against. */
event_t fastmouse;
boolean newfastmouse;
static crispy_t crispy_s;
crispy_t *const crispy = &crispy_s;

static event_t last_event;
static int posted_events;

void D_PostEvent(event_t *ev)
{
    last_event = *ev;
    posted_events++;
}

int I_GetTimeMS(void)
{
    return 0;
}

void M_BindIntVariable(const char *name, int *variable)
{
    (void) name;
    (void) variable;
}

void M_BindFloatVariable(const char *name, float *variable)
{
    (void) name;
    (void) variable;
}

/* Press 'a' and return the typed char (data3) the game sees. With vanilla
 * keyboard mapping it comes from the key code; otherwise from the
 * SDL_TEXTINPUT the macOS input method queues after the key down, which only
 * arrives while SDL text input is active. sdl2-compat crashes on an
 * SDL_TEXTINPUT the application pushes, so for that mode the test checks the
 * SDL state only, and typing in a field is part of the live check. */
static int type_a(void)
{
    SDL_Event key;

    memset(&key, 0, sizeof(key));
    key.type = SDL_KEYDOWN;
    key.key.keysym.scancode = SDL_SCANCODE_A;
    key.key.keysym.sym = SDLK_a;

    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    posted_events = 0;
    I_HandleKeyboardEvent(&key);

    return posted_events == 1 ? last_event.data3 : -1;
}

static void check_modes(int vanilla)
{
    vanilla_keyboard_mapping = vanilla;

    /* Start-up, as SetVideoMode leaves it once the window exists: in play. */
    I_SyncTextInput();
    CHECK(!SDL_IsTextInputActive());
    CHECK(type_a() == 0);

    /* Save-game name: typed text arrives. With vanilla mapping the name is
     * built from key codes, so SDL text input (and the popup) stays off. */
    I_StartTextInput(0, 0, 0, 0);
    CHECK(SDL_IsTextInputActive() == !vanilla);
    if (vanilla)
    {
        CHECK(type_a() == 'a');
    }

    /* Closing the prompt (Enter or Escape) returns to play. */
    I_StopTextInput();
    CHECK(!SDL_IsTextInputActive());
    CHECK(type_a() == 0);

    /* Chat and the Crispness numeric entry use the same pair. */
    I_StartTextInput(0, 8, 320, 16);
    CHECK(SDL_IsTextInputActive() == !vanilla);
    if (vanilla)
    {
        CHECK(type_a() == 'a');
    }
    I_StopTextInput();
    CHECK(!SDL_IsTextInputActive());

    /* A stop with no field open is harmless and keeps it off. */
    I_StopTextInput();
    CHECK(!SDL_IsTextInputActive());
}

void test_text_input_follows_text_fields(void)
{
    SDL_Window *window;

    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        checks_run++;
        checks_failed++;
        fprintf(stderr, "FAIL: SDL_Init(dummy video): %s\n", SDL_GetError());
        return;
    }

    window = SDL_CreateWindow("psdoom-text-input-test", 0, 0, 64, 64,
                              SDL_WINDOW_HIDDEN);
    CHECK(window != NULL);

    check_modes(1);
    check_modes(0);

    if (window != NULL)
    {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}
