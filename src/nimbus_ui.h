/*
 * nimbus_ui.h — the small set of UI widgets Nimbus needs, reimplemented on
 * Catastrophe. This replaces PakKit: it keeps PakKit's public signatures (so the
 * app's call sites are unchanged) but draws entirely with Catastrophe primitives,
 * so the look is Leaf-native. The genuinely reusable pieces (message/confirm)
 * are candidates to promote into Catastrophe proper later (PLAN.md, Phase 3).
 *
 *   #define NIMBUS_UI_IMPLEMENTATION
 *   #include "nimbus_ui.h"   // after catastrophe.h
 */
#ifndef NIMBUS_UI_H
#define NIMBUS_UI_H

#include "catastrophe.h"
#include <SDL2/SDL.h>
#include <string.h>

#ifndef PAKKIT_SCROLL_STEP
#define PAKKIT_SCROLL_STEP 85
#endif
#ifndef PAKKIT_MAX_MENU_ITEMS
#define PAKKIT_MAX_MENU_ITEMS 12
#endif
#ifndef PAKKIT_MAX_LIST_ITEMS
#define PAKKIT_MAX_LIST_ITEMS 64
#endif
#ifndef PAKKIT_KB_MAX_TEXT
#define PAKKIT_KB_MAX_TEXT 512
#endif

/* --- Hints / footer --- */
typedef struct { const char *button; const char *label; } pakkit_hint;
void pakkit_draw_hints(pakkit_hint *hints, int count);

/* --- Transient screens --- */
void pakkit_loading(const char *message);
void pakkit_message(const char *message, const char *button_label);
int  pakkit_confirm(const char *message, const char *confirm_label, const char *cancel_label);

/* --- Menu --- */
typedef struct { const char *label; } pakkit_menu_item;
typedef struct { int selected_index; /* -1 if cancelled */ } pakkit_menu_result;
int pakkit_menu(const char *title, pakkit_menu_item *items, int count, pakkit_menu_result *result);

/* --- List --- */
typedef enum {
    PAKKIT_ACTION_SELECTED, PAKKIT_ACTION_BACK,
    PAKKIT_ACTION_SECONDARY, PAKKIT_ACTION_TERTIARY,
} pakkit_action;
typedef struct { const char *label; } pakkit_list_item;
typedef struct {
    const char  *title;
    pakkit_hint *hints;
    int          hint_count;
    cat_button   secondary_button;  /* CAT_BTN_NONE to disable */
    cat_button   tertiary_button;   /* CAT_BTN_NONE to disable */
    int          initial_index;
} pakkit_list_opts;
typedef struct { int selected_index; pakkit_action action; } pakkit_list_result;
int pakkit_list(pakkit_list_opts *opts, pakkit_list_item *items, int count, pakkit_list_result *result);

/* --- Detail / About --- */
typedef struct { const char *key; const char *value; } pakkit_info_pair;
typedef struct {
    const char       *title;
    const char       *subtitle;
    pakkit_info_pair *info;
    int               info_count;
    const char      **credits;
    int               credit_count;
} pakkit_detail_opts;
void pakkit_detail_screen(pakkit_detail_opts *opts);

/* --- Scroll helper --- */
typedef struct { int scroll_y; int target_scroll_y; int last_max_scroll; } pakkit_scroll_state;
void pakkit_scroll_handle_input(pakkit_scroll_state *s, int direction, int step);
void pakkit_scroll_animate(pakkit_scroll_state *s);
void pakkit_scroll_update(pakkit_scroll_state *s, int content_height, int viewport_height);

/* --- Keyboard --- */
typedef struct { char text[PAKKIT_KB_MAX_TEXT]; } pakkit_keyboard_result;
typedef struct {
    const char  *prompt;
    const char **shortcuts;
    int          shortcut_count;
} pakkit_keyboard_opts;
int pakkit_keyboard(const char *initial_text, pakkit_keyboard_opts *opts, pakkit_keyboard_result *result);

/* ===================================================================== */
#ifdef NIMBUS_UI_IMPLEMENTATION

/* Map a hint button string ("A"/"B"/"L1"/...) to a Catastrophe button so the
   footer renders the native glyph; unknown strings fall back to text. */
