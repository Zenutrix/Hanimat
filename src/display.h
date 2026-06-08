#pragma once
// =================================================================
//  DISPLAY MODULE — LVGL 8.3.x
//  Replaces Adafruit GFX direct-draw with LVGL widget UI.
//  320x240 ILI9341 landscape. Included as single translation unit.
// =================================================================
#include <lvgl.h>

// ─── HAL: FLUSH CALLBACK & BUFFER ─────────────────────────────────
// Buffer in internal SRAM (ESP32 DMA cannot address PSRAM)
static lv_color_t         lv_buf1[320 * 24];   // ~15 KB
static lv_disp_draw_buf_t lv_draw_buf;
static lv_disp_drv_t      lv_disp_drv_s;

static void lv_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // LV_COLOR_16_SWAP=1 → data is already byte-swapped for ILI9341 big-endian SPI
    tft.writePixels((uint16_t *)color_p, w * h, true, LV_COLOR_16_SWAP);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// ─── FONT ALIASES ─────────────────────────────────────────────────
#define FBOLD (&lv_font_montserrat_32) // bold — fach/slot number
#define FXL (&lv_font_montserrat_28)  // x-large — credit amount
#define FL  (&lv_font_montserrat_24)  // large  — titles, HANIMAT header
#define FM  (&lv_font_montserrat_16)  // medium — body text, slogan
#define FS  (&lv_font_montserrat_12)  // small  — labels, footer URL
#define FXS (&lv_font_montserrat_10)  // x-small — unused (kept for compat)

// ─── THEME COLORS ─────────────────────────────────────────────────
static lv_color_t C_BG, C_HEADER, C_TEXT, C_ACCENT,
                  C_SUCCESS, C_ERROR, C_INFO, C_DIVIDER, C_CARD;

static void _applyColors() {
    if (displayWhiteMode) {
        C_BG      = lv_color_hex(0xFFFFFF);
        C_HEADER  = lv_color_hex(0xFFD600);  // yellow
        C_TEXT    = lv_color_hex(0x000000);
        C_ACCENT  = lv_color_hex(0xE65100);  // dark orange
        C_SUCCESS = lv_color_hex(0x00C853);  // juicy green
        C_ERROR   = lv_color_hex(0xB71C1C);  // dark red
        C_INFO    = lv_color_hex(0x0D47A1);  // dark blue
        C_DIVIDER = lv_color_hex(0x9E9E9E);  // medium grey
        C_CARD    = lv_color_hex(0xE0E0E0);  // light grey cards
    } else {
        C_BG      = lv_color_hex(0x000000);
        C_HEADER  = lv_color_hex(0xFFE000);  // yellow
        C_TEXT    = lv_color_hex(0xFFFFFF);
        C_ACCENT  = lv_color_hex(0xFF6400);  // orange
        C_SUCCESS = lv_color_hex(0x00FF00);  // green
        C_ERROR   = lv_color_hex(0xFF0000);  // red
        C_INFO    = lv_color_hex(0x00FFFF);  // cyan
        C_DIVIDER = lv_color_hex(0x7B7B7B);  // medium grey
        C_CARD    = lv_color_hex(0x1C1C1C);  // dark grey cards
    }
}

// ─── SCREEN POINTERS ──────────────────────────────────────────────
static lv_obj_t *scr_main;      // IDLE / KEYPAD / SLOT states
static lv_obj_t *scr_dispense;  // DISPENSING state
static lv_obj_t *scr_error;     // Error overlay
static lv_obj_t *scr_ota;       // OTA update progress
static lv_obj_t *scr_wifi_setup;// WiFiManager portal
static lv_obj_t *scr_startup;   // Boot splash
static lv_obj_t *scr_offline;   // Offline/AP mode
static lv_obj_t *scr_wifi_info; // WiFi connected info
static lv_obj_t *scr_reset;     // System reset confirm

