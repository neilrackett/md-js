/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: demo_gem.c
 * Description: MD/JS Code — minimal GEM IDE for the SidecarTridge MD/JS
 * JavaScript Worker. Built as MDJSCODE.PRG.
 *
 * Layout:
 *   - GEM menu bar with MD/JS, File and Code menus
 *   - Top fixed GEM window titled "Code" (editable via TexEdit)
 *   - Bottom fixed GEM window titled "Result"
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <string.h>

#include "mdjs.h"
#include "textedit.h"

static short app_id;
static short aes_handle;
static VdiHdl virt_vdi = -1;
static short sys_char_w, sys_char_h, sys_cell_w, sys_cell_h;
static short desk_x, desk_y, desk_w, desk_h;

static short code_win = -1;
static short result_win = -1;
static short quit_requested = 0;
static short result_top_line = 0;
static short result_left_col = 0;
static short async_call_pending = 0;

/* TexEdit for the Code window — declared static because it's ~52 KB */
static TexEdit code_te;

static const char *const initial_code_lines[] = {
    "function main(name) {", "  return 'Hello, ' + (name || 'World') + '!';",
    "}", "", "/* Edit me! */"};
#define INITIAL_CODE_LINE_COUNT 5

static char current_result[2048] = "";

/* Menu bar object tree, built at runtime like the Run dialog */
enum {
  MENU_ROOT = 0, /* G_IBOX covering the screen */
  MENU_BAR_BOX,  /* G_BOX — the visible bar */
  MENU_ACTIVE,   /* G_IBOX holding the titles */
  MENU_T_MDJS,
  MENU_T_FILE,
  MENU_T_CODE,
  MENU_SCREEN, /* G_IBOX holding the drop-down boxes */
  MENU_BOX_MDJS,
  MENU_I_ABOUT,
  MENU_I_DESKSEP,
  MENU_I_ACC1,
  MENU_I_ACC2,
  MENU_I_ACC3,
  MENU_I_ACC4,
  MENU_I_ACC5,
  MENU_I_ACC6,
  MENU_BOX_FILE,
  MENU_I_NEW,
  MENU_I_LOAD,
  MENU_I_SAVE,
  MENU_I_FILESEP,
  MENU_I_QUIT,
  MENU_BOX_CODE,
  MENU_I_RUN
};

#define MENU_COUNT 24

static OBJECT menu_tree[MENU_COUNT];

static char m_t_mdjs[] = " MD/JS ";
static char m_t_file[] = " File ";
static char m_t_code[] = " Code ";
static char m_i_about[] = "  About MD/JS Code";
static char m_i_desksep[] = "--------------------";
static char m_i_acc1[] = "  Accessory 1";
static char m_i_acc2[] = "  Accessory 2";
static char m_i_acc3[] = "  Accessory 3";
static char m_i_acc4[] = "  Accessory 4";
static char m_i_acc5[] = "  Accessory 5";
static char m_i_acc6[] = "  Accessory 6";
static char m_i_new[] = "  New";
static char m_i_load[] = "  Load";
static char m_i_save[] = "  Save";
static char m_i_filesep[] = "----------";
static char m_i_quit[] = "  Quit";
static char m_i_run[] = "  Run...";

/* Shared file selector state — remembers last directory across Load/Save */
static char fsel_path[128] = "";
static char fsel_file[13] = "";

#define FUNC_LEN 32
#define ARGS_LEN 32
static char dialog_func[FUNC_LEN + 1];
static char dialog_func_template[FUNC_LEN + 1];
static char dialog_func_valid[FUNC_LEN + 1];
static char dialog_args[ARGS_LEN + 1];
static char dialog_args_template[ARGS_LEN + 1];
static char dialog_args_valid[ARGS_LEN + 1];

enum {
  DL_ROOT = 0,
  DL_FUNC_LABEL,
  DL_FUNC_INPUT,
  DL_ARGS_LABEL,
  DL_ARGS_INPUT,
  DL_ASYNC_LABEL,
  DL_ASYNC_YES,
  DL_ASYNC_NO,
  DL_OK,
  DL_CANCEL
};

#define DL_COUNT 10

static char d_func_label[] = "Function name";
static char d_args_label[] = "Parameters (JSON)";
static char d_async_label[] = "Run async?";
static char d_async_yes[] = "Yes";
static char d_async_no[] = "No";
static char d_ok[] = "OK";
static char d_cancel[] = "Cancel";

static OBJECT dialog_tree[DL_COUNT];
static TEDINFO dialog_func_ted;
static TEDINFO dialog_args_ted;

static short line_height(void) {
  short h = (short)(sys_char_h + 2);
  if (h < 10) {
    h = 10;
  }
  return h;
}

#define DISPLAY_LINE_WIDTH 159

static short count_text_lines(const char *text) {
  short lines = 0;
  short cols = 0;

  if (!text || !text[0]) {
    return 1;
  }

  while (*text) {
    if (*text == '\n') {
      lines++;
      cols = 0;
    } else {
      cols++;
      if (cols == DISPLAY_LINE_WIDTH) {
        lines++;
        cols = 0;
      }
    }
    text++;
  }
  lines++; /* count the last (unterminated) segment */

  return lines;
}

static short max_text_columns(const char *text) {
  short max_cols = 0;
  short cols = 0;

  if (!text || !text[0]) {
    return 7;
  }

  while (*text) {
    if (*text == '\n') {
      if (cols > max_cols) max_cols = cols;
      cols = 0;
    } else {
      cols++;
      if (cols == DISPLAY_LINE_WIDTH) {
        if (cols > max_cols) max_cols = cols;
        cols = 0;
      }
    }
    text++;
  }

  if (cols > max_cols) max_cols = cols;
  if (max_cols > DISPLAY_LINE_WIDTH) max_cols = DISPLAY_LINE_WIDTH;

  return max_cols;
}

static short visible_lines_for_window(short win) {
  short wx, wy, ww, wh;
  short lh;
  short visible;

  wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  lh = line_height();
  visible = (short)((wh - 4) / lh);
  if (visible < 1) {
    visible = 1;
  }
  return visible;
}

static short visible_columns_for_window(short win) {
  short wx, wy, ww, wh;
  short visible;

  wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  visible = (short)((ww - 8) / sys_char_w);
  if (visible < 1) {
    visible = 1;
  }
  return visible;
}

static short max_top_line_for_window(short win, const char *text) {
  short total = count_text_lines(text);
  short visible = visible_lines_for_window(win);

  if (total <= visible) {
    return 0;
  }
  return (short)(total - visible);
}

static short max_left_col_for_window(short win, const char *text) {
  short total = max_text_columns(text);
  short visible = visible_columns_for_window(win);

  if (total <= visible) {
    return 0;
  }
  return (short)(total - visible);
}