static cat_button nui__btn_from_str(const char *s) {
    if (!s) return CAT_BTN_NONE;
    if (!strcmp(s, "A")) return CAT_BTN_A;
    if (!strcmp(s, "B")) return CAT_BTN_B;
    if (!strcmp(s, "X")) return CAT_BTN_X;
    if (!strcmp(s, "Y")) return CAT_BTN_Y;
    if (!strcmp(s, "L1") || !strcmp(s, "L")) return CAT_BTN_L1;
    if (!strcmp(s, "R1") || !strcmp(s, "R")) return CAT_BTN_R1;
    if (!strcmp(s, "START")) return CAT_BTN_START;
    if (!strcmp(s, "SELECT")) return CAT_BTN_SELECT;
    if (!strcmp(s, "MENU")) return CAT_BTN_MENU;
    return CAT_BTN_NONE;
}

void pakkit_draw_hints(pakkit_hint *hints, int count) {
    if (!hints || count <= 0) return;
    cat_footer_item items[8];
    if (count > 8) count = 8;
    for (int i = 0; i < count; i++) {
        cat_button b = nui__btn_from_str(hints[i].button);
        items[i].button = b;
        items[i].label = hints[i].label;
        items[i].is_confirm = false;
        items[i].button_text = (b == CAT_BTN_NONE) ? hints[i].button : NULL;
    }
    cat_draw_footer(items, count);
}

/* Pump input + present one frame; returns the first pressed button (or CAT_BTN_NONE). */
static cat_button nui__poll_pressed(void) {
    cat_input_event ev;
    cat_button hit = CAT_BTN_NONE;
    while (cat_poll_input(&ev)) {
        if (ev.pressed && hit == CAT_BTN_NONE) hit = ev.button;
    }
    return hit;
}

static void nui__begin_frame(void) { cat_clear_screen(); cat_draw_background(); }
static void nui__end_frame(void) { cat_present(); SDL_Delay(16); }

void pakkit_loading(const char *message) {
    cat_theme *th = cat_get_theme();
    TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    nui__begin_frame();
    if (message) {
        int w = cat_measure_text(f, message);
        cat_draw_text(f, message, (sw - w) / 2, sh / 2 - TTF_FontHeight(f) / 2, th->text);
    }
    cat_present();
}

void pakkit_message(const char *message, const char *button_label) {
    cat_theme *th = cat_get_theme();
    TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    pakkit_hint hints[] = { { "A", button_label ? button_label : "OK" } };
    for (;;) {
        cat_button b = nui__poll_pressed();
        if (b == CAT_BTN_A || b == CAT_BTN_B) return;
        nui__begin_frame();
        int mh = message ? cat_measure_wrapped_text_height(f, message, sw - CAT_DS(60)) : 0;
        cat_draw_text_wrapped(f, message ? message : "", sw / 2, sh / 2 - mh / 2,
                              sw - CAT_DS(60), th->text, CAT_ALIGN_CENTER);
        pakkit_draw_hints(hints, 1);
        nui__end_frame();
    }
}

int pakkit_confirm(const char *message, const char *confirm_label, const char *cancel_label) {
    cat_theme *th = cat_get_theme();
    TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    pakkit_hint hints[] = {
        { "A", confirm_label ? confirm_label : "Confirm" },
        { "B", cancel_label ? cancel_label : "Cancel" },
    };
    for (;;) {
        cat_button b = nui__poll_pressed();
        if (b == CAT_BTN_A) return 1;
        if (b == CAT_BTN_B) return 0;
        nui__begin_frame();
        int mh = message ? cat_measure_wrapped_text_height(f, message, sw - CAT_DS(60)) : 0;
        cat_draw_text_wrapped(f, message ? message : "", sw / 2, sh / 2 - mh / 2,
                              sw - CAT_DS(60), th->text, CAT_ALIGN_CENTER);
        pakkit_draw_hints(hints, 2);
        nui__end_frame();
    }
}

/* Shared vertical-list runner used by menu + list. Returns the action; *out_idx
   gets the selected row (or -1). */