// ─── WIDGET POINTERS (scr_main) ───────────────────────────────────
// Header zone (y=0..50)
static lv_obj_t *m_title;
static lv_obj_t *m_wifi_dot;
static lv_obj_t *m_hdiv;
// Credit zone (y=50..100)
static lv_obj_t *m_credit_lbl;   // "GUTHABEN"
static lv_obj_t *m_credit_val;   // "5.00 EUR"
static lv_obj_t *m_no_credit;    // "Kein Guthaben"
static lv_obj_t *m_cdiv;         // divider below credit zone
// Footer (y=215..240)
static lv_obj_t *m_slogan;
static lv_obj_t *m_footer_url;
// Status line (y=188..215, visible in SLOT state)
static lv_obj_t *m_status;
// Dynamic panels (y=100..188)
static lv_obj_t *p_idle;
static lv_obj_t *p_keypad;
static lv_obj_t *p_slot;
// idle panel
static lv_obj_t *pi_main;
static lv_obj_t *pi_sub;
// keypad panel
static lv_obj_t *pk_div;
static lv_obj_t *pk_card;
static lv_obj_t *pk_fach;
static lv_obj_t *pk_input;
static lv_obj_t *pk_hint;
// slot panel
static lv_obj_t *ps_card;
static lv_obj_t *ps_fach_lbl;
static lv_obj_t *ps_num;
static lv_obj_t *ps_sub_lbl;
static lv_obj_t *ps_sub_val;

// ─── WIDGET POINTERS (scr_dispense) ───────────────────────────────
static lv_obj_t *d_hdiv;
static lv_obj_t *d_slot_info;
static lv_obj_t *d_msg;
static lv_obj_t *d_run;
static lv_obj_t *d_bar;

// ─── WIDGET POINTERS (other screens) ──────────────────────────────
static lv_obj_t *e_hdr, *e_card, *e_l1, *e_l2;
static lv_obj_t *ota_l1, *ota_l2, *ota_l3, *ota_bar, *ota_pct;
static lv_obj_t *ws_warn, *ws_ssid, *ws_pw, *ws_ip;
static lv_obj_t *of_mode, *of_ap, *of_ip;
static lv_obj_t *wi_conn, *wi_ip, *wi_ver, *wi_by;
static lv_obj_t *rs_info, *rs_confirm;

// ─── INTERNAL HELPERS ─────────────────────────────────────────────

static lv_obj_t *_mkScr(lv_color_t bg) {
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, bg, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_set_scroll_dir(s, LV_DIR_NONE);
    return s;
}

static lv_obj_t *_lbl(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c, int x, int y) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *_clbl(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c, int y) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *_hline(lv_obj_t *p, lv_color_t c, int x, int y, int w) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 1);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_NONE);
    return o;
}

static lv_obj_t *_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, 0, 100);
    lv_obj_set_size(p, 320, 88);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_NONE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *_card(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg, lv_color_t brd) {
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, brd, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_NONE);
    return c;
}

static void _hide3(lv_obj_t *a, lv_obj_t *b, lv_obj_t *c) {
    if (a) lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    if (b) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    if (c) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
}

// ─── SCREEN INITIALIZERS ──────────────────────────────────────────