static void update_window_sliders(short win, const char *text, short top_line,
                                  short left_col) {
  short total = count_text_lines(text);
  short visible = visible_lines_for_window(win);
  short max_top = max_top_line_for_window(win, text);
  short total_cols = max_text_columns(text);
  short visible_cols = visible_columns_for_window(win);
  short max_left = max_left_col_for_window(win, text);
  short size;
  short pos;

  if (total <= visible) {
    size = 1000;
    pos = 0;
  } else {
    size = (short)((1000L * visible) / total);
    if (size < 1) {
      size = 1;
    }
    pos = (short)((1000L * top_line) / max_top);
  }

  wind_set(win, WF_VSLSIZE, size, 0, 0, 0);
  wind_set(win, WF_VSLIDE, pos, 0, 0, 0);

  if (total_cols <= visible_cols) {
    size = 1000;
    pos = 0;
  } else {
    size = (short)((1000L * visible_cols) / total_cols);
    if (size < 1) {
      size = 1;
    }
    pos = (short)((1000L * left_col) / max_left);
  }

  wind_set(win, WF_HSLSIZE, size, 0, 0, 0);
  wind_set(win, WF_HSLIDE, pos, 0, 0, 0);
}

static void redraw_all(void);
static void focus_result_window(void);
static void present_result_window(void);
static void fsel_init_path(void);
static void poll_async_result(void);

static void set_result_text(const char *text) {
  if (!text || !text[0]) {
    current_result[0] = '\0';
  } else {
    strncpy(current_result, text, sizeof(current_result) - 1);
    current_result[sizeof(current_result) - 1] = '\0';
  }
  result_top_line = 0;
  result_left_col = 0;
}

static void set_result_error_with_fallback(const char *fallback) {
  char buf[2048];

  buf[0] = '\0';
  if (mdjs_result(buf, (int)sizeof(buf)) == 0 && buf[0] != '\0') {
    set_result_text(buf);
  } else {
    set_result_text(fallback);
  }
}

static void obfix_tree(OBJECT *tree, short count) {
  short i;

  for (i = 0; i < count; i++) {
    rsrc_obfix(tree, i);
  }
}

static void menu_set_box(short idx, short next, short head, short tail,
                         short type, long spec, short x, short y, short w,
                         short h) {
  menu_tree[idx].ob_next = next;
  menu_tree[idx].ob_head = head;
  menu_tree[idx].ob_tail = tail;
  menu_tree[idx].ob_type = type;
  menu_tree[idx].ob_flags = OF_NONE;
  menu_tree[idx].ob_state = OS_NORMAL;
  menu_tree[idx].ob_spec.index = spec;
  menu_tree[idx].ob_x = x;
  menu_tree[idx].ob_y = y;
  menu_tree[idx].ob_width = w;
  menu_tree[idx].ob_height = h;
}

static void menu_set_string(short idx, short next, short type, short state,
                            char *str, short x, short y, short w, short h) {
  menu_tree[idx].ob_next = next;
  menu_tree[idx].ob_head = -1;
  menu_tree[idx].ob_tail = -1;
  menu_tree[idx].ob_type = type;
  menu_tree[idx].ob_flags = OF_NONE;
  menu_tree[idx].ob_state = state;
  menu_tree[idx].ob_spec.free_string = str;
  menu_tree[idx].ob_x = x;
  menu_tree[idx].ob_y = y;
  menu_tree[idx].ob_width = w;
  menu_tree[idx].ob_height = h;
}

static void build_menu(void) {
  short sx, sy, sw, sh;
  short scr_cols;
  short mdjs_w = 7; /* strlen(" MD/JS ") */
  short file_w = 6;
  short code_w = 6;
  short mdjs_box_w = 20;
  short file_box_w = 10;
  short code_box_w = 10; /* "  Run..." (8) + 2 chars right padding */

  /* Use the character width here: graf_handle's box width includes
     padding on some TOS versions (19px in TOS 2.06 mono) and would
     leave the bar covering only part of the screen. */
  wind_get(0, WF_WORKXYWH, &sx, &sy, &sw, &sh);
  scr_cols = (short)(sw / sys_char_w);

  /* Raw coords are chars in the low byte plus pixels in the high byte;
     rsrc_obfix converts them. 513/769 are the standard bar/title heights. */
  menu_set_box(MENU_ROOT, -1, MENU_BAR_BOX, MENU_SCREEN, G_IBOX, 0x0L, 0, 0,
               scr_cols, 25);
  menu_set_box(MENU_BAR_BOX, MENU_SCREEN, MENU_ACTIVE, MENU_ACTIVE, G_BOX,
               0x1100L, 0, 0, scr_cols, 513);
  menu_set_box(MENU_ACTIVE, MENU_BAR_BOX, MENU_T_MDJS, MENU_T_CODE, G_IBOX,
               0x0L, 2, 0, (short)(mdjs_w + file_w + code_w), 769);

  menu_set_string(MENU_T_MDJS, MENU_T_FILE, G_TITLE, OS_NORMAL, m_t_mdjs, 0, 0,
                  mdjs_w, 769);
  menu_set_string(MENU_T_FILE, MENU_T_CODE, G_TITLE, OS_NORMAL, m_t_file,
                  mdjs_w, 0, file_w, 769);
  menu_set_string(MENU_T_CODE, MENU_ACTIVE, G_TITLE, OS_NORMAL, m_t_code,
                  (short)(mdjs_w + file_w), 0, code_w, 769);

  menu_set_box(MENU_SCREEN, MENU_ROOT, MENU_BOX_MDJS, MENU_BOX_CODE, G_IBOX,
               0x0L, 0, 769, scr_cols, 19);

  /* MD/JS menu: About plus the six desk accessory slots the AES manages */
  menu_set_box(MENU_BOX_MDJS, MENU_BOX_FILE, MENU_I_ABOUT, MENU_I_ACC6, G_BOX,
               0xFF1100L, 2, 0, mdjs_box_w, 8);
  menu_set_string(MENU_I_ABOUT, MENU_I_DESKSEP, G_STRING, OS_NORMAL, m_i_about,
                  0, 0, mdjs_box_w, 1);
  menu_set_string(MENU_I_DESKSEP, MENU_I_ACC1, G_STRING, OS_DISABLED,
                  m_i_desksep, 0, 1, mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC1, MENU_I_ACC2, G_STRING, OS_NORMAL, m_i_acc1, 0, 2,
                  mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC2, MENU_I_ACC3, G_STRING, OS_NORMAL, m_i_acc2, 0, 3,
                  mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC3, MENU_I_ACC4, G_STRING, OS_NORMAL, m_i_acc3, 0, 4,
                  mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC4, MENU_I_ACC5, G_STRING, OS_NORMAL, m_i_acc4, 0, 5,
                  mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC5, MENU_I_ACC6, G_STRING, OS_NORMAL, m_i_acc5, 0, 6,
                  mdjs_box_w, 1);
  menu_set_string(MENU_I_ACC6, MENU_BOX_MDJS, G_STRING, OS_NORMAL, m_i_acc6, 0,
                  7, mdjs_box_w, 1);

  /* File menu, aligned under the File title */
  menu_set_box(MENU_BOX_FILE, MENU_BOX_CODE, MENU_I_NEW, MENU_I_QUIT, G_BOX,
               0xFF1100L, (short)(2 + mdjs_w), 0, file_box_w, 5);
  menu_set_string(MENU_I_NEW, MENU_I_LOAD, G_STRING, OS_NORMAL, m_i_new, 0, 0,
                  file_box_w, 1);
  menu_set_string(MENU_I_LOAD, MENU_I_SAVE, G_STRING, OS_NORMAL, m_i_load, 0, 1,
                  file_box_w, 1);
  menu_set_string(MENU_I_SAVE, MENU_I_FILESEP, G_STRING, OS_NORMAL, m_i_save, 0,
                  2, file_box_w, 1);
  menu_set_string(MENU_I_FILESEP, MENU_I_QUIT, G_STRING, OS_DISABLED,
                  m_i_filesep, 0, 3, file_box_w, 1);
  menu_set_string(MENU_I_QUIT, MENU_BOX_FILE, G_STRING, OS_NORMAL, m_i_quit, 0,
                  4, file_box_w, 1);

  /* Code menu, aligned under the Code title */
  menu_set_box(MENU_BOX_CODE, MENU_SCREEN, MENU_I_RUN, MENU_I_RUN, G_BOX,
               0xFF1100L, (short)(2 + mdjs_w + file_w), 0, code_box_w, 1);
  menu_set_string(MENU_I_RUN, MENU_BOX_CODE, G_STRING, OS_NORMAL, m_i_run, 0, 0,
                  code_box_w, 1);
  menu_tree[MENU_I_RUN].ob_flags = OF_LASTOB;

  obfix_tree(menu_tree, MENU_COUNT);

  /* Force full screen size in pixels so the bar redraw always covers the
     whole width regardless of character metrics. */
  menu_tree[MENU_ROOT].ob_width = sw;
  menu_tree[MENU_ROOT].ob_height = (short)(sy + sh);
  menu_tree[MENU_BAR_BOX].ob_width = sw;
  menu_tree[MENU_SCREEN].ob_width = sw;
  menu_tree[MENU_SCREEN].ob_height =
      (short)(sy + sh - menu_tree[MENU_SCREEN].ob_y);
}

