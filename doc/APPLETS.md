# Writing an applet

An applet is a mode you add to gbagate without touching the firmware's core.
You write one C file, list it in one place, and it appears in the main menu.
The same dispatcher as the built-in modes starts and stops it, it gets the
same buttons, and it draws in the current colour theme.

Three examples ship in `applets/`:

| file | shows | in the menu |
|---|---|---|
| `hello.c` | the smallest page applet: state, input, a page | no (uncomment it in `applets.def`) |
| `test_pattern.c` | drawing the whole canvas yourself | yes, **Test Pattern** |
| `chip_dice.c` | asking for a game, using host services, refusing cleanly | yes, **Chip Dice** |
| `battle_chip.c` | the gate, the chip reader and saved favourites as services; tabs, favourite mark and sort as inputs; its own controls | yes, **Battle Chip** |

`test/test_applets.c` runs all three the way the firmware does, and checks
every behaviour this page describes. `make run-applets` runs it.

## 1. The smallest applet

This is `applets/hello.c` in full. It counts button presses and turns of the
knob:

```c
#include "applet.h"

static uint32_t s_presses;
static int32_t  s_turned;

static bool hello_enter(const applet_env_t *env) {
    (void)env;
    s_presses = 0;     /* every visit starts fresh */
    s_turned  = 0;
    return true;
}

static bool hello_input(const applet_env_t *env, applet_input_t in, int32_t value) {
    (void)env;
    switch (in) {
    case APPLET_IN_CONFIRM: s_presses++;       return true;
    case APPLET_IN_MOVE:    s_turned += value; return true;
    default:                return false;   /* Back: let the host leave */
    }
}

static void hello_page(const applet_env_t *env, ui_page_t *p) {
    (void)env;
    ui_page_add(p, COL_TEXT, "Hello, gbagate!");
    ui_page_add(p, COL_TEXT_DIM, "pressed %lu times", (unsigned long)s_presses);
    ui_page_add(p, COL_TEXT_DIM, "turned  %ld", (long)s_turned);
}

const applet_t applet_hello = {
    .name     = "Hello",
    .requires = HW_CAP_DISPLAY,
    .enter    = hello_enter,
    .input    = hello_input,
    .page     = hello_page,
};
```

Three things to notice:

- **State lives in statics**, and `enter()` resets it. Only one mode runs at a
  time, so an applet never shares its state with a second copy of itself.
- **`input()` returns whether it used the button.** If an applet doesn't use
  Back, the host takes it as "leave". So Back always works unless the applet
  has something of its own to cancel first.
- **`page()` only describes the screen.** The host has already set the title
  (the applet's name) and the right half of the footer (how to leave, such as
  `hold back`). The applet adds lines, and the host draws them in the current
  theme. To say what the confirm key does, put it in the left half: pass
  `ui_keys_for()` and a verb of five letters or fewer to `ui_page_keys()`, or
  write `p->footer` directly. A page holds 8 lines of
  20 characters; `ui_page_add_wrapped()` breaks a longer text at spaces.

## 2. Put it in the menu

`applets/applets.def` lists the applets built into the firmware, in menu
order:

```c
APPLET(test_pattern)
APPLET(chip_dice)
APPLET(hello)        /* add this line */
```

`APPLET(hello)` refers to `const applet_t applet_hello`, defined in
`applets/hello.c`. That's all. The firmware build already compiles everything
in `applets/`, and an applet that isn't listed is compiled but left out of the
image by the linker.

The menu shows applets after the built-in modes and before Settings and
Diagnostics, which stay at the bottom on every board. At most `APPLET_MAX` (8)
are listed.

## 3. Drawing the whole screen

A page is enough for most applets. When it isn't, provide `draw()` instead of
`page()` and you get the whole 128×128 canvas. From `applets/test_pattern.c`:

```c
static void tp_draw(const applet_env_t *env, const ui_canvas_t *c) {
    (void)env;
    switch (s_pattern) {
    case PAT_BARS:   bars(c);   break;
    case PAT_BORDER: border(c); break;
    case PAT_GRID:   grid(c);   break;
    default:         theme(c);  break;
    }
    ...
}

const applet_t applet_test_pattern = {
    .name  = "Test Pattern",
    ...
    .draw  = tp_draw,          /* instead of .page */
};
```

The primitives are in `src/ui_render.h`:

- `ui_fill(c, x, y, w, h, colour)` fills a rectangle; anything off the canvas
  is clipped.
- `ui_text(c, x, y, "text", colour)` draws text in the 5×7 font, which
  advances 6 px per character.
- `ui_text_clipped(c, x, y, max_w, "text", colour)` stops at the last whole
  character that fits.

Use the theme's colours (`COL_TEXT`, `COL_BG`, `ui_theme()->good`, and so on)
for anything that isn't the content itself, so the applet matches whichever
theme the owner chose. `RGB565(r, g, b)` makes a fixed colour, which is right
for a test pattern and wrong for a background.

An applet provides exactly one of `page()` and `draw()`. One with neither, or
with both, is not listed: a mode that draws nothing is indistinguishable from
a frozen device.

## 4. Asking for a game, and using the host's services

`applets/chip_dice.c` rolls a random Battle Chip, so it needs to know which
game's chips to roll, and it needs that game's database:

```c
static bool dice_enter(const applet_env_t *env) {
    s_rolls = 0;
    s_have  = false;
    return env->open_db && env->has_game &&
           env->open_db(env->game, &s_db) && chipdb_count(&s_db) > 0;
}

const applet_t applet_chip_dice = {
    .name       = "Chip Dice",
    .needs_game = true,        /* the menu asks which game first */
    .enter      = dice_enter,
    ...
};
```

