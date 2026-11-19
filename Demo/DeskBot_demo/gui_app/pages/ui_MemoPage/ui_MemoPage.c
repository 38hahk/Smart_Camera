/*
 * 备忘录页面
 *
 * 入口：桌面的「备忘录」图标（ui_HomePage.c 的 ui_MemoBtn → "MemoPage"）
 *
 * 两个状态（同一页里切控件，不重建页面）：
 *   列表态：返回 / 备忘录 / 新增   +   一列可滚动的备忘（点一条进编辑）
 *   编辑态：取消 / 删除 / 完成     +   文本域 + LVGL 自带屏幕键盘
 *
 * 存储：工作目录下的 memo.txt（板端 = /root/bin/memo.txt，仿真 = bin/memo.txt）
 *   一行一条，格式 "<unix时间戳>\t正文"；没有时间戳的行也能读（列表里显示 "--"）。
 *   正文里的换行转义成 \n、反斜杠转义成 \\ → 一条永远占一行，
 *   所以这个文件既能让程序读写，也能直接在电脑/SSH 上用记事本编辑。
 *   保存走 先写 .tmp 再 rename，避免写一半断电把备忘录整锅端了。
 *
 * ⚠️ 屏幕键盘只有 ASCII（LVGL 自带 QWERTY，没有中文输入法）：
 *   想存中文就在电脑上编辑 memo.txt —— 显示中文没问题（ui_font_app16 是 GB2312
 *   一级字库），只是没法在屏上打字。
 *
 * 无屏自检：ECHO_MEMO_SELFTEST=1（2 秒一步：新增→打字→完成→点开→删除）
 */

#include "ui_MemoPage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

///////////////////// 参数 ////////////////////

#define MEMO_MAX    30              /* 最多几条（NAND 上内存小，够用就行） */
#define MEMO_LEN    192             /* 一条正文最长字节（UTF-8 中文 3 字节/字） */
#define MEMO_FILE   "memo.txt"
#define MEMO_TMP    "memo.txt.tmp"

///////////////////// 状态 ////////////////////

typedef struct {
    long ts;                        /* 最后一次修改时间；0 = 没有（手写进去的） */
    char text[MEMO_LEN];
} memo_t;

static memo_t memos[MEMO_MAX];
static int    memo_count = 0;
static int    edit_index = -1;      /* 正在编辑第几条；-1 = 新增 */

static lv_obj_t *memo_page;
static lv_obj_t *list_bar, *edit_bar;
static lv_obj_t *memo_list, *memo_hint;
static lv_obj_t *memo_ta, *memo_kb, *memo_del_btn;
static lv_obj_t *memo_add_btn, *memo_done_btn;

///////////////////// 存储 ////////////////////

/* 正文 → 单行（换行 → \n，反斜杠 → \\），这样一条永远占一行 */
static void memo_escape(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < n; p++) {
        if (*p == '\\')       { dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (*p == '\n')  { dst[o++] = '\\'; dst[o++] = 'n';  }
        else if (*p == '\r')  { continue; }         /* 忽略 CR */
        else                  { dst[o++] = *p; }
    }
    dst[o] = 0;
}

/* 上面那个的逆操作（原地做，结果只会变短） */
static void memo_unescape(char *s)
{
    char *w = s;
    for (char *p = s; *p; ) {
        if (p[0] == '\\' && p[1] == '\\')      { *w++ = '\\'; p += 2; }
        else if (p[0] == '\\' && p[1] == 'n')  { *w++ = '\n'; p += 2; }
        else                                    { *w++ = *p++; }
    }
    *w = 0;
}

static void memo_load(void)
{
    memo_count = 0;

    FILE *fp = fopen(MEMO_FILE, "r");
    if (!fp) {
        printf("[MemoPage] 还没有 %s，从空的开始\n", MEMO_FILE);
        return;
    }

    char line[MEMO_LEN * 2];
    while (fgets(line, sizeof(line), fp) && memo_count < MEMO_MAX) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) continue;

        long ts = 0;
        char *text = line;
        char *tab  = strchr(line, '\t');
        if (tab && tab != line) {
            int all_digit = 1;
            for (char *q = line; q < tab; q++)
                if (!isdigit((unsigned char)*q)) { all_digit = 0; break; }
            if (all_digit) { *tab = 0; ts = atol(line); text = tab + 1; }
        }
        memo_unescape(text);
        if (text[0] == 0) continue;                 /* 空行/只有时间戳的丢掉 */

        memos[memo_count].ts = ts;
        /* 用 %.Ns 显式限定长度：行缓冲比一条正文长，不改会用 -Wformat-truncation 报警 */
        snprintf(memos[memo_count].text, MEMO_LEN, "%.*s", MEMO_LEN - 1, text);
        memo_count++;
    }
    fclose(fp);
    printf("[MemoPage] 载入 %d 条备忘（%s）\n", memo_count, MEMO_FILE);
}