static void build_dialog(void) {
  short args_label_w = (short)strlen(d_args_label);
  short input_w = ARGS_LEN;
  short btn_w = 8;
  short dlg_w = (short)(input_w + 4);
  short i;

  if (args_label_w + 4 > dlg_w) dlg_w = (short)(args_label_w + 4);

  for (i = 0; i < FUNC_LEN; i++) {
    dialog_func_template[i] = '_';
    dialog_func_valid[i] = 'X';
  }
  dialog_func_template[FUNC_LEN] = '\0';
  dialog_func_valid[FUNC_LEN] = '\0';
  strcpy(dialog_func, "main");
  for (i = (short)strlen(dialog_func); i < FUNC_LEN; i++) dialog_func[i] = '\0';

  for (i = 0; i < ARGS_LEN; i++) {
    dialog_args_template[i] = '_';
    dialog_args_valid[i] = 'X';
  }
  dialog_args_template[ARGS_LEN] = '\0';
  dialog_args_valid[ARGS_LEN] = '\0';
  strcpy(dialog_args, "[]");
  for (i = (short)strlen(dialog_args); i < ARGS_LEN; i++) dialog_args[i] = '\0';

  dialog_func_ted.te_ptext = dialog_func;
  dialog_func_ted.te_ptmplt = dialog_func_template;
  dialog_func_ted.te_pvalid = dialog_func_valid;
  dialog_func_ted.te_font = 3;
  dialog_func_ted.te_fontid = 0;
  dialog_func_ted.te_just = 0;
  dialog_func_ted.te_color = 0x1180;
  dialog_func_ted.te_fontsize = 0;
  dialog_func_ted.te_thickness = -1;
  dialog_func_ted.te_txtlen = FUNC_LEN + 1;
  dialog_func_ted.te_tmplen = FUNC_LEN + 1;

  dialog_args_ted.te_ptext = dialog_args;
  dialog_args_ted.te_ptmplt = dialog_args_template;
  dialog_args_ted.te_pvalid = dialog_args_valid;
  dialog_args_ted.te_font = 3;
  dialog_args_ted.te_fontid = 0;
  dialog_args_ted.te_just = 0;
  dialog_args_ted.te_color = 0x1180;
  dialog_args_ted.te_fontsize = 0;
  dialog_args_ted.te_thickness = -1;
  dialog_args_ted.te_txtlen = ARGS_LEN + 1;
  dialog_args_ted.te_tmplen = ARGS_LEN + 1;

  /* Layout (character rows):
     0  top margin
     1  "Function:" label
     2  function input
     3  (blank line)
     4  "Parameters (JSON):" label
     5  args input
     6  (blank line)
     7  Run async checkbox
     8  (margin)
     9  buttons
    10  bottom margin             => total height 11 */

  dialog_tree[DL_ROOT].ob_next = -1;
  dialog_tree[DL_ROOT].ob_head = DL_FUNC_LABEL;
  dialog_tree[DL_ROOT].ob_tail = DL_CANCEL;
  dialog_tree[DL_ROOT].ob_type = G_BOX;
  dialog_tree[DL_ROOT].ob_flags = OF_NONE;
  dialog_tree[DL_ROOT].ob_state = OS_OUTLINED;
  dialog_tree[DL_ROOT].ob_spec.index = 0x21100L;
  dialog_tree[DL_ROOT].ob_x = 0;
  dialog_tree[DL_ROOT].ob_y = 0;
  dialog_tree[DL_ROOT].ob_width = dlg_w;
  dialog_tree[DL_ROOT].ob_height = 11;

  dialog_tree[DL_FUNC_LABEL].ob_next = DL_FUNC_INPUT;
  dialog_tree[DL_FUNC_LABEL].ob_head = -1;
  dialog_tree[DL_FUNC_LABEL].ob_tail = -1;
  dialog_tree[DL_FUNC_LABEL].ob_type = G_STRING;
  dialog_tree[DL_FUNC_LABEL].ob_flags = OF_NONE;
  dialog_tree[DL_FUNC_LABEL].ob_state = OS_NORMAL;
  dialog_tree[DL_FUNC_LABEL].ob_spec.free_string = d_func_label;
  dialog_tree[DL_FUNC_LABEL].ob_x = 2;
  dialog_tree[DL_FUNC_LABEL].ob_y = 1;
  dialog_tree[DL_FUNC_LABEL].ob_width = (short)strlen(d_func_label);
  dialog_tree[DL_FUNC_LABEL].ob_height = 1;

  dialog_tree[DL_FUNC_INPUT].ob_next = DL_ARGS_LABEL;
  dialog_tree[DL_FUNC_INPUT].ob_head = -1;
  dialog_tree[DL_FUNC_INPUT].ob_tail = -1;
  dialog_tree[DL_FUNC_INPUT].ob_type = G_FTEXT;
  dialog_tree[DL_FUNC_INPUT].ob_flags = OF_EDITABLE;
  dialog_tree[DL_FUNC_INPUT].ob_state = OS_NORMAL;
  dialog_tree[DL_FUNC_INPUT].ob_spec.tedinfo = &dialog_func_ted;
  dialog_tree[DL_FUNC_INPUT].ob_x = 2;
  dialog_tree[DL_FUNC_INPUT].ob_y = 2;
  dialog_tree[DL_FUNC_INPUT].ob_width = FUNC_LEN;
  dialog_tree[DL_FUNC_INPUT].ob_height = 1;

  dialog_tree[DL_ARGS_LABEL].ob_next = DL_ARGS_INPUT;
  dialog_tree[DL_ARGS_LABEL].ob_head = -1;
  dialog_tree[DL_ARGS_LABEL].ob_tail = -1;
  dialog_tree[DL_ARGS_LABEL].ob_type = G_STRING;
  dialog_tree[DL_ARGS_LABEL].ob_flags = OF_NONE;
  dialog_tree[DL_ARGS_LABEL].ob_state = OS_NORMAL;
  dialog_tree[DL_ARGS_LABEL].ob_spec.free_string = d_args_label;
  dialog_tree[DL_ARGS_LABEL].ob_x = 2;
  dialog_tree[DL_ARGS_LABEL].ob_y = 4;
  dialog_tree[DL_ARGS_LABEL].ob_width = args_label_w;
  dialog_tree[DL_ARGS_LABEL].ob_height = 1;

  dialog_tree[DL_ARGS_INPUT].ob_next = DL_ASYNC_LABEL;
  dialog_tree[DL_ARGS_INPUT].ob_head = -1;
  dialog_tree[DL_ARGS_INPUT].ob_tail = -1;
  dialog_tree[DL_ARGS_INPUT].ob_type = G_FTEXT;
  dialog_tree[DL_ARGS_INPUT].ob_flags = OF_EDITABLE;
  dialog_tree[DL_ARGS_INPUT].ob_state = OS_NORMAL;
  dialog_tree[DL_ARGS_INPUT].ob_spec.tedinfo = &dialog_args_ted;
  dialog_tree[DL_ARGS_INPUT].ob_x = 2;
  dialog_tree[DL_ARGS_INPUT].ob_y = 5;
  dialog_tree[DL_ARGS_INPUT].ob_width = input_w;
  dialog_tree[DL_ARGS_INPUT].ob_height = 1;

  {
    short async_label_w = (short)strlen(d_async_label);
    short yes_w = (short)((short)strlen(d_async_yes) + 4);
    short no_w = (short)((short)strlen(d_async_no) + 4);
    short yes_x = (short)(2 + async_label_w + 2);
    short no_x = (short)(yes_x + yes_w + 1);

    dialog_tree[DL_ASYNC_LABEL].ob_next = DL_ASYNC_YES;
    dialog_tree[DL_ASYNC_LABEL].ob_head = -1;
    dialog_tree[DL_ASYNC_LABEL].ob_tail = -1;
    dialog_tree[DL_ASYNC_LABEL].ob_type = G_STRING;
    dialog_tree[DL_ASYNC_LABEL].ob_flags = OF_NONE;
    dialog_tree[DL_ASYNC_LABEL].ob_state = OS_NORMAL;
    dialog_tree[DL_ASYNC_LABEL].ob_spec.free_string = d_async_label;
    dialog_tree[DL_ASYNC_LABEL].ob_x = 2;
    dialog_tree[DL_ASYNC_LABEL].ob_y = 7;
    dialog_tree[DL_ASYNC_LABEL].ob_width = async_label_w;
    dialog_tree[DL_ASYNC_LABEL].ob_height = 1;

    dialog_tree[DL_ASYNC_YES].ob_next = DL_ASYNC_NO;
    dialog_tree[DL_ASYNC_YES].ob_head = -1;
    dialog_tree[DL_ASYNC_YES].ob_tail = -1;
    dialog_tree[DL_ASYNC_YES].ob_type = G_BUTTON;
    dialog_tree[DL_ASYNC_YES].ob_flags = OF_SELECTABLE | OF_RBUTTON;
    dialog_tree[DL_ASYNC_YES].ob_state = OS_NORMAL;
    dialog_tree[DL_ASYNC_YES].ob_spec.free_string = d_async_yes;
    dialog_tree[DL_ASYNC_YES].ob_x = yes_x;
    dialog_tree[DL_ASYNC_YES].ob_y = 7;
    dialog_tree[DL_ASYNC_YES].ob_width = yes_w;
    dialog_tree[DL_ASYNC_YES].ob_height = 1;

    dialog_tree[DL_ASYNC_NO].ob_next = DL_OK;
    dialog_tree[DL_ASYNC_NO].ob_head = -1;
    dialog_tree[DL_ASYNC_NO].ob_tail = -1;
    dialog_tree[DL_ASYNC_NO].ob_type = G_BUTTON;
    dialog_tree[DL_ASYNC_NO].ob_flags = OF_SELECTABLE | OF_RBUTTON;
    dialog_tree[DL_ASYNC_NO].ob_state = OS_SELECTED;
    dialog_tree[DL_ASYNC_NO].ob_spec.free_string = d_async_no;
    dialog_tree[DL_ASYNC_NO].ob_x = no_x;
    dialog_tree[DL_ASYNC_NO].ob_y = 7;
    dialog_tree[DL_ASYNC_NO].ob_width = no_w;
    dialog_tree[DL_ASYNC_NO].ob_height = 1;
  }

  dialog_tree[DL_OK].ob_next = DL_CANCEL;
  dialog_tree[DL_OK].ob_head = -1;
  dialog_tree[DL_OK].ob_tail = -1;
  dialog_tree[DL_OK].ob_type = G_BUTTON;
  dialog_tree[DL_OK].ob_flags = OF_SELECTABLE | OF_EXIT | OF_DEFAULT;
  dialog_tree[DL_OK].ob_state = OS_NORMAL;
  dialog_tree[DL_OK].ob_spec.free_string = d_ok;
  dialog_tree[DL_OK].ob_x = 2;
  dialog_tree[DL_OK].ob_y = 9;
  dialog_tree[DL_OK].ob_width = btn_w;
  dialog_tree[DL_OK].ob_height = 1;

  dialog_tree[DL_CANCEL].ob_next = DL_ROOT;
  dialog_tree[DL_CANCEL].ob_head = -1;
  dialog_tree[DL_CANCEL].ob_tail = -1;
  dialog_tree[DL_CANCEL].ob_type = G_BUTTON;
  dialog_tree[DL_CANCEL].ob_flags = OF_SELECTABLE | OF_EXIT | OF_LASTOB;
  dialog_tree[DL_CANCEL].ob_state = OS_NORMAL;
  dialog_tree[DL_CANCEL].ob_spec.free_string = d_cancel;
  dialog_tree[DL_CANCEL].ob_x = (short)(dlg_w - btn_w - 2);
  dialog_tree[DL_CANCEL].ob_y = 9;
  dialog_tree[DL_CANCEL].ob_width = btn_w;
  dialog_tree[DL_CANCEL].ob_height = 1;

  obfix_tree(dialog_tree, DL_COUNT);
}