- **`needs_game = true`** makes the menu ask for a game before starting the
  applet, as Battle Chip does. Back from the applet returns to the game list.
- **`env`** is what the host offers:

  | field | what |
  |---|---|
  | `now_ms` | milliseconds since boot, refreshed every frame |
  | `caps` | which hardware this board has (`HW_CAP_*`) |
  | `game`, `has_game` | the chosen game, for an applet with `needs_game` |
  | `hint` | how to leave (`hold back`, `B back`), the right half of a page's footer |
  | `open_db(game, &db)` | opens a game's chip database. **NULL** on an image without databases |
  | `random()` | 32 random bits (the ESP32's hardware generator). Never NULL |
  | `gate_start(game)`, `gate_stop()` | be that game's Battle Chip Gate on the link port. NULL without a link port |
  | `gate_send(wire_id)`, `gate_cancel()`, `gate_linked()` | send a chip by wire ID (`chipdb_id_for_slot`, never the record's index); drop one still going out; is the game clocking us |
  | `read_chip()` | the wire ID of a chip in the reader board, or `CG_CHIP_NONE`. NULL without a reader. Read it a few times a second, not every frame |
  | `favorites`, `favorites_len`, `favorites_changed()` | a bit per chip record, kept across power cycles; call `favorites_changed()` after changing it |

  Treat every service as optional except `random()`.
- **Refuse in `enter()`** when the applet can't run. The menu then says "mode
  did not start: refused" instead of showing an empty screen. `exit()` still
  runs after a refusal, so release whatever `enter()` took there.

## 5. Callbacks, in full

```c
typedef struct applet {
    const char *name;         /* menu label and page title (max 18 characters) */
    uint32_t    requires;     /* HW_CAP_* bits; hidden from the menu without them */
    const char *absent_hint;  /* why it is hidden, for Diagnostics */
    bool        needs_game;

    bool (*enter)(const applet_env_t *env);                  /* optional */
    void (*exit)(void);                                      /* optional */
    void (*tick)(const applet_env_t *env);                   /* optional */
    bool (*input)(const applet_env_t *env, applet_input_t in, int32_t value);
    void (*page)(const applet_env_t *env, ui_page_t *page);  /* one of these */
    void (*draw)(const applet_env_t *env, const ui_canvas_t *canvas);
} applet_t;
```

- **`tick()`** runs every UI iteration, about 60 times a second, whether or
  not a button was pressed. Use it for anything that moves on its own: a
  timer, an animation, polling a peripheral.
- **`page()` or `draw()`** is called every frame, so a screen can change
  without input.
- **`input()`** gets one intent at a time:

  | intent | value | from |
  |---|---|---|
  | `APPLET_IN_MOVE` | signed step | turning, or up/down; L/R page by 8 |
  | `APPLET_IN_CONFIRM` | | the encoder button, or A |
  | `APPLET_IN_BACK` | | a hold on the encoder, or B |
  | `APPLET_IN_TAB` | +1 or -1 | left/right on the five-way |
  | `APPLET_IN_MARK` | | Select |
  | `APPLET_IN_MENU` | | Start, or a hold on A without one |

  The physical buttons behind them depend on the board and on Settings →
  input, and the applet never needs to know which. The last three arrive on
  any board with those buttons. List the ones you use in `.controls`, as
  `CTRL_BIT()`s from `controls.h`: that decides the long presses, so a
  two-switch board treats a hold on A as Menu only for an applet that asked
  for Menu. An encoder has only turn, click and hold, so there only the
  first three ever arrive.

Everything runs on the UI task. Don't block in a callback: a flash write, a
long transfer or a `vTaskDelay` freezes the screen and the buttons. Anything
slow belongs in a service the host provides.

## 6. Testing it

Applets are portable C, so they run on your computer. The pattern from
`test/test_applets.c`:

```c
applet_env_t env = { .hint = "hold back" };
applet_bind_env(&env);

check("enters", applet_hello.enter(&env), 1);
applet_hello.input(&env, APPLET_IN_CONFIRM, 0);

ui_page_t p;
ui_page_init(&p, applet_hello.name, env.hint);
applet_hello.page(&env, &p);
check("counted", strcmp(p.line[1], "pressed 1 times"), 0);
```

To see it, render it into a framebuffer. `applet_render(&applet_hello, &env,
&canvas)` draws an applet exactly as the device does, and `make screens` adds
every listed applet to `build/screens/gallery.png`, next to the built-in
screens.

To run it through the real menu and dispatcher, `host_init()` and
`host_choose()` in `test/test_applets.c` do in about 20 lines what the
firmware does at boot.

## How it joins the firmware

You don't need this to write an applet. It's here for changing the layer
itself.

- `src/applet.c` collects `applets.def` into a list. It skips invalid
  entries, and gives the applet with index *n* the mode number
  `APP_MODE_APPLET_FIRST + n`.
- `applet_add_to_shell()` puts them in the menu, using
  `shell_add_entry()` in `src/ui_shell.c`.
- `applet_handlers()` makes one dispatcher handler per applet. One set of
  adapter functions serves them all, told which applet by the handler's
  `ctx`.
- `pio/src/gate_app.c` routes `APP_ROUTE_APPLET` to `render_applet()`. That
  function maps the encoder and buttons to `applet_input_t`, refreshes
  `env`, and draws.
