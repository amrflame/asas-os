/*
 * gui_app_calc.c — Calculator: display + 5-row button grid, integer arithmetic
 * Rebuilt with gui_widget buttons for hover/press effects.
 */

#include "gui_wm.h"
#include "gui_draw.h"
#include "gui_theme.h"
#include "gui_compositor.h"
#include "gui_widget.h"

#define DISPLAY_H   44
#define BTN_ROWS     5
#define BTN_COLS     4
#define BTN_PAD      4
#define BTN_COUNT   (BTN_ROWS * BTN_COLS)

typedef struct { const char *label; UINT32 bg; UINT32 bg_hover; } CALC_BTN_DEF;

static const CALC_BTN_DEF s_btn_defs[BTN_ROWS][BTN_COLS] = {
    { {"C",   0xFF3A3A4A,0xFF505060}, {"+/-",0xFF3A3A4A,0xFF505060}, {"%",  0xFF3A3A4A,0xFF505060}, {"/",  C_ACCENT_DIM,0xFF2D6FAF} },
    { {"7",   0xFF1E2C3C,0xFF2A3C50}, {"8",  0xFF1E2C3C,0xFF2A3C50}, {"9",  0xFF1E2C3C,0xFF2A3C50}, {"*",  C_ACCENT_DIM,0xFF2D6FAF} },
    { {"4",   0xFF1E2C3C,0xFF2A3C50}, {"5",  0xFF1E2C3C,0xFF2A3C50}, {"6",  0xFF1E2C3C,0xFF2A3C50}, {"-",  C_ACCENT_DIM,0xFF2D6FAF} },
    { {"1",   0xFF1E2C3C,0xFF2A3C50}, {"2",  0xFF1E2C3C,0xFF2A3C50}, {"3",  0xFF1E2C3C,0xFF2A3C50}, {"+",  C_ACCENT_DIM,0xFF2D6FAF} },
    { {"BS",  0xFF3A3A4A,0xFF505060}, {"0",  0xFF1E2C3C,0xFF2A3C50}, {".",  0xFF1E2C3C,0xFF2A3C50}, {"=",  C_ACCENT,    0xFF3D8AE0} },
};

/* 20 widget buttons — dynamically positioned each paint */
static GUI_WIDGET s_btn_widgets[BTN_COUNT];

/* Calculator state */
static char    s_disp[24];
static UINT32  s_disp_len;
static __int64 s_operand;
static char    s_op;
static UINT8   s_new_input;
static UINT8   s_dot_used;

static void disp_set_int(__int64 v)
{
    UINT8 neg = 0; char tmp[24]; UINT32 i = 0, j;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0 && i < 22) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    if (neg && i < 23) tmp[i++] = '-';
    for (j = 0; j < i; j++) s_disp[j] = tmp[i - 1 - j];
    s_disp[i] = '\0'; s_disp_len = i;
}

static __int64 disp_to_int(void)
{
    __int64 v = 0; UINT32 i = 0; UINT8 neg = 0;
    if (s_disp[0] == '-') { neg = 1; i = 1; }
    while (s_disp[i] >= '0' && s_disp[i] <= '9') v = v * 10 + (s_disp[i++] - '0');
    return neg ? -v : v;
}

static void calc_press(const char *label)
{
    if (label[0] == 'C') {
        s_disp[0]='0'; s_disp[1]='\0'; s_disp_len=1;
        s_operand=0; s_op=0; s_new_input=1; s_dot_used=0; return;
    }
    if (label[0]=='B' && label[1]=='S') {
        if (s_disp_len > 1) { s_disp[--s_disp_len]='\0'; }
        else { s_disp[0]='0'; s_disp_len=1; } return;
    }
    if (label[0]=='+' && label[1]=='/') {
        if (s_disp[0]=='-') { UINT32 k; for(k=0;k<s_disp_len;k++) s_disp[k]=s_disp[k+1]; s_disp_len--; }
        else if (s_disp[0]!='0') { UINT32 k; for(k=s_disp_len+1;k>0;k--) s_disp[k]=s_disp[k-1]; s_disp[0]='-'; s_disp_len++; }
        return;
    }
    if (label[0]=='%') { __int64 v=disp_to_int(); disp_set_int(v/100); return; }
    if (label[0]=='=') {
        if (s_op) {
            __int64 a=s_operand, b=disp_to_int(), r=0;
            if      (s_op=='+') r=a+b;
            else if (s_op=='-') r=a-b;
            else if (s_op=='*') r=a*b;
            else if (s_op=='/' && b!=0) r=a/b;
            disp_set_int(r); s_op=0; s_new_input=1; s_dot_used=0;
        }
        return;
    }
    if (label[0]=='+'||label[0]=='-'||label[0]=='*'||label[0]=='/') {
        s_operand=disp_to_int(); s_op=label[0]; s_new_input=1; s_dot_used=0; return;
    }
    if (label[0]>='0' && label[0]<='9') {
        if (s_new_input) { s_disp[0]=label[0]; s_disp[1]='\0'; s_disp_len=1; s_new_input=0; }
        else if (s_disp_len < 18) { s_disp[s_disp_len++]=label[0]; s_disp[s_disp_len]='\0'; }
        return;
    }
    if (label[0]=='.' && !s_dot_used) {
        if (s_new_input) { s_disp[0]='0'; s_disp[1]='\0'; s_disp_len=1; s_new_input=0; }
        if (s_disp_len < 18) { s_disp[s_disp_len++]='.'; s_disp[s_disp_len]='\0'; s_dot_used=1; }
    }
}