static void _initMainScr() {
    scr_main = _mkScr(C_BG);

    // Header (y=0..50): "HANIMAT" centered, WiFi dot top-right, divider at y=48
    m_title    = _clbl(scr_main, "HANIMAT", FL, C_HEADER, 8);
    m_hdiv     = _hline(scr_main, C_DIVIDER, 10, 48, 300);

    m_wifi_dot = lv_obj_create(scr_main);
    lv_obj_set_size(m_wifi_dot, 14, 14);
    lv_obj_set_style_radius(m_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(m_wifi_dot, C_SUCCESS, 0);
    lv_obj_set_style_bg_opa(m_wifi_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_wifi_dot, 0, 0);
    lv_obj_set_style_pad_all(m_wifi_dot, 0, 0);
    lv_obj_align(m_wifi_dot, LV_ALIGN_TOP_RIGHT, -8, 17);

    // Credit zone (y=50..100)
    m_credit_lbl = _lbl(scr_main, "GUTHABEN", FS, C_DIVIDER, 10, 56);
    lv_obj_add_flag(m_credit_lbl, LV_OBJ_FLAG_HIDDEN);

    m_credit_val = _clbl(scr_main, "0.00 EUR", FXL, C_SUCCESS, 60);
    lv_obj_add_flag(m_credit_val, LV_OBJ_FLAG_HIDDEN);

    m_no_credit = _clbl(scr_main, "Kein Guthaben", FM, C_DIVIDER, 62);
    m_cdiv      = _hline(scr_main, C_DIVIDER, 10, 96, 300);

    // Status line — visible only when slot selected
    m_status = _clbl(scr_main, "", FM, C_SUCCESS, 176);
    lv_obj_add_flag(m_status, LV_OBJ_FLAG_HIDDEN);

    // Footer
    m_slogan     = _clbl(scr_main, "", FM, C_TEXT, 200);
    lv_obj_add_flag(m_slogan, LV_OBJ_FLAG_HIDDEN);
    m_footer_url = _clbl(scr_main, "www.hanimat.at", FM, C_HEADER, 222);

    // ── Dynamic panels (y=100..188, one shown at a time) ──────────

    // IDLE panel
    p_idle = _panel(scr_main);
    pi_main = _clbl(p_idle, "Geld einwerfen",       FM, C_TEXT,    30);
    pi_sub  = _clbl(p_idle, "oder Fach 1-N waehlen", FM, C_DIVIDER, 50);

    // KEYPAD panel
    p_keypad = _panel(scr_main);
    lv_obj_set_size(p_keypad, 320, 92);
    pk_div   = _hline(p_keypad, C_DIVIDER, 10, 12, 300);
    pk_card  = _card(p_keypad, 8, 18, 304, 58, C_CARD, C_HEADER);
    pk_fach  = _lbl(pk_card,  "FACH", FS, C_DIVIDER, 14, 8);
    pk_input = _lbl(pk_card,  "1_",   FXL, C_HEADER,  14, 18);
    pk_hint  = _clbl(p_keypad, "2. Ziffer oder # bestaetigen", FM, C_DIVIDER, 76);

    // SLOT panel
    p_slot      = _panel(scr_main);
    lv_obj_set_size(p_slot, 320, 72);
    ps_card     = _card(p_slot, 8, 4, 304, 62, C_CARD, C_ACCENT);

    ps_fach_lbl = _lbl(ps_card, "FACH", FS, C_DIVIDER, 4, 4);
    lv_obj_set_width(ps_fach_lbl, 144);
    lv_obj_set_style_text_align(ps_fach_lbl, LV_TEXT_ALIGN_CENTER, 0);

    ps_num = _lbl(ps_card, "1", FBOLD, lv_color_hex(0xFFE000), 4, 18);
    lv_obj_set_width(ps_num, 144);
    lv_obj_set_style_text_align(ps_num, LV_TEXT_ALIGN_CENTER, 0);

    ps_sub_lbl = _lbl(ps_card, "PREIS", FS, C_DIVIDER, 156, 4);
    lv_obj_set_width(ps_sub_lbl, 144);
    lv_obj_set_style_text_align(ps_sub_lbl, LV_TEXT_ALIGN_CENTER, 0);

    ps_sub_val = _lbl(ps_card, "5.00 EUR", FL, C_ACCENT, 156, 18);
    lv_obj_set_width(ps_sub_val, 144);
    lv_obj_set_style_text_align(ps_sub_val, LV_TEXT_ALIGN_CENTER, 0);
}

static void _initDispenseScr() {
    scr_dispense = _mkScr(C_BG);

    // Title "VIELEN DANK" in green, centered
    _clbl(scr_dispense, "VIELEN DANK", FL, C_SUCCESS, 8);
    _hline(scr_dispense, C_DIVIDER, 10, 48, 300);

    d_slot_info = _clbl(scr_dispense, "Fach #1  5.00 EUR", FS, C_DIVIDER, 56);
    d_msg       = _clbl(scr_dispense, "Bitte Produkt entnehmen", FM, C_TEXT, 76);
    d_run       = _clbl(scr_dispense, "Ausgabe lauft...", FS, C_DIVIDER, 104);

    // Progress bar (y=120..142)
    d_bar = lv_bar_create(scr_dispense);
    lv_obj_set_pos(d_bar, 10, 120);
    lv_obj_set_size(d_bar, 300, 22);
    lv_bar_set_range(d_bar, 0, 100);
    lv_bar_set_value(d_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(d_bar, C_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d_bar,  LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d_bar, C_SUCCESS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(d_bar,  LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(d_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(d_bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(d_bar, C_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d_bar, 1, LV_PART_MAIN);
}

static void _initErrorScr() {
    scr_error = _mkScr(C_BG);
    e_hdr = _clbl(scr_error, "HINWEIS", FL, C_ACCENT, 8);
    _hline(scr_error, C_DIVIDER, 10, 48, 300);

    e_card = _card(scr_error, 10, 58, 300, 110, C_CARD, C_ERROR);
    // Red top-bar accent strip on card
    lv_obj_t *strip = lv_obj_create(e_card);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_size(strip, 300, 6);
    lv_obj_set_style_bg_color(strip, C_ERROR, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_style_radius(strip, 0, 0);
    lv_obj_set_scroll_dir(strip, LV_DIR_NONE);

    e_l1 = _clbl(e_card, "", FM, C_ERROR, 32);
    e_l2 = _clbl(e_card, "", FM, C_TEXT,  62);
}

static void _initOtaScr() {
    scr_ota = _mkScr(C_BG);
    _clbl(scr_ota, "SYSTEM UPDATE", FL, C_HEADER, 8);
    _hline(scr_ota, C_DIVIDER, 10, 48, 300);
    ota_l1 = _clbl(scr_ota, "", FM, C_TEXT, 80);
    ota_l2 = _clbl(scr_ota, "", FM, C_TEXT, 110);
    ota_l3 = _clbl(scr_ota, "", FS, C_DIVIDER, 140);

    ota_bar = lv_bar_create(scr_ota);
    lv_obj_set_pos(ota_bar, 10, 168);
    lv_obj_set_size(ota_bar, 300, 18);
    lv_bar_set_range(ota_bar, 0, 100);
    lv_bar_set_value(ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ota_bar, C_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ota_bar,  LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ota_bar,  LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ota_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(ota_bar, 5, LV_PART_INDICATOR);
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);

    ota_pct = _clbl(scr_ota, "", FS, C_TEXT, 196);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
}

static void _initWifiSetupScr() {
    scr_wifi_setup = _mkScr(C_BG);
    _clbl(scr_wifi_setup, "HANIMAT Setup", FL, C_HEADER, 8);
    _hline(scr_wifi_setup, C_DIVIDER, 10, 48, 300);
    ws_warn = _clbl(scr_wifi_setup, "WLAN nicht konfiguriert!", FM, C_ACCENT, 62);
    ws_ssid = _clbl(scr_wifi_setup, "", FM, C_TEXT, 96);
    ws_pw   = _clbl(scr_wifi_setup, "PW: Honig1234", FM, C_TEXT, 124);
    ws_ip   = _clbl(scr_wifi_setup, "IP: 192.168.4.1", FM, C_TEXT, 152);
}

static void _initStartupScr() {
    scr_startup = _mkScr(C_BG);
    _clbl(scr_startup, "HANIMAT",        FL, C_HEADER,  82);
    _clbl(scr_startup, "startet...",     FM, C_TEXT,    118);
    _clbl(scr_startup, FIRMWARE_VERSION, FS, C_DIVIDER, 150);
}

static void _initOfflineScr() {
    scr_offline = _mkScr(C_BG);
    of_mode = _clbl(scr_offline, "OFFLINE MODUS", FM, C_ACCENT, 30);
    of_ap   = _clbl(scr_offline, "AP: HANIMAT-Offline", FM, C_TEXT, 62);
    of_ip   = _clbl(scr_offline, "IP: 192.168.4.1", FM, C_TEXT, 90);
}

static void _initWifiInfoScr() {
    scr_wifi_info = _mkScr(C_BG);
    _clbl(scr_wifi_info, "INFO", FL, C_ACCENT, 8);
    _hline(scr_wifi_info, C_DIVIDER, 10, 48, 300);
    wi_conn = _clbl(scr_wifi_info, "WLAN verbunden", FM, C_SUCCESS, 68);
    wi_ip   = _clbl(scr_wifi_info, "", FM, C_TEXT,    96);
    wi_ver  = _clbl(scr_wifi_info, "", FM, C_INFO,    124);
    wi_by   = _clbl(scr_wifi_info, "By Hanimat | Thomas Schoepf", FS, C_DIVIDER, 158);
}

static void _initResetScr() {
    scr_reset = _mkScr(C_BG);
    _clbl(scr_reset, "SYSTEM RESET", FL, C_ACCENT, 8);
    _hline(scr_reset, C_DIVIDER, 10, 48, 300);
    rs_info    = _clbl(scr_reset, "Bestaetigen mit #", FM, C_TEXT, 96);
    rs_confirm = _clbl(scr_reset, "WERKSRESET...", FL, C_ERROR, 140);
    lv_obj_add_flag(rs_confirm, LV_OBJ_FLAG_HIDDEN);
}

// ─── APPLY THEME TO ALL SCREENS ───────────────────────────────────
// Called after _applyColors() to push new colors to every widget.
static void _recolorAll() {
    // Helpers
    auto sbc = [](lv_obj_t *o, lv_color_t c){ lv_obj_set_style_bg_color(o, c, 0); };
    auto stc = [](lv_obj_t *o, lv_color_t c){ lv_obj_set_style_text_color(o, c, 0); };
    auto sbc2= [](lv_obj_t *o, lv_color_t c, lv_style_selector_t sel){ lv_obj_set_style_bg_color(o, c, sel); };

    // ── scr_main ──
    sbc(scr_main,        C_BG);
    stc(m_title,         C_HEADER);
    sbc(m_hdiv,          C_DIVIDER);
    sbc(m_wifi_dot,      (WiFi.status() == WL_CONNECTED) ? C_SUCCESS : C_ERROR);
    stc(m_credit_lbl,    C_DIVIDER);
    stc(m_credit_val,    C_SUCCESS);
    stc(m_no_credit,     C_DIVIDER);
    sbc(m_cdiv,          C_DIVIDER);
    stc(m_status,        C_SUCCESS);
    stc(m_slogan,        C_TEXT);
    stc(m_footer_url,    C_HEADER);

    // idle panel
    stc(pi_main, C_TEXT);
    stc(pi_sub,  C_DIVIDER);

    // keypad panel
    sbc(pk_div,  C_DIVIDER);
    sbc(pk_card, C_CARD);
    lv_obj_set_style_border_color(pk_card, C_HEADER, 0);
    stc(pk_fach,  C_DIVIDER);
    stc(pk_input, C_HEADER);
    stc(pk_hint,  C_DIVIDER);

    // slot panel
    sbc(ps_card, C_CARD);
    lv_obj_set_style_border_color(ps_card, C_ACCENT, 0);
    stc(ps_fach_lbl, C_DIVIDER);
    // ps_num stays hardcoded #FFE000 (bright yellow) — not theme-dependent
    stc(ps_sub_lbl,  C_DIVIDER);
    stc(ps_sub_val,  C_ACCENT);

    // ── scr_dispense ──
    sbc(scr_dispense, C_BG);
    stc(d_slot_info,  C_DIVIDER);
    stc(d_msg,        C_TEXT);
    stc(d_run,        C_DIVIDER);
    sbc2(d_bar, C_DIVIDER, LV_PART_MAIN);
    sbc2(d_bar, C_SUCCESS, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(d_bar, C_DIVIDER, LV_PART_MAIN);

    // ── scr_error ──
    sbc(scr_error, C_BG);
    stc(e_hdr,     C_ACCENT);
    sbc(e_card,    C_CARD);
    lv_obj_set_style_border_color(e_card, C_ERROR, 0);
    stc(e_l1, C_ERROR);
    stc(e_l2, C_TEXT);

    // ── scr_ota ──
    sbc(scr_ota, C_BG);
    stc(ota_l1, C_TEXT);
    stc(ota_l2, C_TEXT);
    stc(ota_l3, C_DIVIDER);
    stc(ota_pct, C_TEXT);
    lv_obj_set_style_bg_color(ota_bar, C_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_bar, C_ACCENT,  LV_PART_INDICATOR);

    // ── scr_wifi_setup ──
    sbc(scr_wifi_setup, C_BG);
    stc(ws_warn, C_ACCENT);
    stc(ws_ssid, C_TEXT);
    stc(ws_pw,   C_TEXT);
    stc(ws_ip,   C_TEXT);

    // ── scr_startup ──
    sbc(scr_startup, C_BG);

    // ── scr_offline ──
    sbc(scr_offline, C_BG);
    stc(of_mode, C_ACCENT);
    stc(of_ap,   C_TEXT);
    stc(of_ip,   C_TEXT);

    // ── scr_wifi_info ──
    sbc(scr_wifi_info, C_BG);
    stc(wi_conn, C_SUCCESS);
    stc(wi_ip,   C_TEXT);
    stc(wi_ver,  C_INFO);
    stc(wi_by,   C_DIVIDER);

    // ── scr_reset ──
    sbc(scr_reset, C_BG);
    stc(rs_info,    C_TEXT);
    stc(rs_confirm, C_ERROR);

    // Force redraw of current screen
    lv_obj_invalidate(lv_scr_act());
}

// ─── PUBLIC: INIT ─────────────────────────────────────────────────
void initLVGL() {
    _applyColors();

    lv_init();
    lv_disp_draw_buf_init(&lv_draw_buf, lv_buf1, NULL, 320 * 24);
    lv_disp_drv_init(&lv_disp_drv_s);
    lv_disp_drv_s.hor_res  = 320;
    lv_disp_drv_s.ver_res  = 240;
    lv_disp_drv_s.flush_cb = lv_flush_cb;
    lv_disp_drv_s.draw_buf = &lv_draw_buf;
    lv_disp_drv_register(&lv_disp_drv_s);

    _initMainScr();
    _initDispenseScr();
    _initErrorScr();
    _initOtaScr();
    _initWifiSetupScr();
    _initStartupScr();
    _initOfflineScr();
    _initWifiInfoScr();
    _initResetScr();
}

// ─── PUBLIC: APPLY THEME ──────────────────────────────────────────
// Call after changing displayWhiteMode to push colors to all widgets.
void applyLVGLTheme() {
    _applyColors();
    _recolorAll();
    lastDrawnMode = DrawnMode::NONE;
    displayNeedsUpdate = true;
}

// ─── PUBLIC: STARTUP SCREEN ───────────────────────────────────────
void displayStartupScreen() {
    lv_scr_load(scr_startup);
    lv_timer_handler();
}

// ─── PUBLIC: OFFLINE MODE SCREEN ──────────────────────────────────
void displayOfflineModeScreen(const String &ip) {
    char buf[64];
    snprintf(buf, sizeof(buf), "IP: %s", ip.c_str());
    lv_label_set_text(of_ip, buf);
    lv_scr_load(scr_offline);
    lv_timer_handler();
}

// ─── PUBLIC: WIFI CONNECTED INFO SCREEN ───────────────────────────
void displayWifiConnectedScreen(const String &ip, const String &version) {
    char buf[64];
    snprintf(buf, sizeof(buf), "IP: %s", ip.c_str());
    lv_label_set_text(wi_ip, buf);
    snprintf(buf, sizeof(buf), "Firmware %s", version.c_str());
    lv_label_set_text(wi_ver, buf);
    lv_scr_load(scr_wifi_info);
    lv_timer_handler();
}

// ─── PUBLIC: SYSTEM RESET SCREEN ──────────────────────────────────
void displaySystemResetScreen(bool confirmed) {
    if (confirmed) {
        lv_obj_clear_flag(rs_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rs_info, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(rs_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(rs_info, LV_OBJ_FLAG_HIDDEN);
    }
    lv_scr_load(scr_reset);
    lv_timer_handler();
}

// ─── PUBLIC: WIIFMANAGER CALLBACK ─────────────────────────────────
void configModeCallback(WiFiManager *myWiFiManager) {
    logMessage("Kein WLAN gefunden. Setup-Portal gestartet.");
    lastDrawnMode = DrawnMode::NONE;
    char buf[64];
    snprintf(buf, sizeof(buf), "WLAN SSID: %s", myWiFiManager->getConfigPortalSSID().c_str());
    lv_label_set_text(ws_ssid, buf);
    lv_scr_load(scr_wifi_setup);
    lv_timer_handler();
    ledcWriteTone(0, 1500);
    delay(200);
    ledcWriteTone(0, 0);
}

// ─── PUBLIC: OTA SCREEN ───────────────────────────────────────────
void displayOTAMessageTFT(String line1, String line2, String line3, uint16_t /*color*/) {
    lastDrawnMode = DrawnMode::NONE;
    lv_label_set_text(ota_l1, line1.c_str());
    lv_label_set_text(ota_l2, line2.c_str());
    lv_label_set_text(ota_l3, line3.c_str());
    lv_bar_set_value(ota_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(scr_ota);
    lv_timer_handler();
}

void displayOTAProgressTFT(int pct) {
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    lv_bar_set_value(ota_bar, pct, LV_ANIM_OFF);
    lv_obj_clear_flag(ota_bar, LV_OBJ_FLAG_HIDDEN);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(ota_pct, buf);
    lv_obj_clear_flag(ota_pct, LV_OBJ_FLAG_HIDDEN);
    lv_timer_handler();
}

// ─── PUBLIC: ERROR SCREEN ─────────────────────────────────────────
void displayErrorMessage(const String &line1, const String &line2) {
    logMessage("Display Error: " + line1 + (line2.length() > 0 ? " | " + line2 : ""));
    currentSystemState = CurrentSystemState::ERROR_DISPLAY;
    lv_label_set_text(e_l1, line1.c_str());
    lv_label_set_text(e_l2, line2.c_str());
    lv_scr_load(scr_error);
    lv_timer_handler();
    playErrorSound();
    displayNeedsUpdate = false;
    lastDrawnMode      = DrawnMode::NONE;
    errorDisplayActive = true;
    errorDisplayUntil  = millis() + 4000;
}

// ─── PUBLIC: drawPageHeader (no-op — kept for ABI compatibility) ──
void drawPageHeader(String /*title*/, uint16_t /*color*/) {}

// ─── PUBLIC: MAIN UPDATE ──────────────────────────────────────────
/**
 * Updates scr_main panels and labels based on current system state.
 * Called from loop() when displayNeedsUpdate==true.
 * lv_timer_handler() is called separately from loop().
 */
void updateDisplayScreen() {
    char buf[64];

    // ── DISPENSING: switch to dedicated screen ─────────────────────
    if (currentSystemState == CurrentSystemState::DISPENSING) {
        if (lastDrawnMode != DrawnMode::DISPENSING) {
            // Build slot info text once
            snprintf(buf, sizeof(buf), "Fach #%d  \xc2\xb7  %s EUR",
                     dispenseJob.slot + 1,
                     centsToEurStr(slotPriceCents[dispenseJob.slot]).c_str());
            lv_label_set_text(d_slot_info, buf);
            lv_bar_set_value(d_bar, 0, LV_ANIM_OFF);
            lv_scr_load(scr_dispense);
            lastDrawnMode = DrawnMode::DISPENSING;
        }
        // Update progress bar
        unsigned long elapsed = millis() - dispenseJob.startTime;
        if (elapsed > (unsigned long)DISPENSE_RELAY_ON_TIME)
            elapsed = (unsigned long)DISPENSE_RELAY_ON_TIME;
        int pct = (int)(elapsed * 100UL / (unsigned long)DISPENSE_RELAY_ON_TIME);
        lv_bar_set_value(d_bar, pct, LV_ANIM_OFF);
        return;
    }

    // ── Switch back to scr_main after dispensing ───────────────────
    if (lv_scr_act() != scr_main) {
        lv_scr_load(scr_main);
        lastDrawnMode = DrawnMode::NONE;  // force full refresh
    }

    if (currentSystemState == CurrentSystemState::ERROR_DISPLAY) return;

    bool modeChanged = (lastDrawnMode != DrawnMode::NORMAL);
    if (modeChanged) {
        lastDrawnMode = DrawnMode::NORMAL;
    }

    // ── WiFi dot color ─────────────────────────────────────────────
    bool offlineModeActive = (digitalRead(OFFLINE_MODE_PIN) == LOW);
    if (!offlineModeActive) {
        lv_obj_set_style_bg_color(m_wifi_dot,
            (WiFi.status() == WL_CONNECTED) ? C_SUCCESS : C_ERROR, 0);
    }

    // ── Footer update (slogan + URL) ───────────────────────────────
    if (modeChanged) {
        lv_label_set_text(m_footer_url, displayFooter.c_str());
        if (displaySlogan.length() > 0) {
            lv_label_set_text(m_slogan, displaySlogan.c_str());
            lv_obj_clear_flag(m_slogan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(m_slogan,     LV_ALIGN_TOP_MID, 0, 200);
            lv_obj_align(m_footer_url, LV_ALIGN_TOP_MID, 0, 222);
        } else {
            lv_obj_add_flag(m_slogan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(m_footer_url, LV_ALIGN_TOP_MID, 0, 218);
        }
    }

    // ── Credit zone ────────────────────────────────────────────────
    lv_obj_clear_flag(m_credit_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m_credit_val, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m_no_credit, LV_OBJ_FLAG_HIDDEN);
    if (creditCents > 0) {
        snprintf(buf, sizeof(buf), "%s EUR", centsToEurStr(creditCents).c_str());
        lv_label_set_text(m_credit_val, buf);
        lv_obj_set_style_text_color(m_credit_val, C_SUCCESS, 0);
    } else {
        lv_label_set_text(m_credit_val, "0.00 EUR");
        lv_obj_set_style_text_color(m_credit_val, C_TEXT, 0);
    }

    // ── SLOT SELECTED ──────────────────────────────────────────────
    if (selectedSlot != -1) {
        bool avail = slotAvailable[selectedSlot];
        bool lock  = slotLocked[selectedSlot];

        // Show slot panel, hide others
        lv_obj_add_flag(p_idle,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p_keypad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p_slot,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(m_status,  LV_OBJ_FLAG_HIDDEN);

        // Slot number
        snprintf(buf, sizeof(buf), "%d", selectedSlot + 1);
        lv_label_set_text(ps_num, buf);

        // Card border color based on state
        lv_obj_set_style_border_color(ps_card,
            (!lock && avail) ? C_ACCENT : C_ERROR, 0);

        if (lock) {
            lv_label_set_text(ps_sub_lbl, "");
            lv_label_set_text(ps_sub_val, "Gesperrt");
            lv_obj_set_style_text_color(ps_sub_val, C_ERROR, 0);
        } else if (!avail) {
            lv_label_set_text(ps_sub_lbl, "");
            lv_label_set_text(ps_sub_val, "Leer");
            lv_obj_set_style_text_color(ps_sub_val, C_ERROR, 0);
        } else {
            lv_label_set_text(ps_sub_lbl, "PREIS");
            snprintf(buf, sizeof(buf), "%s EUR", centsToEurStr(slotPriceCents[selectedSlot]).c_str());
            lv_label_set_text(ps_sub_val, buf);
            lv_obj_set_style_text_color(ps_sub_val, C_ACCENT, 0);
        }

        // Status line (only when slot available and not locked)
        if (!lock && avail) {
            if (creditCents >= slotPriceCents[selectedSlot]) {
                lv_label_set_text(m_status, "Bereit zum Kauf!");
                lv_obj_set_style_text_color(m_status, C_SUCCESS, 0);
            } else {
                int missing = slotPriceCents[selectedSlot] - creditCents;
                snprintf(buf, sizeof(buf), "Fehlt: %s EUR", centsToEurStr(missing).c_str());
                lv_label_set_text(m_status, buf);
                lv_obj_set_style_text_color(m_status, C_ACCENT, 0);
            }
        } else {
            lv_obj_add_flag(m_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // ── PARTIAL KEYPAD INPUT ───────────────────────────────────────
    if (keypadInputBuffer.length() > 0) {
        lv_obj_add_flag(p_idle,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p_slot,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(m_status,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p_keypad, LV_OBJ_FLAG_HIDDEN);

        String inputDisplay = keypadInputBuffer + "_";
        lv_label_set_text(pk_input, inputDisplay.c_str());

        lv_label_set_text(pk_hint, "2. Ziffer oder # bestaetigen");
        return;
    }

    // ── IDLE ───────────────────────────────────────────────────────
    lv_obj_add_flag(p_keypad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p_slot,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p_idle, LV_OBJ_FLAG_HIDDEN);

    if (creditCents > 0) {
        lv_label_set_text(pi_main, "Fach waehlen");
        lv_obj_set_style_text_color(pi_main, C_TEXT, 0);
        lv_label_set_text(pi_sub, "# zum Kaufen druecken");
        lv_obj_set_style_text_color(pi_sub, C_SUCCESS, 0);
    } else {
        lv_label_set_text(pi_main, "Geld einwerfen");
        lv_obj_set_style_text_color(pi_main, C_TEXT, 0);
        snprintf(buf, sizeof(buf), "oder Fach 1-%d waehlen", activeSlots);
        lv_label_set_text(pi_sub, buf);
        lv_obj_set_style_text_color(pi_sub, C_DIVIDER, 0);
    }
}