static void open_windows(void) {
  short content_y;
  short content_h;
  short half_h;
  short work_in[11];
  short work_out[57];
  short i;
  short cell_w, cell_h;

  wind_get(0, WF_WORKXYWH, &desk_x, &desk_y, &desk_w, &desk_h);

  content_y = desk_y;
  content_h = desk_h;
  half_h = (short)(content_h / 2);

  code_win = wind_create(
      NAME | CLOSER | UPARROW | DNARROW | VSLIDE | LFARROW | RTARROW | HSLIDE,
      desk_x, content_y, desk_w, half_h);
  if (code_win < 0) {
    return;
  }
  wind_set_str(code_win, WF_NAME, " Code ");
  wind_open(code_win, desk_x, content_y, desk_w, half_h);

  result_win = wind_create(
      NAME | CLOSER | UPARROW | DNARROW | VSLIDE | LFARROW | RTARROW | HSLIDE,
      desk_x, (short)(content_y + half_h), desk_w, (short)(content_h - half_h));
  if (result_win < 0) {
    return;
  }
  wind_set_str(result_win, WF_NAME, " Result ");
  wind_open(result_win, desk_x, (short)(content_y + half_h), desk_w,
            (short)(content_h - half_h));

  /* Open virtual VDI workstation and get accurate cell metrics */
  for (i = 0; i < 10; i++) work_in[i] = 1;
  work_in[10] = 2;
  v_opnvwk(work_in, &virt_vdi, work_out);

  cell_w = sys_cell_w;
  cell_h = sys_cell_h;

  /* Override cell_w for ST low-res pixel doubling.
     atrib[7..12] = ptsout; atrib[9] = ptsout[2] = rendered char width. */
  if (virt_vdi >= 0) {
    short atrib[13];
    vqt_attributes(virt_vdi, atrib);
    if (atrib[9] > 0) cell_w = atrib[9];
  }

  /* Init textedit for Code window */
  textedit_init(&code_te, code_win, virt_vdi, cell_w, cell_h);
  textedit_set_text(&code_te, initial_code_lines, INITIAL_CODE_LINE_COUNT);
  textedit_update_sliders(&code_te);

  update_window_sliders(result_win, current_result, result_top_line,
                        result_left_col);

  /* Keep Code focused at startup even though Result is opened afterwards. */
  wind_set(code_win, WF_TOP, 0, 0, 0, 0);
}