/* Update widget geometry from current window rect */
static void calc_layout(const GUI_WIN *win)
{
    int ww = (int)win->w, wh = (int)win->h - CHROME_H;
    int btn_w = (ww - BTN_PAD * (BTN_COLS + 1)) / BTN_COLS;
    int btn_h = (wh - DISPLAY_H - BTN_PAD * (BTN_ROWS + 1)) / BTN_ROWS;
    UINT32 r, c;
    for (r = 0; r < BTN_ROWS; r++) {
        for (c = 0; c < BTN_COLS; c++) {
            GUI_WIDGET *b = &s_btn_widgets[r * BTN_COLS + c];
            b->x = win->x + BTN_PAD + (int)c * (btn_w + BTN_PAD);
            b->y = win->y + CHROME_H + DISPLAY_H + BTN_PAD + (int)r * (btn_h + BTN_PAD);
            b->w = (UINT32)btn_w;
            b->h = (UINT32)btn_h;
        }
    }
}

static void calc_paint(GUI_WIN *win)
{
    int wx = win->x, wy = win->y + CHROME_H;
    UINT32 r, c;

    /* Layout buttons to current window size */
    calc_layout(win);

    /* Display area */
    gui_fill_rect(wx, wy, win->w, (UINT32)DISPLAY_H, C_WIN_CHROME);
    gui_fill_rect(wx, wy + DISPLAY_H - 1, win->w, 1, C_BORDER);

    /* Operator indicator */
    if (s_op) {
        char op_str[3] = { s_op, ' ', '\0' };
        gui_draw_text(wx + 8, wy + 8, op_str, C_TEXT_MUTED);
    }
    /* Display value — right-aligned, large */
    gui_draw_text_right(wx, wy + (DISPLAY_H - FONT_H) / 2,
                        win->w - 12, s_disp, C_TEXT_PRIMARY);

    /* Render all button widgets */
    for (r = 0; r < BTN_ROWS; r++) {
        for (c = 0; c < BTN_COLS; c++) {
            gui_widget_render(&s_btn_widgets[r * BTN_COLS + c]);
        }
    }
}

static void calc_on_key(GUI_WIN *win, UINT8 scancode)
{
    (void)win;
    if (scancode == 0x0E) { calc_press("BS"); return; }
    if (scancode == 0x1C) { calc_press("=");  return; }
    if (scancode == 0x39) { calc_press("C");  return; } /* space = clear */
}

void app_calc_handle_click(int mx, int my)
{
    UINT32 r, c;
    const GUI_WIN *win = gui_wm_get(GUI_WIN_CALC);
    if (!win || win->minimized) return;

    for (r = 0; r < BTN_ROWS; r++) {
        for (c = 0; c < BTN_COLS; c++) {
            GUI_WIDGET *b = &s_btn_widgets[r * BTN_COLS + c];
            if (gui_widget_handle_click(b, mx, my)) {
                calc_press(s_btn_defs[r][c].label);
                return;
            }
        }
    }
}

void app_calc_update_hover(int mx, int my)
{
    UINT32 r, c;
    for (r = 0; r < BTN_ROWS; r++)
        for (c = 0; c < BTN_COLS; c++)
            gui_widget_update_hover(&s_btn_widgets[r * BTN_COLS + c], mx, my);
}

void app_calc_initialize(void)
{
    UINT32 r, c;
    s_disp[0]='0'; s_disp[1]='\0'; s_disp_len=1;
    s_operand=0; s_op=0; s_new_input=1; s_dot_used=0;

    /* Initialise button widgets */
    for (r = 0; r < BTN_ROWS; r++) {
        for (c = 0; c < BTN_COLS; c++) {
            GUI_WIDGET *b = &s_btn_widgets[r * BTN_COLS + c];
            gui_widget_button(b, 0, 0, 0, 0, s_btn_defs[r][c].label);
            b->style.bg       = s_btn_defs[r][c].bg;
            b->style.bg_hover = s_btn_defs[r][c].bg_hover;
            b->style.bg_press = 0xFF0D1117;
            b->style.border   = 0;
            b->style.corner_r = 3;
            b->style.fg       = C_TEXT_PRIMARY;
        }
    }
    gui_wm_set_callbacks(GUI_WIN_CALC, calc_paint, calc_on_key);
}