static void memo_save(void)
{
    FILE *fp = fopen(MEMO_TMP, "w");
    if (!fp) {
        printf("[MemoPage] 打不开 %s（目录只读？）\n", MEMO_TMP);
        return;
    }
    char esc[MEMO_LEN * 2];
    for (int i = 0; i < memo_count; i++) {
        memo_escape(esc, sizeof(esc), memos[i].text);
        fprintf(fp, "%ld\t%s\n", memos[i].ts, esc);
    }
    fclose(fp);
    /* 先写临时文件再改名：写一半掉电也只会丢这一次改动，不会把整个文件写坏 */
    if (rename(MEMO_TMP, MEMO_FILE) != 0)
        printf("[MemoPage] rename 失败，%s 没生效\n", MEMO_FILE);
    else
        printf("[MemoPage] 已保存 %d 条 → %s\n", memo_count, MEMO_FILE);
}

///////////////////// 列表 ////////////////////

static void memo_build_list(void)
{
    lv_obj_clean(memo_list);

    if (memo_count == 0) {
        lv_obj_clear_flag(memo_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(memo_hint, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < memo_count; i++) {
        lv_obj_t *row = lv_button_create(memo_list);
        lv_obj_set_width(row, 300);
        lv_obj_set_height(row, 46);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1E2836), 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* 第一行：时间（没有就 "--"） */
        char tb[32];
        if (memos[i].ts > 0) {
            time_t t = (time_t)memos[i].ts;
            struct tm *lt = localtime(&t);
            if (lt) strftime(tb, sizeof(tb), "%m-%d %H:%M", lt);
            else    snprintf(tb, sizeof(tb), "%ld", memos[i].ts);
        } else {
            snprintf(tb, sizeof(tb), "--");
        }
        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, tb);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0x8FA4B4), 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, 0);

        /* 第二行：正文（换行在列表里显示成空格，太长用省略号收尾） */
        char one[MEMO_LEN];
        snprintf(one, sizeof(one), "%s", memos[i].text);
        for (char *q = one; *q; q++) if (*q == '\n') *q = ' ';
        lv_obj_t *x = lv_label_create(row);
        lv_label_set_text(x, one);
        lv_obj_set_style_text_font(x, &ui_font_app16, 0);
        lv_obj_set_style_text_color(x, lv_color_white(), 0);
        lv_obj_set_width(x, 288);
        lv_label_set_long_mode(x, LV_LABEL_LONG_DOT);
        lv_obj_align(x, LV_ALIGN_BOTTOM_LEFT, 2, 0);

        lv_obj_add_event_cb(row, memo_event_row, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    }
}

///////////////////// 状态切换 ////////////////////