static void fill_clip_rect(short x, short y, short w, short h) {
  short pxy[4];

  pxy[0] = x;
  pxy[1] = y;
  pxy[2] = (short)(x + w - 1);
  pxy[3] = (short)(y + h - 1);

  vswr_mode(aes_handle, MD_REPLACE);
  vsf_color(aes_handle, 0);
  vsf_interior(aes_handle, FIS_SOLID);
  vsf_perimeter(aes_handle, 0);
  vr_recfl(aes_handle, pxy);
}

static void draw_multiline_text(const char *text, short left, short top,
                                short bottom, short first_line,
                                short first_col) {
  short line_y;
  short line_h;
  char line_buf[160];
  short current_line = 0;

  line_h = line_height();
  line_y = (short)(top + sys_char_h);

  vswr_mode(aes_handle, MD_REPLACE);
  vst_effects(aes_handle, 0);
  vst_color(aes_handle, 1);

  if (text && text[0]) {
    const char *p = text;
    while (*p && line_y + sys_char_h <= bottom) {
      const char *eol = p;
      short total_len;

      while (*eol && *eol != '\n') {
        eol++;
      }
      total_len = (short)(eol - p);

      /* Wrap long logical lines into multiple visual rows of line_buf width. */
      {
        short wrap_w = (short)(sizeof(line_buf) - 1);
        short seg_start = 0;
        do {
          short seg_len = (short)(total_len - seg_start);
          if (seg_len > wrap_w) seg_len = wrap_w;

          if (current_line >= first_line && line_y + sys_char_h <= bottom) {
            short copy_start = first_col;
            short copy_len = seg_len;
            if (copy_start > copy_len) copy_start = copy_len;
            copy_len = (short)(copy_len - copy_start);
            memcpy(line_buf, p + seg_start + copy_start, (size_t)copy_len);
            line_buf[copy_len] = '\0';
            v_gtext(aes_handle, left, line_y, line_buf);
            line_y = (short)(line_y + line_h);
          }
          current_line++;
          seg_start = (short)(seg_start + wrap_w);
        } while (seg_start < total_len);

        /* Ensure at least one visual row for empty lines. */
        if (total_len == 0) {
          if (current_line >= first_line && line_y + sys_char_h <= bottom) {
            v_gtext(aes_handle, left, line_y, "");
            line_y = (short)(line_y + line_h);
          }
          current_line++;
        }
      }

      if (*eol == '\0') {
        break;
      }
      p = eol + 1;
    }
  } else {
    v_gtext(aes_handle, left, line_y, "Code > Run... to execute the code");
  }
}

static void redraw_code_window(void) {
  if (code_win < 0) {
    return;
  }
  textedit_redraw_all(&code_te);
}