static pakkit_action nui__run_list(const char *title, const char *const *labels, int count,
                                   int initial_index, cat_button secondary, cat_button tertiary,
                                   pakkit_hint *extra_hints, int extra_count, int *out_idx) {
    cat_theme *th = cat_get_theme();
    TTF_Font *title_f = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *row_f = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    int cursor = (initial_index >= 0 && initial_index < count) ? initial_index : 0;
    int row_h = TTF_FontHeight(row_f) + CAT_DS(14);
    int top = CAT_DS(20) + (title ? TTF_FontHeight(title_f) + CAT_DS(16) : 0);
    int footer_h = CAT_DS(48);
    int visible = (sh - top - footer_h) / row_h;
    if (visible < 1) visible = 1;
    int scroll = 0;

    pakkit_hint hints[8]; int hc = 0;
    hints[hc++] = (pakkit_hint){ "A", "Select" };
    hints[hc++] = (pakkit_hint){ "B", "Back" };
    for (int i = 0; i < extra_count && hc < 8; i++) hints[hc++] = extra_hints[i];

    for (;;) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            switch (ev.button) {
                case CAT_BTN_UP:   cursor = (cursor - 1 + count) % count; break;
                case CAT_BTN_DOWN: cursor = (cursor + 1) % count; break;
                case CAT_BTN_A:    *out_idx = count ? cursor : -1; return PAKKIT_ACTION_SELECTED;
                case CAT_BTN_B:    *out_idx = -1; return PAKKIT_ACTION_BACK;
                default:
                    if (secondary != CAT_BTN_NONE && ev.button == secondary) {
                        *out_idx = count ? cursor : -1; return PAKKIT_ACTION_SECONDARY;
                    }
                    if (tertiary != CAT_BTN_NONE && ev.button == tertiary) {
                        *out_idx = count ? cursor : -1; return PAKKIT_ACTION_TERTIARY;
                    }
                    break;
            }
        }
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + visible) scroll = cursor - visible + 1;

        nui__begin_frame();
        if (title) cat_draw_text(title_f, title, CAT_DS(24), CAT_DS(20), th->text);
        for (int i = 0; i < visible && scroll + i < count; i++) {
            int idx = scroll + i;
            int y = top + i * row_h;
            bool sel = (idx == cursor);
            if (sel) cat_draw_rounded_rect(CAT_DS(12), y, sw - CAT_DS(24), row_h - CAT_DS(4),
                                           CAT_DS(8), th->highlight);
            cat_draw_text_ellipsized(row_f, labels[idx], CAT_DS(24), y + CAT_DS(5),
                                     sel ? th->highlighted_text : th->text, sw - CAT_DS(48));
        }
        pakkit_draw_hints(hints, hc);
        nui__end_frame();
    }
}

int pakkit_menu(const char *title, pakkit_menu_item *items, int count, pakkit_menu_result *result) {
    const char *labels[PAKKIT_MAX_MENU_ITEMS];
    if (count > PAKKIT_MAX_MENU_ITEMS) count = PAKKIT_MAX_MENU_ITEMS;
    for (int i = 0; i < count; i++) labels[i] = items[i].label;
    int idx = -1;
    pakkit_action a = nui__run_list(title, labels, count, 0, CAT_BTN_NONE, CAT_BTN_NONE,
                                    NULL, 0, &idx);
    if (result) result->selected_index = (a == PAKKIT_ACTION_SELECTED) ? idx : -1;
    return 0;
}

int pakkit_list(pakkit_list_opts *opts, pakkit_list_item *items, int count, pakkit_list_result *result) {
    static const char *labels[PAKKIT_MAX_LIST_ITEMS];
    if (count > PAKKIT_MAX_LIST_ITEMS) count = PAKKIT_MAX_LIST_ITEMS;
    for (int i = 0; i < count; i++) labels[i] = items[i].label;
    int idx = -1;
    pakkit_action a = nui__run_list(opts ? opts->title : NULL, labels, count,
                                    opts ? opts->initial_index : 0,
                                    opts ? opts->secondary_button : CAT_BTN_NONE,
                                    opts ? opts->tertiary_button : CAT_BTN_NONE,
                                    opts ? opts->hints : NULL, opts ? opts->hint_count : 0, &idx);
    if (result) { result->selected_index = idx; result->action = a; }
    return 0;
}