static void memo_show_list(void)
{
    edit_index = -1;
    memo_build_list();

    lv_obj_clear_flag(list_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(memo_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(edit_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(memo_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(memo_kb, LV_OBJ_FLAG_HIDDEN);
    if (memo_count == 0) lv_obj_clear_flag(memo_hint, LV_OBJ_FLAG_HIDDEN);
}

static void memo_show_edit(int idx)
{
    edit_index = idx;

    if (idx >= 0) {
        lv_textarea_set_text(memo_ta, memos[idx].text);
        lv_obj_clear_flag(memo_del_btn, LV_OBJ_FLAG_HIDDEN);   /* 编辑已有 → 可以删 */
        printf("[MemoPage] 编辑第 %d 条\n", idx + 1);
    } else {
        lv_textarea_set_text(memo_ta, "");
        lv_obj_add_flag(memo_del_btn, LV_OBJ_FLAG_HIDDEN);     /* 新增 → 没得删 */
        printf("[MemoPage] 新建一条\n");
    }

    lv_obj_add_flag(list_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(memo_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(memo_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(edit_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(memo_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(memo_kb, LV_OBJ_FLAG_HIDDEN);

    lv_keyboard_set_textarea(memo_kb, memo_ta);                /* 键盘的输出接到文本域 */
}

/* 完成：写回并回到列表。正文全空白时，编辑已有 = 删除，新增 = 直接丢掉 */
static void memo_commit(void)
{
    const char *txt = lv_textarea_get_text(memo_ta);
    if (txt == NULL) txt = "";

    int blank = 1;
    for (const char *p = txt; *p; p++)
        if (!isspace((unsigned char)*p)) { blank = 0; break; }

    if (blank) {
        if (edit_index >= 0) {
            printf("[MemoPage] 内容清空 → 删除第 %d 条\n", edit_index + 1);
            for (int i = edit_index; i < memo_count - 1; i++) memos[i] = memos[i + 1];
            memo_count--;
            memo_save();
        }
    } else if (edit_index >= 0) {
        snprintf(memos[edit_index].text, MEMO_LEN, "%s", txt);
        memos[edit_index].ts = (long)time(NULL);
        memo_save();
    } else if (memo_count < MEMO_MAX) {
        memos[memo_count].ts = (long)time(NULL);
        snprintf(memos[memo_count].text, MEMO_LEN, "%s", txt);
        memo_count++;
        memo_save();
    } else {
        ui_msgbox_info("备忘录", "最多 30 条, 先删掉几条再新增");
        return;                                     /* 不切回列表，让人能改短一点 */
    }
    memo_show_list();
}

///////////////////// 事件 ////////////////////

void memo_event_row(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= memo_count) return;
    memo_show_edit(idx);
}

static void memo_event_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_lib_pm_OpenPrePage(&page_manager);           /* 回桌面 */
}

static void memo_event_add(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    memo_show_edit(-1);
}

static void memo_event_done(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    memo_commit();
}

static void memo_event_cancel(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    printf("[MemoPage] 取消编辑\n");
    memo_show_list();
}

static void memo_event_del(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (edit_index < 0 || edit_index >= memo_count) return;
    printf("[MemoPage] 删除第 %d 条\n", edit_index + 1);
    for (int i = edit_index; i < memo_count - 1; i++) memos[i] = memos[i + 1];
    memo_count--;
    memo_save();
    memo_show_list();
}

/* 键盘上的 ✓（READY）等于「完成」，× （CANCEL）等于「取消」 */
static void memo_event_kb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)       memo_commit();
    else if (code == LV_EVENT_CANCEL) memo_show_list();
}

///////////////////// 自检（无屏时看日志） ////////////////////

static void memo_selftest_cb(lv_timer_t *t)
{
    static int step = 0;
    switch (step++) {
    case 1:
        printf("[selftest] 点 新增\n");
        lv_obj_send_event(memo_add_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 2:
        printf("[selftest] 键盘打字\n");
        lv_textarea_set_text(memo_ta, "buy milk");
        break;
    case 3:
        printf("[selftest] 点 完成（应回到列表并有 1 条）\n");
        lv_obj_send_event(memo_done_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 4:
        printf("[selftest] 点 第一条\n");
        if (lv_obj_get_child(memo_list, 0))
            lv_obj_send_event(lv_obj_get_child(memo_list, 0), LV_EVENT_CLICKED, NULL);
        break;
    case 5:
        printf("[selftest] 点 删除\n");
        lv_obj_send_event(memo_del_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 6:
        printf("[selftest] 结束（列表应为空）\n");
        lv_timer_delete(t);
        break;
    }
}

///////////////////// 页面 ////////////////////

/* 顶部条上的一个按钮 */
static lv_obj_t *memo_top_btn(lv_obj_t *bar, lv_align_t align, lv_coord_t x, const char *txt)
{
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_size(btn, 62, 28);
    lv_obj_align(btn, align, x, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_app16, 0);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *memo_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 34);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_set_style_bg_opa(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

void ui_MemoPage_init(void)
{
    memo_load();

    memo_page = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(memo_page, lv_color_hex(0x101418), 0);

    /* ---------- 顶部：两套按钮，按状态显示 ---------- */
    list_bar = memo_top_bar(memo_page);
    lv_obj_t *back_btn = memo_top_btn(list_bar, LV_ALIGN_LEFT_MID, 3, "返回");
    lv_obj_add_event_cb(back_btn, memo_event_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(list_bar);
    lv_label_set_text(title, "备忘录");
    lv_obj_set_style_text_font(title, &ui_font_app16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    memo_add_btn = memo_top_btn(list_bar, LV_ALIGN_RIGHT_MID, -3, "新增");
    lv_obj_add_event_cb(memo_add_btn, memo_event_add, LV_EVENT_CLICKED, NULL);

    edit_bar = memo_top_bar(memo_page);
    lv_obj_t *cancel_btn = memo_top_btn(edit_bar, LV_ALIGN_LEFT_MID, 3, "取消");
    lv_obj_add_event_cb(cancel_btn, memo_event_cancel, LV_EVENT_CLICKED, NULL);

    memo_del_btn = memo_top_btn(edit_bar, LV_ALIGN_CENTER, 0, "删除");
    lv_obj_set_style_bg_color(memo_del_btn, lv_color_hex(0xB83B5E), 0);
    lv_obj_add_event_cb(memo_del_btn, memo_event_del, LV_EVENT_CLICKED, NULL);

    memo_done_btn = memo_top_btn(edit_bar, LV_ALIGN_RIGHT_MID, -3, "完成");
    lv_obj_add_event_cb(memo_done_btn, memo_event_done, LV_EVENT_CLICKED, NULL);

    /* ---------- 列表（可滚动，flex 竖排） ---------- */
    memo_list = lv_obj_create(memo_page);
    lv_obj_set_size(memo_list, 316, 200);
    lv_obj_align(memo_list, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_opa(memo_list, 0, 0);
    lv_obj_set_style_border_opa(memo_list, 0, 0);
    lv_obj_set_style_pad_all(memo_list, 0, 0);
    lv_obj_set_style_pad_row(memo_list, 6, 0);
    lv_obj_set_flex_flow(memo_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(memo_list, LV_SCROLLBAR_MODE_ACTIVE);

    memo_hint = lv_label_create(memo_page);
    lv_label_set_text(memo_hint, "还没有备忘\n点右上角 新增 写一条");
    lv_obj_set_style_text_font(memo_hint, &ui_font_app16, 0);
    lv_obj_set_style_text_color(memo_hint, lv_color_hex(0x8FA4B4), 0);
    lv_obj_set_style_text_align(memo_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(memo_hint, LV_ALIGN_CENTER, 0, 10);

    /* ---------- 编辑态：文本域 + 屏幕键盘 ---------- */
    memo_ta = lv_textarea_create(memo_page);
    lv_obj_set_size(memo_ta, 306, 62);
    lv_obj_align(memo_ta, LV_ALIGN_TOP_MID, 0, 38);
    lv_textarea_set_max_length(memo_ta, MEMO_LEN - 1);
    lv_obj_set_style_text_font(memo_ta, &ui_font_app16, 0);
    lv_obj_set_style_text_color(memo_ta, lv_color_white(), 0);
    lv_obj_set_style_bg_color(memo_ta, lv_color_hex(0x1E2836), 0);
    lv_obj_add_flag(memo_ta, LV_OBJ_FLAG_HIDDEN);

    memo_kb = lv_keyboard_create(memo_page);
    lv_obj_set_size(memo_kb, 316, 132);
    lv_obj_align(memo_kb, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_keyboard_set_mode(memo_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(memo_kb, memo_event_kb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(memo_kb, LV_OBJ_FLAG_HIDDEN);

    memo_show_list();

    printf("[MemoPage] 进入备忘录（共 %d 条）\n", memo_count);

    const char *st = getenv("ECHO_MEMO_SELFTEST");
    if (st && st[0] == '1') {
        printf("[MemoPage] 自检开始\n");
        lv_timer_create(memo_selftest_cb, 2000, NULL);
    }

    /* ⚠️ 页面管理器不会帮你切屏，每个页面 init 末尾都要自己 load */
    lv_scr_load_anim(memo_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_MemoPage_deinit(void)
{
    list_bar = NULL;
    edit_bar = NULL;
    memo_list = NULL;
    memo_hint = NULL;
    memo_ta = NULL;
    memo_kb = NULL;
    memo_del_btn = NULL;
    memo_add_btn = NULL;
    memo_done_btn = NULL;
    memo_page = NULL;
    printf("[MemoPage] deinit 完成\n");
}
