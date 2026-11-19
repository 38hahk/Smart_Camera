/*******************************************************************************
 * Size: 24 px
 * Bpp: 4
 * Opts: --font iconfont.ttf --size 24 --bpp 4 -r 0xe781 --format lvgl -o ui_font_iconfont24.c
 ******************************************************************************/

#include "../ui.h"

#ifndef UI_FONT_ICONFONT24
#define UI_FONT_ICONFONT24 1
#endif

#if UI_FONT_ICONFONT24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E781 "" */
    0x0, 0xff, 0xea, 0x14, 0x67, 0x73, 0x6d, 0xc4,
    0x3, 0xff, 0x86, 0x57, 0xae, 0x62, 0x22, 0x48,
    0xe9, 0x0, 0xff, 0xe0, 0xa6, 0xa0, 0x7, 0xf3,
    0x69, 0x0, 0x7f, 0x92, 0xc0, 0x3f, 0xf8, 0x5,
    0x82, 0x1, 0xfd, 0x40, 0x1f, 0xfc, 0x21, 0xb0,
    0xf, 0xce, 0x20, 0x1f, 0xfc, 0x35, 0x20, 0xf,
    0xac, 0x3, 0xff, 0x8b, 0x40, 0x1f, 0x18, 0x7,
    0xff, 0x15, 0xf9, 0x80, 0x3f, 0xf9, 0x83, 0x36,
    0x1, 0x88, 0x3, 0xff, 0x90, 0x94, 0x1, 0x28,
    0x7, 0xff, 0x25, 0x50, 0x1, 0x20, 0x1f, 0xfc,
    0xae, 0x0, 0xc, 0x0, 0x7f, 0xf2, 0x50, 0x2,
    0x77, 0x0, 0x7f, 0xf2, 0x4, 0x3, 0x42, 0x0,
    0x7f, 0xf1, 0xc8, 0x3, 0x90, 0x3, 0xff, 0x8e,
    0x80, 0x1a, 0x80, 0x28, 0xf5, 0x0, 0xff, 0xe1,
    0x58, 0x4, 0x6e, 0x6f, 0xce, 0x16, 0x20, 0x1f,
    0xfc, 0x9, 0x20, 0x8, 0xf7, 0x20, 0x40, 0x3,
    0xe4, 0x1, 0xfe, 0x96, 0x0, 0xff, 0xe0, 0xe,
    0xc0, 0x80, 0x7e, 0x20, 0xf, 0xfe, 0x1b, 0xf6,
    0xd4, 0xd5, 0x90, 0x2, 0x0, 0x3f, 0xf8, 0xa4,
    0xac, 0xa9, 0xb2, 0x84, 0xc0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 384, .box_w = 26, .box_h = 23, .ofs_x = -1, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 59265, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_iconfont24 = {
#else
lv_font_t ui_font_iconfont24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 23,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_ICONFONT24*/