static void redraw_result_window(void) {
  short clip_x, clip_y, clip_w, clip_h;
  short work_x, work_y, work_w, work_h;
  short clip[4];

  if (result_win < 0) {
    return;
  }

  wind_update(BEG_UPDATE);
  graf_mouse(M_OFF, NULL);

  wind_get(result_win, WF_WORKXYWH, &work_x, &work_y, &work_w, &work_h);
  wind_get(result_win, WF_FIRSTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  while (clip_w > 0 && clip_h > 0) {
    clip[0] = clip_x;
    clip[1] = clip_y;
    clip[2] = (short)(clip_x + clip_w - 1);
    clip[3] = (short)(clip_y + clip_h - 1);

    vs_clip(aes_handle, 1, clip);
    fill_clip_rect(clip_x, clip_y, clip_w, clip_h);
    draw_multiline_text(current_result, (short)(work_x + 4), work_y,
                        (short)(work_y + work_h - 4), result_top_line,
                        result_left_col);
    vs_clip(aes_handle, 0, clip);

    wind_get(result_win, WF_NEXTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  }

  graf_mouse(M_ON, NULL);
  graf_mouse(ARROW, NULL);
  wind_update(END_UPDATE);
  update_window_sliders(result_win, current_result, result_top_line,
                        result_left_col);
}

static void do_redraw(short win) {
  if (win == code_win) {
    redraw_code_window();
  } else if (win == result_win) {
    redraw_result_window();
  }
}

static short run_dialog(short *async_out) {
  short x, y, w, h;
  short exit_obj;

  strcpy(dialog_args, "[]");

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  form_center(dialog_tree, &x, &y, &w, &h);
  form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
  form_dial(FMD_GROW, 0, 0, 0, 0, x, y, w, h);

  objc_draw(dialog_tree, ROOT, MAX_DEPTH, x, y, w, h);
  exit_obj = (short)(form_do(dialog_tree, DL_FUNC_INPUT) & 0x7FFF);
  dialog_tree[exit_obj].ob_state &= ~OS_SELECTED;

  if (async_out) {
    *async_out =
        (short)((dialog_tree[DL_ASYNC_YES].ob_state & OS_SELECTED) != 0);
  }

  form_dial(FMD_SHRINK, x, y, w, h, 0, 0, 0, 0);
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  /* FMD_FINISH must run with the update lock released so the AES actually
     redraws the window frames it exposes; holding BEG_UPDATE across it left
     the code/result border band (now under the dialog centre) unpainted. */
  form_dial(FMD_FINISH, x, y, w, h, 0, 0, 0, 0);
  redraw_all();
  graf_mouse(ARROW, NULL);

  return (short)(exit_obj == DL_OK);
}

/* Flatten the TexEdit buffer back into a single string for upload */
static void build_upload_buf(char *buf, int buflen) {
  short r, n;
  char line[TE_MAX_LINE_LEN + 1];
  int pos = 0;

  n = textedit_get_line_count(&code_te);
  for (r = 0; r < n; r++) {
    short len = textedit_get_line(&code_te, r, line, (short)sizeof(line));
    if (len < 0) len = 0;
    if (pos + len + 1 >= buflen) break;
    memcpy(buf + pos, line, len);
    pos += len;
    if (r < n - 1) {
      buf[pos++] = '\n';
    }
  }
  buf[pos] = '\0';
}

static void do_run(void) {
  short err;
  short async_mode = 0;
  char args_input[ARGS_LEN + 4];
  char result[2048];
  static char upload_buf[TE_MAX_LINES * (TE_MAX_LINE_LEN + 1)];

  if (!run_dialog(&async_mode)) {
    return;
  }

  {
    short i, end;

    strcpy(args_input, dialog_args);
    end = (short)strlen(args_input);
    for (i = (short)(end - 1); i >= 0; i--) {
      if (args_input[i] == ' ' || args_input[i] == '_') {
        args_input[i] = '\0';
      } else {
        break;
      }
    }
    if (args_input[0] == '\0') {
      strcpy(args_input, "[]");
    }
  }

  err = (short)mdjs_ping();
  if (err != 0) {
    set_result_text("Error: no SidecarT or MD/JS not loaded.");
    present_result_window();
    return;
  }

  build_upload_buf(upload_buf, (int)sizeof(upload_buf));

  err = (short)mdjs_upload(upload_buf);
  if (err != 0) {
    set_result_error_with_fallback("Error: upload failed.");
    present_result_window();
    return;
  }

  memset(result, 0, sizeof(result));
  {
    short i, end;
    end = (short)strlen(dialog_func);
    for (i = (short)(end - 1); i >= 0; i--) {
      if (dialog_func[i] == ' ' || dialog_func[i] == '_') {
        dialog_func[i] = '\0';
      } else {
        break;
      }
    }
    if (dialog_func[0] == '\0') strcpy(dialog_func, "main");
  }

  if (async_mode) {
    err = (short)mdjs_call_async(dialog_func, args_input);
    if (err != 0) {
      set_result_error_with_fallback("Error: async call failed.");
      present_result_window();
      return;
    }

    set_result_text("Running code...");
    present_result_window();
    async_call_pending = 1;
    return;
  }

  err = (short)mdjs_call(dialog_func, args_input, result, (int)sizeof(result));
  if (err != 0) {
    set_result_error_with_fallback("Error: call failed.");
    present_result_window();
    return;
  }

  set_result_text(result);
  present_result_window();
}

static void do_about(void) {
  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  form_alert(1,
             "[1][MD/JS Code"
             "| "
             "|Want to embed MD/JS in your"
             "|own ST apps? Visit"
             "|github.com/neilrackett/md-js][OK]");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);

  redraw_all();
  graf_mouse(ARROW, NULL);
}

static void do_save(void) {
  short exit_btn;
  char save_file[13] = "";
  char save_path[128];
  char full[128 + 13];
  char *slash;
  long fh;
  static char line_buf[TE_MAX_LINE_LEN + 2];
  short r, n, len;

  fsel_init_path();

  /* Build a save-path using the current fsel directory but no wildcard */
  strcpy(save_path, fsel_path);
  slash = save_path;
  {
    char *p = save_path;
    while (*p) {
      if (*p == '\\') slash = p;
      p++;
    }
  }
  slash[1] = '\0';
  strcat(save_path, "*.JS");

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  fsel_exinput(save_path, save_file, &exit_btn, "Save JavaScript file");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  redraw_all();
  graf_mouse(ARROW, NULL);

  if (exit_btn != 1 || save_file[0] == '\0') {
    return;
  }

  /* Build full path */
  strcpy(full, save_path);
  slash = full;
  {
    char *p = full;
    while (*p) {
      if (*p == '\\') slash = p;
      p++;
    }
  }
  slash[1] = '\0';
  strcat(full, save_file);

  fh = Fcreate(full, 0);
  if (fh < 0) {
    form_alert(1, "[1][Could not create file.][OK]");
    return;
  }

  n = textedit_get_line_count(&code_te);
  for (r = 0; r < n; r++) {
    len = textedit_get_line(&code_te, r, line_buf, TE_MAX_LINE_LEN + 1);
    if (len < 0) len = 0;
    if (r < n - 1) {
      line_buf[len] = '\r';
      line_buf[len + 1] = '\n';
      len += 2;
    }
    Fwrite((short)fh, (long)len, line_buf);
  }

  Fclose((short)fh);

  /* Update shared fsel path to the saved directory */
  {
    short dirlen = (short)(slash - full + 1);
    memcpy(fsel_path, full, dirlen);
    strcpy(fsel_path + dirlen, "*.JS");
  }
}

static void fsel_init_path(void) {
  short drive, len;

  if (fsel_path[0] != '\0') return;

  drive = Dgetdrv();
  fsel_path[0] = (char)('A' + drive);
  fsel_path[1] = ':';
  fsel_path[2] = '\\';
  Dgetpath(fsel_path + 3, (short)(drive + 1));
  len = (short)strlen(fsel_path);
  if (fsel_path[len - 1] != '\\') {
    fsel_path[len] = '\\';
    fsel_path[len + 1] = '\0';
  }
  strcat(fsel_path, "*.JS");
}

static void do_load(void) {
  short exit_btn;

  fsel_init_path();

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  fsel_exinput(fsel_path, fsel_file, &exit_btn, "Load JavaScript file");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  redraw_all();
  graf_mouse(ARROW, NULL);

  if (exit_btn != 1 || fsel_file[0] == '\0') {
    return;
  }

  {
    /* Build full path: fsel_path ends at last backslash (or is bare drive),
       append the filename returned in fsel_file. */
    char full[128 + 13];
    char *slash;
    short len;
    long fh;
    long file_size;
    char *buf;
    static char line_buf[TE_MAX_LINE_LEN + 1];
    long bytes_read;

    strcpy(full, fsel_path);
    slash = full;
    {
      char *p = full;
      while (*p) {
        if (*p == '\\') slash = p;
        p++;
      }
    }
    slash[1] = '\0';
    strcat(full, fsel_file);

    fh = Fopen(full, 0);
    if (fh < 0) {
      form_alert(1, "[1][Could not open file.][OK]");
      return;
    }

    file_size = Fseek(0L, (short)fh, 2);
    Fseek(0L, (short)fh, 0);

    if (file_size <= 0 ||
        file_size > (long)(TE_MAX_LINES * (TE_MAX_LINE_LEN + 1))) {
      form_alert(1, "[1][File is empty or too large.][OK]");
      Fclose((short)fh);
      return;
    }

    buf = (char *)Malloc(file_size + 1);
    if (!buf) {
      form_alert(1, "[1][Not enough memory.][OK]");
      Fclose((short)fh);
      return;
    }

    bytes_read = Fread((short)fh, file_size, buf);
    Fclose((short)fh);
    buf[bytes_read > 0 ? bytes_read : 0] = '\0';

    /* Split on newlines and load into editor.
       Zero num_lines directly so append_line fills from row 0 without
       the ghost empty line that textedit_clear intentionally leaves. */
    textedit_clear(&code_te);
    code_te.num_lines = 0;
    {
      char *p = buf;
      char *eol;
      short line_len;

      while (*p) {
        eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        line_len = (short)(eol - p);
        if (line_len > TE_MAX_LINE_LEN) line_len = TE_MAX_LINE_LEN;
        memcpy(line_buf, p, line_len);
        line_buf[line_len] = '\0';
        textedit_append_line(&code_te, line_buf);
        if (*eol == '\r' && *(eol + 1) == '\n') eol++;
        p = (*eol) ? eol + 1 : eol;
      }
    }

    Mfree(buf);

    /* Update the fsel path to point to the directory we just loaded from */
    len = (short)(slash - full + 1);
    memcpy(fsel_path, full, len);
    strcpy(fsel_path + len, "*.JS");

    textedit_update_sliders(&code_te);
    textedit_redraw_all(&code_te);
  }
}

static void scroll_window(short win, const char *text, short *top_line,
                          short delta) {
  short max_top = max_top_line_for_window(win, text);
  short new_top = (short)(*top_line + delta);

  if (new_top < 0) {
    new_top = 0;
  } else if (new_top > max_top) {
    new_top = max_top;
  }

  if (new_top == *top_line) {
    return;
  }

  *top_line = new_top;
  do_redraw(win);
}

static void slider_window(short win, const char *text, short *top_line,
                          short pos) {
  short max_top = max_top_line_for_window(win, text);
  short new_top;

  if (max_top <= 0) {
    new_top = 0;
  } else {
    new_top = (short)((pos * max_top + 500L) / 1000L);
  }

  if (new_top == *top_line) {
    return;
  }

  *top_line = new_top;
  do_redraw(win);
}

static void hscroll_window(short win, const char *text, short *left_col,
                           short delta) {
  short max_left = max_left_col_for_window(win, text);
  short new_left = (short)(*left_col + delta);

  if (new_left < 0) {
    new_left = 0;
  } else if (new_left > max_left) {
    new_left = max_left;
  }

  if (new_left == *left_col) {
    return;
  }

  *left_col = new_left;
  do_redraw(win);
}

static void hslider_window(short win, const char *text, short *left_col,
                           short pos) {
  short max_left = max_left_col_for_window(win, text);
  short new_left;

  if (max_left <= 0) {
    new_left = 0;
  } else {
    new_left = (short)((pos * max_left + 500L) / 1000L);
  }

  if (new_left == *left_col) {
    return;
  }

  *left_col = new_left;
  do_redraw(win);
}

static const char *const new_code_lines[] = {"function main() {",
                                             "  /* Edit me! */", "}"};
#define NEW_CODE_LINE_COUNT 3

static void do_new(void) {
  textedit_clear(&code_te);
  code_te.num_lines = 0;
  textedit_set_text(&code_te, new_code_lines, NEW_CODE_LINE_COUNT);
  textedit_update_sliders(&code_te);
  textedit_redraw_all(&code_te);
}

static void handle_menu_selected(short title, short item) {
  switch (item) {
    case MENU_I_ABOUT:
      do_about();
      break;
    case MENU_I_NEW:
      do_new();
      break;
    case MENU_I_LOAD:
      do_load();
      break;
    case MENU_I_SAVE:
      do_save();
      break;
    case MENU_I_QUIT:
      quit_requested = 1;
      break;
    case MENU_I_RUN:
      do_run();
      break;
    default:
      break;
  }
  menu_tnormal(menu_tree, title, 1);
}

static void handle_code_arrowed(short direction) {
  short wx, wy, ww, wh;
  short vis_rows, vis_cols;

  wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  vis_rows = (code_te.cell_h > 0) ? wh / code_te.cell_h : 1;
  vis_cols = (code_te.cell_w > 0) ? ww / code_te.cell_w : 1;

  switch (direction) {
    case WA_UPLINE:
      code_te.vscroll--;
      break;
    case WA_DNLINE:
      code_te.vscroll++;
      break;
    case WA_UPPAGE:
      code_te.vscroll -= vis_rows;
      break;
    case WA_DNPAGE:
      code_te.vscroll += vis_rows;
      break;
    case WA_LFLINE:
      code_te.hscroll--;
      break;
    case WA_RTLINE:
      code_te.hscroll++;
      break;
    case WA_LFPAGE:
      code_te.hscroll -= vis_cols;
      break;
    case WA_RTPAGE:
      code_te.hscroll += vis_cols;
      break;
  }
  textedit_update_sliders(&code_te);
  textedit_redraw_all(&code_te);
}

static void poll_async_result(void) {
  char result[2048];
  unsigned char status;

  if (!async_call_pending) return;

  status = mdjs_status();
  if (status == MDJS_STATUS_BUSY) return;

  async_call_pending = 0;

  if (mdjs_result(result, (int)sizeof(result)) != 0 || result[0] == '\0') {
    set_result_text("Error: empty result.");
  } else {
    set_result_text(result);
  }
  present_result_window();
}

static void event_loop(void) {
  short ev;
  short msg[8];
  short mx, my, mb, ks, key, clicks;
  short last_mb;

  redraw_code_window();
  redraw_result_window();
  graf_mkstate(&mx, &my, &last_mb, &ks);

  while (!quit_requested) {
    ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_TIMER, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, msg, (long)TE_BLINK_MS, &mx, &my, &mb, &ks,
                    &key, &clicks);

    if (last_mb == 0 && mb != 0) {
      short wx, wy, ww, wh;

      if (code_win >= 0) {
        wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
        if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh) {
          textedit_click(&code_te, mx, my);
        }
      }
    }
    last_mb = mb;

    if (ev & MU_TIMER) {
      textedit_blink(&code_te);
      poll_async_result();
    }

    if (ev & MU_KEYBD) {
      TEDirty batch;
      short peek_ev;
      short peek_msg[8];
      short peek_mx, peek_my, peek_mb, peek_ks, peek_key, peek_clicks;

      code_te.cursor_visible = 1;
      batch = textedit_apply_key(&code_te, key);

      /* Drain queued keystrokes to reduce flicker */
      do {
        peek_ev = evnt_multi(MU_KEYBD | MU_TIMER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, peek_msg, 0L, &peek_mx, &peek_my,
                             &peek_mb, &peek_ks, &peek_key, &peek_clicks);
        if (peek_ev & MU_KEYBD) {
          TEDirty d = textedit_apply_key(&code_te, peek_key);
          TE_DIRTY_UNION(batch, d);
        }
      } while (peek_ev & MU_KEYBD);

      textedit_finish_edit(&code_te, batch);
    }

    if (!(ev & MU_MESAG)) {
      continue;
    }

    switch (msg[0]) {
      case MN_SELECTED:
        handle_menu_selected(msg[3], msg[4]);
        break;

      case WM_REDRAW:
        if (msg[3] == code_win) {
          short area[4];
          area[0] = msg[4];
          area[1] = msg[5];
          area[2] = msg[6];
          area[3] = msg[7];
          textedit_redraw(&code_te, area);
        } else {
          do_redraw(msg[3]);
        }
        break;

      case WM_ARROWED:
        if (msg[3] == code_win) {
          handle_code_arrowed(msg[4]);
        } else if (msg[3] == result_win) {
          switch (msg[4]) {
            case WA_UPLINE:
              scroll_window(result_win, current_result, &result_top_line, -1);
              break;
            case WA_DNLINE:
              scroll_window(result_win, current_result, &result_top_line, 1);
              break;
            case WA_UPPAGE:
              scroll_window(result_win, current_result, &result_top_line,
                            -visible_lines_for_window(result_win));
              break;
            case WA_DNPAGE:
              scroll_window(result_win, current_result, &result_top_line,
                            visible_lines_for_window(result_win));
              break;
            case WA_LFLINE:
              hscroll_window(result_win, current_result, &result_left_col, -1);
              break;
            case WA_RTLINE:
              hscroll_window(result_win, current_result, &result_left_col, 1);
              break;
            case WA_LFPAGE:
              hscroll_window(result_win, current_result, &result_left_col,
                             -visible_columns_for_window(result_win));
              break;
            case WA_RTPAGE:
              hscroll_window(result_win, current_result, &result_left_col,
                             visible_columns_for_window(result_win));
              break;
          }
        }
        break;

      case WM_VSLID:
        if (msg[3] == code_win) {
          short wx, wy, ww, wh;
          short vmax;
          wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
          vmax = code_te.num_lines -
                 ((code_te.cell_h > 0) ? wh / code_te.cell_h : 1);
          if (vmax < 0) vmax = 0;
          code_te.vscroll = (short)((long)msg[4] * vmax / 1000L);
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else if (msg[3] == result_win) {
          slider_window(result_win, current_result, &result_top_line, msg[4]);
        }
        break;

      case WM_HSLID:
        if (msg[3] == code_win) {
          short wx, wy, ww, wh;
          short hmax;
          short max_w = 0;
          short i;
          wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
          for (i = 0; i < code_te.num_lines; i++)
            if (code_te.line_len[i] > max_w) max_w = code_te.line_len[i];
          hmax = max_w + 1 - ((code_te.cell_w > 0) ? ww / code_te.cell_w : 1);
          if (hmax < 0) hmax = 0;
          code_te.hscroll = (short)((long)msg[4] * hmax / 1000L);
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else if (msg[3] == result_win) {
          hslider_window(result_win, current_result, &result_left_col, msg[4]);
        }
        break;

      case WM_SIZED:
      case WM_MOVED:
        wind_set(msg[3], WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
        if (msg[3] == code_win) {
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else {
          do_redraw(msg[3]);
        }
        break;

      case WM_TOPPED:
        wind_set(msg[3], WF_TOP, 0, 0, 0, 0);
        break;

      case WM_CLOSED:
        quit_requested = 1;
        break;

      default:
        break;
    }
  }
}

/* Redraw the menu bar. form_dial()/form_alert() draw over the bar and the
   AES only restores the desktop underneath, not the bar contents, so every
   dialog-close path has to repaint it. Drawing from MENU_BAR_BOX covers the
   bar box and its title children but not the drop-down boxes (siblings under
   MENU_SCREEN), and the clip rect keeps it to the bar row.

   After painting the titles normal we call menu_tnormal() for each one so the
   AES's own "is this title highlighted" flag matches what is on screen. Without
   this the AES still believes the selected title is highlighted, and its next
   hover XOR-toggles from that stale baseline, leaving titles stuck black/white
   until another dialog's mouse-control cycle resets the menu state machine. */
static void redraw_menu(void) {
  short bar_w = menu_tree[MENU_ROOT].ob_width;
  short bar_h =
      (short)(menu_tree[MENU_BAR_BOX].ob_y + menu_tree[MENU_BAR_BOX].ob_height);

  wind_update(BEG_UPDATE);
  graf_mouse(M_OFF, NULL);
  objc_draw(menu_tree, MENU_BAR_BOX, MAX_DEPTH, 0, 0, bar_w, bar_h);
  graf_mouse(M_ON, NULL);
  graf_mouse(ARROW, NULL);
  wind_update(END_UPDATE);

  menu_tnormal(menu_tree, MENU_T_MDJS, 1);
  menu_tnormal(menu_tree, MENU_T_FILE, 1);
  menu_tnormal(menu_tree, MENU_T_CODE, 1);
}

/* Force the AES to repaint a window's whole frame (title bar, scroll bars and
   border edges). redraw_all() only repaints work areas, so when a dialog draws
   over the shared border band between the two tiled windows, TOS 2.06 leaves
   the frame damaged and offers no single "redraw frame" call. Momentarily
   shrinking then restoring the window makes the AES redraw the full frame; the
   work area is repainted by the caller afterwards. */
static void force_frame_redraw(short win) {
  short x, y, w, h;
  if (win < 0) {
    return;
  }
  wind_get(win, WF_CURRXYWH, &x, &y, &w, &h);
  wind_set(win, WF_CURRXYWH, x, y, w, (short)(h - 2));
  wind_set(win, WF_CURRXYWH, x, y, w, h);
}

static void redraw_all(void) {
  redraw_menu();
  force_frame_redraw(code_win);
  force_frame_redraw(result_win);
  redraw_code_window();
  textedit_update_sliders(&code_te);
  redraw_result_window();
}

static void focus_result_window(void) {
  if (code_win >= 0) {
    wind_set(code_win, WF_TOP, 0, 0, 0, 0);
  }
  if (result_win >= 0) {
    wind_set(result_win, WF_TOP, 0, 0, 0, 0);
  }
}

static void present_result_window(void) {
  redraw_all();
  focus_result_window();
  redraw_all();
}

int main(void) {
  app_id = appl_init();
  if (app_id < 0) {
    return 1;
  }

  aes_handle = graf_handle(&sys_char_w, &sys_char_h, &sys_cell_w, &sys_cell_h);

  build_dialog();
  build_menu();
  menu_bar(menu_tree, 1);
  open_windows();
  event_loop();

  menu_bar(menu_tree, 0);

  if (code_win >= 0) {
    wind_close(code_win);
    wind_delete(code_win);
  }
  if (result_win >= 0) {
    wind_close(result_win);
    wind_delete(result_win);
  }

  if (virt_vdi >= 0) {
    v_clsvwk(virt_vdi);
  }

  appl_exit();
  return 0;
}