void pakkit_scroll_handle_input(pakkit_scroll_state *s, int direction, int step) {
    if (!s) return;
    s->target_scroll_y += direction * step;
    if (s->target_scroll_y < 0) s->target_scroll_y = 0;
    if (s->target_scroll_y > s->last_max_scroll) s->target_scroll_y = s->last_max_scroll;
}
void pakkit_scroll_animate(pakkit_scroll_state *s) {
    if (!s) return;
    int d = s->target_scroll_y - s->scroll_y;
    if (d > -2 && d < 2) { s->scroll_y = s->target_scroll_y; return; }
    s->scroll_y += d * 15 / 100;  /* ~0.15 ease toward target */
}
void pakkit_scroll_update(pakkit_scroll_state *s, int content_height, int viewport_height) {
    if (!s) return;
    int maxs = content_height - viewport_height;
    if (maxs < 0) maxs = 0;
    s->last_max_scroll = maxs;
    if (s->target_scroll_y > maxs) s->target_scroll_y = maxs;
    if (s->scroll_y > maxs) s->scroll_y = maxs;
    if (s->scroll_y < 0) s->scroll_y = 0;
}

void pakkit_detail_screen(pakkit_detail_opts *opts) {
    if (!opts) return;
    cat_theme *th = cat_get_theme();
    TTF_Font *tf = cat_get_font(CAT_FONT_LARGE);
    TTF_Font *sf = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *bf = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    pakkit_hint hints[] = { { "B", "Back" } };
    pakkit_scroll_state sc = {0};
    int line = TTF_FontHeight(bf) + CAT_DS(6);
    int footer_h = CAT_DS(48);

    for (;;) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            if (ev.button == CAT_BTN_B || ev.button == CAT_BTN_A) return;
            if (ev.button == CAT_BTN_UP)   pakkit_scroll_handle_input(&sc, -1, PAKKIT_SCROLL_STEP);
            if (ev.button == CAT_BTN_DOWN) pakkit_scroll_handle_input(&sc, +1, PAKKIT_SCROLL_STEP);
        }
        pakkit_scroll_animate(&sc);

        /* Content height: title + subtitle + info pairs + credits. */
        int content = TTF_FontHeight(tf) + CAT_DS(10);
        if (opts->subtitle) content += TTF_FontHeight(sf) + CAT_DS(12);
        content += opts->info_count * line + CAT_DS(10);
        content += opts->credit_count * (TTF_FontHeight(sf) + CAT_DS(2));
        pakkit_scroll_update(&sc, content, sh - footer_h - CAT_DS(20));

        nui__begin_frame();
        int x = CAT_DS(24);
        int y = CAT_DS(20) - sc.scroll_y;
        cat_draw_text(tf, opts->title ? opts->title : "", x, y, th->text);
        y += TTF_FontHeight(tf) + CAT_DS(10);
        if (opts->subtitle) { cat_draw_text(sf, opts->subtitle, x, y, th->hint);
                              y += TTF_FontHeight(sf) + CAT_DS(12); }
        for (int i = 0; i < opts->info_count; i++) {
            cat_draw_text(bf, opts->info[i].key, x, y, th->hint);
            int kw = cat_measure_text(bf, opts->info[i].key);
            cat_draw_text(bf, opts->info[i].value, x + kw + CAT_DS(10), y, th->text);
            y += line;
        }
        y += CAT_DS(10);
        for (int i = 0; i < opts->credit_count; i++) {
            cat_draw_text(sf, opts->credits[i], x, y, th->hint);
            y += TTF_FontHeight(sf) + CAT_DS(2);
        }
        pakkit_draw_hints(hints, 1);
        nui__end_frame();
    }
}

/* Phase 1 stub: the on-screen keyboard is wired to Catastrophe's keyboard in a
   later phase (PLAN.md). For now it cancels so location-search no-ops cleanly. */
int pakkit_keyboard(const char *initial_text, pakkit_keyboard_opts *opts, pakkit_keyboard_result *result) {
    (void)opts;
    if (result) {
        result->text[0] = '\0';
        if (initial_text) snprintf(result->text, sizeof(result->text), "%s", initial_text);
    }
    return -1; /* cancelled — TODO: Catastrophe on-screen keyboard */
}

#endif /* NIMBUS_UI_IMPLEMENTATION */
#endif /* NIMBUS_UI_H */
