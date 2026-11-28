#include "app.h"
#include "help-text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gutter_layout(void)
{
	struct app app;
	assert(app_open(&app));
	uint32_t pixels[320 * 200];
	char *lines = malloc(100000);
	assert(lines);
	memset(lines, '\n', 100000);
	const struct {
		size_t lines;
		int digits;
	} layouts[] = {
		{ 9, 4 },     { 10, 4 },     { 99, 4 },	  { 100, 4 },
		{ 999, 4 },   { 1000, 4 },   { 9999, 4 }, { 10000, 5 },
		{ 99999, 5 }, { 100000, 6 },
	};
	for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
		assert(editor_replace(&app.editor, 0, app.editor.length, lines,
				      layouts[i].lines - 1));
		app.editor.cursor = 0;
		assert(app_paint(&app, pixels, 320, 200, 320));
		assert(app.gutter == (layouts[i].digits + 2) * app.cell);
		assert(app_hit_test(&app, app.padding + app.gutter,
				    app.padding) == 0);
		bool number_ink = false;
		int number_x = app.padding + (layouts[i].digits - 1) * app.cell;
		int gap_x = number_x + app.cell;
		for (int y = app.padding; y < app.padding + app.line_height;
		     ++y) {
			for (int x = number_x; x < gap_x; ++x)
				number_ink |= pixels[y * 320 + x] != 0xff18202b;
			for (int x = gap_x; x < app.padding + app.gutter; ++x)
				assert(pixels[y * 320 + x] == 0xff18202b);
		}
		assert(number_ink); /* right-aligned number, then two clear text cells */
	}
	free(lines);
	assert(editor_replace(&app.editor, 0, app.editor.length, "\n", 1));
	app.editor.cursor = 0;
	editor_command(&app.editor, "set nonu");
	assert(app_paint(&app, pixels, 320, 200, 320) && app.gutter == 0);
	editor_command(&app.editor, "set nu");
	assert(app_paint(&app, pixels, 320, 200, 320));
	assert(app.gutter == 6 * app.cell);
	assert(editor_replace(&app.editor, 0, app.editor.length, "one two one",
			      11));
	app.editor.cursor = 0;
	app_input(&app, KEY_TEXT, "/");
	app_input(&app, KEY_TEXT, "one");
	app_input(&app, KEY_ENTER, "");
	editor_command(&app.editor, "noh");
	assert(strstr(app.editor.message, "Unknown command"));
	assert(app_paint(&app, pixels, 320, 200, 320));
	int text_x = app.padding + app.gutter;
	assert(pixels[app.padding * 320 + text_x] == 0xff66502c);
	assert(pixels[app.padding * 320 + text_x + 8 * app.cell] == 0xff66502c);
	app_close(&app);
}

static void search_visibility(void)
{
	for (unsigned raw = 0; raw < 2; ++raw) {
		struct app app;
		assert(app_open(&app));
		if (raw)
			editor_command(&app.editor, "set view=source");
		assert(editor_replace(&app.editor, 0, 0, "sd sd\n", 6));
		app.editor.cursor = 0;
		app_input(&app, KEY_TEXT, "/");
		app_input(&app, KEY_TEXT, "sd");
		app_input(&app, KEY_ENTER, "");
		uint32_t pixels[320 * 200];
		assert(app_paint(&app, pixels, 320, 200, 320));
		int sample = app.padding * 320 + app.text_x;
		assert(pixels[sample] == 0xff66502c);
		app_input(&app, KEY_TEXT, "i");
		assert(app_paint(&app, pixels, 320, 200, 320));
		assert(pixels[sample] == 0xff18202b &&
		       !strcmp(app.editor.search, "sd"));
		app_input(&app, KEY_ESCAPE, "");
		assert(app_paint(&app, pixels, 320, 200, 320));
		assert(pixels[sample] == 0xff18202b);
		app_input(&app, KEY_TEXT, "n");
		assert(app_paint(&app, pixels, 320, 200, 320));
		assert(pixels[sample] == 0xff66502c);
		app_close(&app);
	}
}

/* A toast shows in the status bar, survives keys, wakes the event loop and then clears. */
static void toast(void)
{
	struct app app;
	assert(app_open(&app));
	static uint32_t before[320 * 200], during[320 * 200], after[320 * 200];
	assert(app_paint(&app, before, 320, 200, 320));
	assert(!app_deadline(&app));
	app_toast(&app, "Image copied");
	assert(app.dirty && app_deadline(&app) == app.toast_until &&
	       app.toast_until);
	assert(app_paint(&app, during, 320, 200, 320));
	assert(memcmp(before, during, sizeof(before)));
	app_input(&app, KEY_TEXT, "l");
	assert(!strcmp(app.toast, "Image copied"));
	app.toast_until = 1;
	app_tick(&app);
	assert(!app.toast[0] && !app_deadline(&app));
	assert(app_paint(&app, after, 320, 200, 320));
	assert(!memcmp(before, after, sizeof(before)));
	app_close(&app);
}

/* colorcolumn draws a quiet band at each column, in the theme's color, a fixed step from
 * whatever background is under it. */
static void color_column(void)
{
	struct app app;
	assert(app_open(&app));
	const char *note = "one\n```\ncode\n```\n";
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	enum { WIDTH = 320, HEIGHT = 200 };
	uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
	assert(pixels);
	editor_command(&app.editor, "set cc=10");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	int x = app.text_x + 9 * app.cell + app.cell / 2,
	    y = app.padding + app.line_height / 2,
	    code = y + 2 * app.line_height;
	assert(pixels[y * WIDTH + x] == 0xff1e2633);
	assert(pixels[y * WIDTH + x - app.cell] == 0xff18202b);
	/* The dark code background 0xff242d3b plus the band's step of 6, 6 and 8. */
	assert(pixels[code * WIDTH + x] == 0xff2a3343);
	editor_command(&app.editor, "colorscheme tokyonight");
	editor_command(&app.editor, "set bg=light");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(pixels[y * WIDTH + x] == 0xffd1d2d7);
	editor_command(&app.editor, "set view=reading");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	x = app.text_x + 9 * app.cell + app.cell / 2;
	assert(pixels[y * WIDTH + x] ==
	       0xffe1e2e7); /* not in the reading view */
	editor_command(&app.editor, "set view=normal cc=");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	x = app.text_x + 9 * app.cell + app.cell / 2;
	assert(pixels[y * WIDTH + x] == 0xffe1e2e7);
	free(pixels);
	app_close(&app);
}
/* wrapwidth soft-wraps at a column narrower than the window, and a colorcolumn just past it
 * still shows; the reading view uses it as its measure. */
static void wrap_width(void)
{
	struct app app;
	assert(app_open(&app));
	const char *note = "alpha beta gamma delta epsilon\n";
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	app.editor.cursor = 0;
	enum { WIDTH = 480, HEIGHT = 200 };
	uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
	assert(pixels);
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	unsigned wide = app.editor.viewport_columns;
	assert(wide > 30 && app_view_next(&app, 0) == strlen(note));
	editor_command(&app.editor, "set wrapwidth=12 cc=13");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.editor.viewport_columns == 12 &&
	       app.text_width == 12 * app.cell);
	assert(app_view_next(&app, 0) == strlen("alpha beta "));
	int y = app.padding + app.line_height / 2;
	assert(pixels[y * WIDTH + app.text_x + 12 * app.cell + app.cell / 2] ==
	       0xff1e2633);
	editor_command(&app.editor, "set wrapwidth=1000");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.editor.viewport_columns == wide);
	editor_command(&app.editor, "set view=reading wrapwidth=20");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.editor.viewport_columns == 20);
	editor_command(&app.editor, "set wrapwidth=0");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.editor.viewport_columns > 20 &&
	       app.editor.viewport_columns <= 80);
	free(pixels);
	app_close(&app);
}
static void theme_layout(void)
{
	struct app app;
	assert(app_open(&app));
	assert(!app.editor.light);
	const char *note = "`code` match\n# Heading\n> Quote\n";
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	app.editor.cursor = 0;
	strcpy(app.editor.search, "match");
	enum { WIDTH = 320, HEIGHT = 200, STRIDE = 327 };
	uint32_t *dark = malloc(WIDTH * HEIGHT * sizeof(*dark));
	uint32_t *light = malloc(STRIDE * HEIGHT * sizeof(*light));
	assert(dark && light);
	assert(app_paint(&app, dark, WIDTH, HEIGHT, WIDTH));
	unsigned columns = app.editor.viewport_columns,
		 rows = app.editor.viewport_rows;
	uint64_t revision = app.editor.revision;
	app_input(&app, KEY_TEXT, ":");
	app_input(&app, KEY_TEXT, "set background=light");
	app_input(&app, KEY_ENTER, "");
	assert(app.editor.light && app.editor.revision == revision);
	for (int i = 0; i < STRIDE * HEIGHT; ++i)
		light[i] = 0x12345678;
	assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
	assert(app.editor.viewport_columns == columns &&
	       app.editor.viewport_rows == rows);
	assert(light[0] == 0xfffaf9f6 &&
	       light[(HEIGHT - 1) * STRIDE] == 0xffe5e8eb);
	int x = app.padding + app.gutter, y = app.padding;
	assert(light[y * STRIDE + x + app.cell] ==
	       0xffeeede7); /* inline code */
	assert(light[y * STRIDE + x + 7 * app.cell] == 0xfff5dc8e); /* search */
	for (int row = 0; row < HEIGHT; ++row)
		for (int col = WIDTH; col < STRIDE; ++col)
			assert(light[row * STRIDE + col] == 0x12345678);
	app.editor.mode = VISUAL;
	app.editor.anchor = 0;
	assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
	assert(light[y * STRIDE + x] == 0xffc5dced);
	app.editor.mode = NORMAL;
	app.context_menu = true;
	app.context_x = x;
	app.context_y = y + app.line_height;
	assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
	assert(light[app.context_y * STRIDE + x] == 0xffcbdbe8);
	assert(light[(app.context_y + app.line_height) * STRIDE + x] ==
	       0xffe9edf1);
	assert(light[(app.context_y + 2 * app.line_height) * STRIDE + x] ==
	       0xffe9edf1);
	app.context_menu = false;
	editor_command(&app.editor, "set background=dark");
	assert(!app.editor.light);
	assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
	for (int row = 0; row < HEIGHT; ++row)
		assert(!memcmp(dark + row * WIDTH, light + row * STRIDE,
			       WIDTH * sizeof(*dark)));
	assert(!strcmp(app.editor.text, note) &&
	       app.editor.revision == revision);
	free(dark);
	free(light);
	app_close(&app);
}

static void decoration_layout(void)
{
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, "first\nsecond\n", 13));
	app.editor.cursor = 0;
	enum { WIDTH = 640, HEIGHT = 400 };
	uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
	assert(pixels);
	const unsigned scales[] = { 120, 150, 180, 240 };
	for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
		app.scale120 = scales[i];
		app.editor.light = i % 2 != 0;
		app.client_decorated = false;
		assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
		unsigned rows = app.editor.viewport_rows;
		app.client_decorated = true;
		assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
		int h = app.title_height, y = h / 2 * 120 / (int)scales[i];
		assert(h == app.line_height + app.padding &&
		       app.editor.viewport_rows < rows);
		assert(pixels[0] == 0);
		assert(pixels[WIDTH / 2] ==
		       (app.editor.light ? 0xfff2f1ee : 0xff1d2733));
		assert(app_title_hit(&app, 20, y) == TITLE_MOVE);
		assert(app_title_hit(&app,
				     (WIDTH - 5 * h / 2) * 120 / (int)scales[i],
				     y) == TITLE_MINIMIZE);
		assert(app_title_hit(&app,
				     (WIDTH - 3 * h / 2) * 120 / (int)scales[i],
				     y) == TITLE_MAXIMIZE);
		assert(app_title_hit(&app,
				     (WIDTH - h / 2) * 120 / (int)scales[i],
				     y) == TITLE_CLOSE);
		for (enum app_title_action action = TITLE_MINIMIZE;
		     action <= TITLE_CLOSE; ++action) {
			int stroke = (int)(scales[i] / 120);
			int sample = WIDTH - (TITLE_CLOSE - action + 1) * h +
				     4 * stroke;
			app.title_hover = action;
			assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
			uint32_t hover = action == TITLE_CLOSE ? 0xffc74242 :
					 app.editor.light      ? 0xffe0e2df :
								 0xff303e4e;
			assert(pixels[h / 2 * WIDTH + sample] == hover);
			app.title_pressed = action;
			if (action == TITLE_MAXIMIZE)
				app.maximized =
					true; /* the restore button shares the same feedback */
			assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
			uint32_t pressed = action == TITLE_CLOSE ? 0xffa83333 :
					   app.editor.light	 ? 0xffcdd3d0 :
								   0xff3d5064;
			assert(pixels[h / 2 * WIDTH + sample] == pressed);
			app.title_hover = TITLE_NONE;
			assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
			assert(pixels[h / 2 * WIDTH + sample] ==
			       pixels[WIDTH / 2]);
			app.title_pressed = TITLE_NONE;
			app.maximized = false;
		}
		int x = (app.padding + app.gutter) * 120 / (int)scales[i];
		int first = (h + app.padding + app.line_height / 2) * 120 /
			    (int)scales[i];
		int second = first + app.line_height * 120 / (int)scales[i];
		assert(app_hit_test(&app, x, first) == 0);
		assert(app_hit_test(&app, x, second) == 6);
		assert(app_title_hit(&app, x, second) == TITLE_NONE);
		app.client_decorated = false;
		assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
		assert(app.title_height == 0 &&
		       app.editor.viewport_rows == rows);
		assert(app_title_hit(&app, 20, y) == TITLE_NONE);
	}
	free(pixels);
	app_close(&app);
}

static void rounded_window(void)
{
	struct app app;
	assert(app_open(&app));
	app.client_decorated = true;
	enum { WIDTH = 321, HEIGHT = 201, STRIDE = 328 };
	uint32_t pixels[STRIDE * HEIGHT];
	const unsigned scales[] = { 120, 150, 180, 240 };
	for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
		app.scale120 = scales[i];
		app.editor.light = i % 2 != 0;
		for (int p = 0; p < STRIDE * HEIGHT; ++p)
			pixels[p] = 0x12345678;
		assert(app_paint(&app, pixels, WIDTH, HEIGHT, STRIDE));
		assert(app_corner_radius(&app, WIDTH, HEIGHT, scales[i]) ==
		       (int)(12 * scales[i] / 120));
		bool partial = false;
		for (int y = 0; y < HEIGHT; ++y) {
			for (int x = 0; x < WIDTH; ++x) {
				uint32_t pixel = pixels[y * STRIDE + x];
				unsigned alpha = pixel >> 24;
				assert(alpha ==
				       (pixels[y * STRIDE + WIDTH - 1 - x] >>
					24));
				assert(alpha ==
				       (pixels[(HEIGHT - 1 - y) * STRIDE + x] >>
					24));
				assert(((pixel >> 16) & 255) <= alpha);
				assert(((pixel >> 8) & 255) <= alpha &&
				       (pixel & 255) <= alpha);
				partial |= alpha > 0 && alpha < 255;
			}
			for (int x = WIDTH; x < STRIDE; ++x)
				assert(pixels[y * STRIDE + x] == 0x12345678);
		}
		assert(partial && pixels[0] == 0 && pixels[WIDTH - 1] == 0);
		assert(pixels[(HEIGHT - 1) * STRIDE] == 0);
		assert(pixels[(HEIGHT - 1) * STRIDE + WIDTH - 1] == 0);
		assert((pixels[WIDTH / 2] >> 24) == 255);
		for (unsigned state = 0; state < 3; ++state) {
			app.maximized = state == 0;
			app.tiled = state == 1;
			app.client_decorated = state != 2;
			assert(app_paint(&app, pixels, WIDTH, HEIGHT, STRIDE));
			assert((pixels[0] >> 24) == 255);
			assert(app_corner_radius(&app, WIDTH, HEIGHT,
						 scales[i]) == 0);
		}
		app.maximized = app.tiled = false;
		app.client_decorated = true;
	}
	/* No corner overlap, out-of-bounds writes, or invalid alpha on tiny buffers. */
	app.scale120 = 120;
	for (int h = 1; h < 27; ++h)
		for (int w = 1; w < 27; ++w) {
			for (int p = 0; p < STRIDE * HEIGHT; ++p)
				pixels[p] = 0x12345678;
			assert(app_paint(&app, pixels, w, h, STRIDE));
			for (int y = 0; y < h; ++y)
				assert(pixels[y * STRIDE + w] == 0x12345678);
			assert(pixels[h * STRIDE] == 0x12345678);
		}
	app_close(&app);
}

static void type(struct app *app, const char *keys)
{
	for (; *keys; ++keys) {
		char text[2] = { *keys, 0 };
		if (*keys == '\n')
			app_input(app, KEY_ENTER, NULL);
		else
			app_input(app, KEY_TEXT, text);
	}
}
/* The fence finder behind the Copy button: fence lines belong to their block, whose text runs
 * between the fences; a block left open runs to the end. */
static void code_blocks(void)
{
	/* The opening fence starts at 2, "echo hi" at 14, the closing fence at 22, "b" at 26. */
	static const char page[] =
		"a\n```sh\nls -l\necho hi\n```\nb\n~~~\nopen";
	struct editor ed = { 0 };
	assert(editor_open(&ed, NULL));
	assert(editor_replace(&ed, 0, 0, page, sizeof(page) - 1));
	size_t from, to;
	static const size_t inside[] = { 2, 8, 14, 21, 22, 25 };
	for (size_t i = 0; i < sizeof(inside) / sizeof(*inside); ++i)
		assert(editor_code_block(&ed, inside[i], &from, &to) &&
		       from == 8 && to == 21);
	assert(!editor_code_block(&ed, 0, &from, &to));
	assert(!editor_code_block(&ed, 26, &from, &to));
	assert(editor_code_block(&ed, ed.length, &from, &to) && from == 32 &&
	       to == ed.length);
	editor_destroy(&ed);
}
/* In the reading view, moving the mouse over a fenced code block shows a Copy button at the
 * top right of its first row. Clicking it copies the code without moving the cursor, and the
 * rest of that click selects nothing. Keys and leaving the window hide it; other views and
 * text outside blocks never show it, and y stays the yank operator. */
static void code_copy_button(void)
{
	static const char page[] = "a\n```sh\nls -l\necho hi\n```\nb\n";
	enum { WIDTH = 640, HEIGHT = 240 };
	static uint32_t plain[WIDTH * HEIGHT], shown[WIDTH * HEIGHT];
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, page, sizeof(page) - 1));
	app.editor.cursor = 0;
	type(&app, ":set view=reading\n");
	assert(app_paint(&app, plain, WIDTH, HEIGHT, WIDTH));
	assert(!app.code_button);
	/* The rows of "a", "ls -l" and "echo hi", found by hit-testing down the text. */
	int x = app.text_x + app.cell / 2, row_a = -1, row_ls = -1,
	    row_echo = -1;
	for (int y = app.title_height + app.padding;
	     y < app.title_height + app.padding + app.text_height; ++y) {
		size_t at = app_hit_test(&app, x, y);
		if (at == 0 && row_a < 0)
			row_a = y;
		if (at == 8 && row_ls < 0)
			row_ls = y;
		if (at == 14 && row_echo < 0)
			row_echo = y;
	}
	assert(row_a >= 0 && row_ls > row_a && row_echo > row_ls);
	assert(!app_hover(&app, x, row_a, false) && !app.code_hover);
	/* Over the block's second row, the button goes on its first. */
	app.dirty = false;
	assert(!app_hover(&app, x, row_echo, false) && app.code_hover &&
	       app.dirty && app.code_from == 8 && app.code_to == 21);
	assert(app_paint(&app, shown, WIDTH, HEIGHT, WIDTH));
	assert(app.code_button && memcmp(plain, shown, sizeof(plain)));
	assert(app.code_button_y <= row_ls &&
	       row_ls < app.code_button_y + app.code_button_h &&
	       app.code_button_x + app.code_button_w ==
		       app.text_x + app.text_width);
	/* Over the button, the pointer is a hand. */
	int button_x = app.code_button_x + app.code_button_w / 2,
	    button_y = app.code_button_y + app.code_button_h / 2;
	assert(app_hover(&app, button_x, button_y, false) &&
	       app.code_button_hover);
	/* Clicking it copies, and dragging from it selects nothing. */
	app_pointer(&app, button_x, button_y, false);
	assert(app.editor.register_text &&
	       !strcmp(app.editor.register_text, "ls -l\necho hi\n") &&
	       app.editor.register_linewise && app.editor.cursor == 0 &&
	       !strcmp(app.toast, "Code copied") &&
	       !strcmp(app.editor.text, page));
	app_pointer(&app, x, row_echo, true);
	assert(app.editor.mode == NORMAL && app.editor.cursor == 0);
	app_pointer_release(&app);
	/* A click elsewhere moves the cursor as before. */
	app_pointer(&app, x, row_echo, false);
	assert(app.editor.cursor == 14);
	/* Leaving the window hides the button, and so does a key. */
	assert(!app_hover(&app, -1, -1, false) && !app.code_hover);
	assert(app_paint(&app, shown, WIDTH, HEIGHT, WIDTH) &&
	       !app.code_button);
	app_hover(&app, x, row_echo, false);
	assert(app.code_hover);
	app_input(&app, KEY_TEXT, "k");
	assert(!app.code_hover && !app.code_button_hover);
	assert(app_paint(&app, shown, WIDTH, HEIGHT, WIDTH) &&
	       !app.code_button);
	/* y in a block is still the yank operator, waiting for its motion. */
	app.editor.cursor = 14;
	type(&app, "y");
	assert(!strcmp(app.editor.pending, "y"));
	app_input(&app, KEY_ESCAPE, NULL);
	/* Other views show no button. */
	type(&app, ":set view=normal\n");
	assert(app_paint(&app, shown, WIDTH, HEIGHT, WIDTH));
	app_hover(&app, x, app.title_height + app.padding + app.line_height * 2,
		  false);
	assert(!app.code_hover);
	assert(app_paint(&app, shown, WIDTH, HEIGHT, WIDTH) &&
	       !app.code_button);
	app_close(&app);
}
/* Whether two paints match inside the Copy button's box. */
static bool same_button(const struct app *app, const uint32_t *a,
			const uint32_t *b, int stride)
{
	for (int y = app->code_button_y;
	     y < app->code_button_y + app->code_button_h; ++y)
		if (memcmp(a + (size_t)y * stride + app->code_button_x,
			   b + (size_t)y * stride + app->code_button_x,
			   (size_t)app->code_button_w * sizeof(*a)))
			return false;
	return true;
}
/* Copied shows only on the block that was copied: another block's button, or the same one
 * after the mouse has left it, says Copy again. */
static void code_copied_label(void)
{
	/* "one" starts at 4 and "two" at 17. */
	static const char page[] = "```\none\n```\n\n```\ntwo\n```\n";
	enum { WIDTH = 640, HEIGHT = 240 };
	static uint32_t one[WIDTH * HEIGHT], one_copied[WIDTH * HEIGHT],
		one_again[WIDTH * HEIGHT], two[WIDTH * HEIGHT],
		two_after[WIDTH * HEIGHT];
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, page, sizeof(page) - 1));
	app.editor.cursor = 0;
	type(&app, ":set view=reading\n");
	assert(app_paint(&app, one, WIDTH, HEIGHT, WIDTH));
	int x = app.text_x + app.cell / 2, row_one = -1, row_two = -1;
	for (int y = app.title_height + app.padding;
	     y < app.title_height + app.padding + app.text_height; ++y) {
		size_t at = app_hit_test(&app, x, y);
		if (at == 4 && row_one < 0)
			row_one = y;
		if (at == 17 && row_two < 0)
			row_two = y;
	}
	assert(row_one >= 0 && row_two > row_one);
	app_hover(&app, x, row_two, false);
	assert(app_paint(&app, two, WIDTH, HEIGHT, WIDTH) && app.code_button);
	app_hover(&app, x, row_one, false);
	assert(app_paint(&app, one, WIDTH, HEIGHT, WIDTH) && app.code_button);
	/* Click the first block's button: it says Copied. */
	int button_x = app.code_button_x + app.code_button_w / 2,
	    button_y = app.code_button_y + app.code_button_h / 2;
	app_hover(&app, button_x, button_y, false);
	app_pointer(&app, button_x, button_y, false);
	app_pointer_release(&app);
	app_hover(&app, x, row_one, false);
	assert(app.code_copied && !strcmp(app.editor.register_text, "one\n"));
	assert(app_paint(&app, one_copied, WIDTH, HEIGHT, WIDTH));
	assert(!same_button(&app, one, one_copied, WIDTH));
	/* While the toast still shows, the second block says Copy. */
	app_hover(&app, x, row_two, false);
	assert(!app.code_copied && app.toast[0]);
	assert(app_paint(&app, two_after, WIDTH, HEIGHT, WIDTH));
	assert(same_button(&app, two, two_after, WIDTH));
	/* Back on the first block, it is Copy again. */
	app_hover(&app, x, row_one, false);
	assert(!app.code_copied);
	assert(app_paint(&app, one_again, WIDTH, HEIGHT, WIDTH));
	assert(same_button(&app, one, one_again, WIDTH));
	app_close(&app);
}
/* :help shows the built-in help document in place of the page, read-only, until Esc. */
static void help_document(void)
{
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, "notes\n", 6));
	app.editor.cursor = 0;
	enum { WIDTH = 800, HEIGHT = 560 };
	uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
	assert(pixels);
	/* Help is shown in the reading view, whatever the page uses. */
	type(&app, ":set view=source\n:help\n");
	assert(app.help && app.editor.readonly && !app.editor.modified &&
	       app.editor.view == VIEW_READING);
	/* The text is docs/help.md, whatever it says. */
	size_t length = sizeof(help_text) - 1;
	assert(app.editor.length == length &&
	       !memcmp(app.editor.text, help_text, length));
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	/* Nothing edits it. */
	type(&app, "xddp");
	app_input(&app, KEY_CTRL_V, "");
	app_input(&app, KEY_CTRL_S, "");
	assert(app.editor.length == length && !app.editor.modified &&
	       !memcmp(app.editor.text, help_text, length) &&
	       strstr(app.editor.message, "read-only"));
	/* There is no command line, and ZZ doesn't act on it. */
	type(&app, ":");
	assert(!app.editor.command_active &&
	       !strcmp(app.editor.message, "Esc closes help"));
	type(&app, "ZZ");
	assert(app.help && app.running && !app.editor.quit);
	/* Search works, and Esc cancels a search or selection before it leaves help. The word
	 * searched for is the first one of four letters after the first line. */
	char word[5] = { 0 }, search[8];
	for (const char *p = strchr(help_text, '\n'); p && *p && !word[0];
	     ++p) {
		size_t n = 0;
		while (n < 4 && ((p[n] >= 'a' && p[n] <= 'z') ||
				 (p[n] >= 'A' && p[n] <= 'Z')))
			++n;
		if (n == 4)
			memcpy(word, p, 4);
	}
	assert(word[0]);
	snprintf(search, sizeof(search), "/%s", word);
	type(&app, search);
	app_input(&app, KEY_ESCAPE, NULL);
	assert(app.help && !app.editor.command_active && !app.editor.cursor);
	type(&app, search);
	type(&app, "\n");
	size_t found = app.editor.cursor;
	assert(found);
	for (int i = 0; i < 4;
	     ++i) /* search ignores case unless it has capitals */
		assert((app.editor.text[found + i] | 0x20) == (word[i] | 0x20));
	type(&app, "v");
	app_input(&app, KEY_ESCAPE, NULL);
	assert(app.help && app.editor.mode == NORMAL);
	type(&app, "y");
	assert(!strcmp(app.editor.pending, "y"));
	app_input(&app, KEY_ESCAPE, NULL);
	assert(app.help && !app.editor.pending[0]);
	app_input(&app, KEY_ESCAPE, NULL);
	assert(!app.help && !app.editor.readonly &&
	       !strcmp(app.editor.text, "notes\n") && app.editor.modified &&
	       app.editor.cursor == 0 && app.editor.view == VIEW_SOURCE);
	type(&app, "x");
	assert(!strcmp(app.editor.text, "otes\n"));
	/* Opening it again goes back to where you were in it. */
	type(&app, ":h marks\n");
	assert(app.help && app.editor.cursor == found);
	/* Ctrl+Q leaves help and quits from the page, which has unsaved changes. */
	app_input(&app, KEY_CTRL_Q, "");
	assert(!app.help && app.running && !strcmp(app.editor.text, "otes\n") &&
	       strstr(app.editor.message, "Unsaved"));
	/* Closing the window from help puts the page back first. */
	type(&app, ":help\n");
	app_request_close(&app);
	assert(!app.help && app.running && !strcmp(app.editor.text, "otes\n"));
	type(&app, ":help\n");
	assert(app.help);
	free(pixels);
	app_close(&app);
}
static void render_modes(void)
{
	struct app app;
	assert(app_open(&app));
	const char *note =
		"# Head\n**bold** [label](https://example.com/a/very/long/destination) tail\n"
		"> quote\n- [x] task\n```\n# raw\n```\n";
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	app.editor.cursor = 0;
	enum { WIDTH = 320, HEIGHT = 400 };
	uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
	assert(pixels);
	assert(app.editor.view == VIEW_NORMAL);
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	int normal_height = app.line_height;
	size_t second = (size_t)(strstr(note, "**bold") - note);
	assert(app_view_next(&app, second) <
	       editor_line_end(&app.editor, second));
	editor_command(&app.editor, "set view=source");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.gutter == 6 * app.cell && app.line_height == normal_height);
	assert(app_hit_test(&app, app.text_x,
			    app.padding + app.line_height / 2) == 0);
	uint64_t revision = app.editor.revision;
	editor_command(&app.editor, "set view=reading");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.gutter == 0 && app.text_x > app.padding &&
	       app.line_height > normal_height);
	assert(app_view_next(&app, second) ==
	       editor_line_end(&app.editor, second) + 1);
	int x = app.text_x, first = app.padding + app.line_height / 2;
	assert(app_hit_test(&app, x, first) ==
	       2); /* heading marker is concealed */
	int body_y = app.padding + app_view_height(&app, 0);
	int body_first = body_y + app.line_height / 2;
	assert(app_hit_test(&app, x, body_first) == second + 2);
	assert(app_hit_test(&app, x + 5 * app.cell, body_first) ==
	       (size_t)(strstr(note, "label") - note));
	assert(app_hit_test(&app, x + 11 * app.cell, body_first) ==
	       (size_t)(strstr(note, "tail") - note));
	assert(app_hit_test(&app, x, body_first + app.line_height) ==
	       (size_t)(strstr(note, "quote") - note));
	assert(app_hit_test(&app, x + 2 * app.cell,
			    body_first + 2 * app.line_height) ==
	       (size_t)(strstr(note, "task") - note));
	size_t fence = (size_t)(strstr(note, "```") - note);
	assert(app_view_rows(&app, fence) == 0);
	assert(app_hit_test(&app, x, body_first + 3 * app.line_height) ==
	       (size_t)(strstr(note, "# raw") - note));
	assert(pixels[(body_y + 3 * app.line_height) * WIDTH + x] ==
	       0xff242d3b);
	assert(pixels[(body_y + app.line_height) * WIDTH + x - app.padding] ==
	       0xffa3be8c);
	app_pointer(&app, x, body_first, false);
	app_pointer(&app, x + 3 * app.cell, body_first, true);
	assert(app.editor.mode == VISUAL && app.editor.anchor == second + 2 &&
	       app.editor.cursor == second + 5);
	assert(app.editor.revision == revision &&
	       !strcmp(app.editor.text, note));
	app_input(&app, KEY_TEXT, "d");
	assert(app.editor.revision == revision &&
	       !strcmp(app.editor.text, note));
	editor_command(&app.editor, "set view=normal");
	assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
	assert(app.gutter == 6 * app.cell && app.line_height == normal_height);
	assert(!strcmp(app.editor.text, note));
	free(pixels);
	app_close(&app);
}

static unsigned style_at(const char *text, const char *needle)
{
	struct md_span spans[128];
	size_t n = markdown_line(text, 0, strlen(text), spans, 128, false);
	size_t at = (size_t)(strstr(text, needle) - text);
	unsigned style = 0;
	for (size_t i = 0; i < n; ++i)
		if (at >= spans[i].start && at < spans[i].end)
			style |= spans[i].style;
	return style;
}
static void markdown_notes(void)
{
	assert(style_at("**bold and *nested italic***", "nested") ==
	       (MD_BOLD | MD_ITALIC));
	assert(style_at("*italic and **nested bold***", "nested") ==
	       (MD_BOLD | MD_ITALIC));
	assert(style_at("***both***", "both") == (MD_BOLD | MD_ITALIC));
	assert(style_at("~~old **bold**~~", "bold") == (MD_STRIKE | MD_BOLD));
	assert(style_at("`~~literal~~`", "literal") == MD_CODE);
	assert(style_at("https://example.org/a_b", "a_b") == MD_LINK);
	assert(!(style_at("word_with_underscores", "with") & MD_ITALIC));
	assert(markdown_block("  12) item", 0, 10).indent == 6);
	assert(markdown_block("    - [x] task", 0, 14).indent == 6);
	assert(markdown_block("* * *", 0, 5).rule);
	assert(!markdown_block("- text", 0, 6).rule);
	assert(!markdown_block("1234567890. text", 0, 16).indent);
	struct app app;
	assert(app_open(&app));
	const char *note =
		"## Heading\n  12. a long nested list item with several words for wrapping\n---\n";
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	editor_command(&app.editor, "set view=reading");
	uint32_t pixels[400 * 400];
	assert(app_paint(&app, pixels, 400, 400, 400));
	assert(app_view_rows(&app, 0) == 2);
	size_t list = (size_t)(strstr(note, "  12.") - note);
	size_t wrap = app_view_next(&app, list);
	assert(wrap > list && wrap < editor_line_end(&app.editor, list));
	/* Continuation text begins under the first item character, not the number. */
	assert(app_hit_test(&app, app.text_x + 6 * app.cell,
			    app.padding + app_view_height(&app, 0) +
				    app.line_height) == wrap);
	size_t rule = (size_t)(strstr(note, "---") - note);
	assert(app_view_next(&app, rule) == strlen(note));
	editor_command(&app.editor, "set view=source");
	assert(app_paint(&app, pixels, 400, 400, 400));
	assert(app_view_rows(&app, 0) == 1);
	assert(!strcmp(app.editor.text, note));
	app_close(&app);
}

static void markdown_tables(void)
{
	const char *note =
		"| Name | Value |\n| :--- | ---: |\n"
		"| **alpha** with several words that wrap neatly | 42 |\n"
		"| literal \\| pipe | `code` |\n";
	struct md_table table = { 0 };
	size_t separator = (size_t)(strchr(note, '\n') - note) + 1;
	size_t body = (size_t)(strchr(note + separator, '\n') - note) + 1;
	size_t last = (size_t)(strchr(note + body, '\n') - note) + 1;
	markdown_table(note, strlen(note), 0, separator - 1, &table, false);
	assert(table.count == 2 && table.header && !table.separator);
	assert(table.align[0] == 0 && table.align[1] == 2);
	markdown_table(note, strlen(note), separator, body - 1, &table, true);
	assert(table.separator && !table.header);
	markdown_table(note, strlen(note), body, last - 1, &table, true);
	assert(table.count == 2 && !table.header && !table.separator);
	memset(&table, 0, sizeof(table));
	markdown_table(note, strlen(note), last, strlen(note) - 1, &table,
		       false);
	assert(table.count == 2 &&
	       table.align[1] == 2); /* direct access into body */
	assert(table.cells[0].end - table.cells[0].start ==
	       strlen("literal \\| pipe"));
	const char *bad = "a | b\n-- | ---\n";
	markdown_table(bad, strlen(bad), 0, 5, &table, false);
	assert(!table.count);
	struct md_cell cells[MD_TABLE_COLUMNS];
	const char *wide = "a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q";
	assert(!markdown_cells(wide, 0, strlen(wide), cells));
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
	editor_command(&app.editor, "set view=reading");
	uint32_t pixels[400 * 400];
	assert(app_paint(&app, pixels, 400, 400, 400));
	assert(app_view_rows(&app, separator) == 0);
	assert(app_view_next(&app, body) == last);
	int body_rows = app_view_rows(&app, body);
	assert(body_rows > 1);
	/* Clicking wrapped/hidden source and a right-aligned number. */
	assert(app_hit_test(&app, app.text_x + app.cell,
			    app.padding + app.line_height) == body + 4);
	assert(app_hit_test(&app, app.text_x + app.cell,
			    app.padding + 2 * app.line_height) ==
	       (size_t)(strstr(note, "several") - note));
	unsigned content = (app.editor.viewport_columns - 1) / 2 - 3;
	int number_x = app.text_x + (int)(2 * content + 2) * app.cell;
	assert(app_hit_test(&app, number_x, app.padding + app.line_height) ==
	       (size_t)(strstr(note, "42") - note));
	assert(app_hit_test(&app, app.text_x + app.cell,
			    app.padding + (body_rows + 1) * app.line_height) ==
	       last + 2);
	/* A resize must retain access to long rows through the source fallback. */
	assert(app_paint(&app, pixels, 120, 200, 120));
	assert(app_view_next(&app, body) < last);
	app.editor.cursor = app.editor.top = 0;
	editor_command(&app.editor, "set view=source");
	assert(app_paint(&app, pixels, 400, 400, 400));
	assert(app_view_rows(&app, separator) == 1);
	assert(app_hit_test(&app, app.text_x, app.padding) == 0);
	assert(!strcmp(app.editor.text, note));
	const char *code = "```\n| a | b |\n| --- | --- |\n```\n";
	assert(editor_replace(&app.editor, 0, app.editor.length, code,
			      strlen(code)));
	app.editor.cursor = app.editor.top = 0;
	editor_command(&app.editor, "set view=reading");
	assert(app_paint(&app, pixels, 400, 400, 400));
	size_t code_separator = (size_t)(strstr(code, "| ---") - code);
	assert(app_view_rows(&app, code_separator) == 1 &&
	       !app.markdown.table.count);
	app_close(&app);
}

static void normal_markdown_styles(void)
{
	assert(style_at("***bolditalic***", "bolditalic") ==
	       (MD_BOLD | MD_ITALIC));
	assert(style_at("~~strikethrough~~", "strikethrough") == MD_STRIKE);
	const char *note =
		"***slanted***\n **slanted**\n~~xxxx~~\n```text\nshort\n\n```\nafter\n";
	const unsigned scales[] = { 120, 180, 240 };
	for (unsigned light = 0; light < 2; ++light) {
		for (unsigned scale = 0; scale < 3; ++scale) {
			struct app app;
			assert(app_open(&app));
			assert(editor_replace(&app.editor, 0, 0, note,
					      strlen(note)));
			app.editor.cursor = app.editor.length - 1;
			app.editor.light = light;
			app.scale120 = scales[scale];
			int width = 400 * (int)app.scale120 / 120;
			int height = 400 * (int)app.scale120 / 120;
			uint32_t *pixels = malloc((size_t)width * height *
						  sizeof(*pixels));
			assert(pixels &&
			       app_paint(&app, pixels, width, height, width));
			uint32_t background = light ? 0xfffaf9f6 : 0xff18202b;
			uint32_t code = light ? 0xffeeede7 : 0xff242d3b;
			uint32_t text = light ? 0xff263342 : 0xffe5edf5;
			/* Both rows have text in the same columns; italic changes the shape. */
			bool slanted = false;
			for (int y = 0; y < app.line_height; ++y)
				for (int x = app.text_x + 3 * app.cell;
				     x < app.text_x + 10 * app.cell; ++x)
					slanted |= (pixels[(app.padding + y) *
								   width +
							   x] != background) !=
						   (pixels[(app.padding +
							    app.line_height +
							    y) * width +
							   x] != background);
			assert(slanted);
			int strike_y =
				app.padding + 2 * app.line_height +
				tiny_font_metrics(app.font).ascender * 3 / 4;
			for (int x = app.text_x + 2 * app.cell;
			     x < app.text_x + 6 * app.cell; ++x)
				assert(pixels[strike_y * width + x] == text);
			/* Fence markers, content and empty lines all shade through the right edge. */
			for (int row = 3; row <= 6; ++row) {
				int y = app.padding + row * app.line_height;
				assert(pixels[y * width + app.text_x +
					      app.text_width - 1] == code);
				assert(pixels[y * width + app.padding] ==
				       background); /* gutter stays clear */
			}
			assert(pixels[(app.padding + 5 * app.line_height) *
					      width +
				      app.text_x] == code);
			editor_command(&app.editor, "set view=source");
			assert(app_paint(&app, pixels, width, height, width));
			assert(pixels[(app.padding + 5 * app.line_height) *
					      width +
				      app.text_x] == background);
			free(pixels);
			app_close(&app);
		}
	}
}

static void heading_layout(void)
{
	const char *note =
		"# Heading\n## Heading\n### Heading\n#### Heading\n##### Heading\n###### Heading\n"
		"body\n# A much longer heading with enough words to wrap across several lines\ntail\n";
	const unsigned scales[] = { 120, 180, 240 };
	for (unsigned light = 0; light < 2; ++light) {
		for (unsigned scale = 0; scale < 3; ++scale) {
			struct app app;
			assert(app_open(&app));
			assert(editor_replace(&app.editor, 0, 0, note,
					      strlen(note)));
			app.scale120 = scales[scale];
			app.editor.light = light;
			int width = 360 * (int)app.scale120 / 120;
			int height = 720 * (int)app.scale120 / 120;
			uint32_t *pixels = malloc((size_t)width * height *
						  sizeof(*pixels));
			assert(pixels);
			int normal_h1 = 0;
			const char *modes[] = { "set view=normal",
						"set view=reading",
						"set view=source" };
			for (unsigned mode = 0; mode < 3; ++mode) {
				editor_command(&app.editor, modes[mode]);
				app.editor.cursor = app.editor.top = 0;
				app.follow_cursor = false;
				assert(app_paint(&app, pixels, width, height,
						 width));
				size_t p = 0;
				int y = app.padding,
				    first_height = app_view_height(&app, 0),
				    previous = first_height;
				if (mode == 0)
					normal_h1 = first_height;
				if (mode == 1)
					assert(first_height > normal_h1);
				for (unsigned level = 1; level <= 6; ++level) {
					int h = app_view_height(&app, p);
					assert(h <= previous);
					if (mode == 2)
						assert(h == app.line_height);
					/* Every heading's left edge maps to its own source line. */
					int logical_x = (app.text_x + 1) * 120 /
							(int)app.scale120;
					int logical_y = (y + h / 2) * 120 /
							(int)app.scale120;
					assert(app_hit_test(&app, logical_x,
							    logical_y) ==
					       p + (mode == 1 ? level + 1 : 0));
					previous = h;
					y += h;
					p = editor_line_end(&app.editor, p) + 1;
				}
				unsigned base_size =
					18 * 64 * app.scale120 / 120;
				unsigned heading_size =
					mode == 0 ?
						(base_size * 135 + 50) / 100 :
					mode == 1 ?
						(base_size * 190 + 50) / 100 :
						base_size;
				int heading_cell =
					tiny_font_metrics_at(heading_size)
						.advance;
				unsigned target_column = mode == 1 ? 3 : 6;
				int logical_click_x =
					(app.text_x +
					 (int)target_column * heading_cell +
					 heading_cell / 2) *
					120 / (int)app.scale120;
				int logical_click_y =
					(app.padding + first_height / 2) * 120 /
					(int)app.scale120;
				assert(app_hit_test(&app, logical_click_x,
						    logical_click_y) ==
				       target_column + (mode == 1 ? 2 : 0));
				if (mode == 0) {
					app.editor.cursor = 6;
					assert(app_paint(&app, pixels, width,
							 height, width));
					int cursor_x = app.text_x +
						       6 * heading_cell +
						       heading_cell / 2;
					int cursor_y =
						app.padding + first_height - 2;
					assert(pixels[cursor_y * width +
						      cursor_x] ==
					       (light ? 0xff1d6972 :
							0xff8fbcbb));
					app.editor.cursor = 0;
				}
				if (mode != 2)
					assert(first_height > previous);
				assert(app_view_height(&app, p) ==
				       app.line_height); /* body size restored */
				p = editor_line_end(&app.editor, p) + 1;
				y += app.line_height;
				size_t wrap = app_view_next(&app, p);
				assert(wrap > p &&
				       wrap < editor_line_end(&app.editor, p));
				int h = app_view_height(&app, p);
				int logical_x = (app.text_x + 1) * 120 /
						(int)app.scale120;
				int logical_y = (y + h + h / 2) * 120 /
						(int)app.scale120;
				assert(app_hit_test(&app, logical_x,
						    logical_y) == wrap);
				/* On a shorter window, follow the cursor using actual heights. */
				app.editor.cursor =
					(size_t)(strstr(note, "tail") - note);
				app.follow_cursor = true;
				int short_height =
					180 * (int)app.scale120 / 120;
				assert(app_paint(&app, pixels, width,
						 short_height, width));
				assert(app.editor.top > 0);
				int cursor_y = 0;
				p = app.editor.top;
				while (p < app.editor.cursor) {
					cursor_y += app_view_height(&app, p);
					size_t next = app_view_next(&app, p);
					assert(next > p &&
					       next <= app.editor.cursor);
					p = next;
				}
				assert(cursor_y + app_view_height(&app, p) <=
				       app.text_height);
			}
			assert(!strcmp(app.editor.text, note));
			free(pixels);
			app_close(&app);
		}
	}
}

static void colorschemes(void)
{
	const uint32_t backgrounds[COLOR_COUNT][2] = {
		{ 0xff18202b, 0xfffaf9f6 },
		{ 0xff1a1b26, 0xffe1e2e7 },
		{ 0xff1e1e2e, 0xffeff1f5 },
		{ 0xff1f1f28, 0xfff2ecbc },
		{ 0xff2d353b, 0xfffdf6e3 }
	};
	const uint32_t searches[COLOR_COUNT][2] = { { 0xff66502c, 0xfff5dc8e },
						    { 0xff3d59a1, 0xff7890dd },
						    { 0xff3e5767, 0xffa8daf0 },
						    { 0xff2d4f67, 0xffb5cbd2 },
						    { 0xffa7c080,
						      0xff8da101 } };
	const uint32_t search_text[COLOR_COUNT][2] = {
		{ 0xffe5edf5, 0xff263342 },
		{ 0xffc0caf5, 0xff3760bf },
		{ 0xffcdd6f4, 0xff4c4f69 },
		{ 0xffdcd7ba, 0xff545464 },
		{ 0xff2d353b, 0xfffdf6e3 }
	};
	const uint32_t status_backgrounds[COLOR_COUNT][2] = {
		{ 0xff283443, 0xffe5e8eb },
		{ 0xff16161e, 0xffd0d5e3 },
		{ 0xff181825, 0xffe6e9ef },
		{ 0xff16161d, 0xffd5cea3 },
		{ 0xff3d484d, 0xffefebd4 }
	};
	struct app app;
	assert(app_open(&app));
	assert(editor_replace(&app.editor, 0, 0, "MMMM\n", 5));
	strcpy(app.editor.search, "MMMM");
	app.editor.cursor = 4;
	app.client_decorated = true;
	uint32_t pixels[400 * 300];
	const char *modes[] = { "set view=source", "set view=normal",
				"set view=reading" };
	for (unsigned scheme = 0; scheme < COLOR_COUNT; ++scheme) {
		assert(editor_colorscheme(
			&app.editor,
			editor_colorscheme_name((enum color_scheme)scheme)));
		for (unsigned light = 0; light < 2; ++light) {
			editor_command(&app.editor,
				       light ? "set bg=light" : "set bg=dark");
			for (unsigned mode = 0; mode < 3; ++mode) {
				editor_command(&app.editor, modes[mode]);
				assert(app_paint(&app, pixels, 400, 300, 400));
				int top = app.title_height + app.padding;
				assert(pixels[top * 400] ==
				       backgrounds[scheme][light]);
				assert(pixels[299 * 400 + 200] ==
				       status_backgrounds[scheme][light]);
				assert(pixels[top * 400 + app.text_x] ==
				       searches[scheme][light]);
				bool ink = false;
				for (int y = top; y < top + app.line_height;
				     ++y)
					for (int x = app.text_x;
					     x < app.text_x + app.cell; ++x)
						ink |= pixels[y * 400 + x] ==
						       search_text[scheme]
								  [light];
				assert(ink); /* yellow/light search backgrounds need dark ink */
			}
		}
	}
	assert(!strcmp(app.editor.text, "MMMM\n"));
	app_close(&app);
}

/* Whether a gutter cell of a text row has any ink. */
static bool gutter_ink(const struct app *app, const uint32_t *pixels, int row,
		       int column)
{
	for (int y = app->padding + row * app->line_height;
	     y < app->padding + (row + 1) * app->line_height; ++y)
		for (int x = app->padding + column * app->cell;
		     x < app->padding + (column + 1) * app->cell; ++x)
			if (pixels[y * 320 + x] != 0xff18202b)
				return true;
	return false;
}
/* Whether two text rows draw the same gutter. */
static bool same_gutter(const struct app *app, const uint32_t *pixels, int a,
			int b)
{
	for (int y = 0; y < app->line_height; ++y)
		for (int x = app->padding; x < app->padding + app->gutter; ++x)
			if (pixels[(app->padding + a * app->line_height + y) *
					   320 +
				   x] !=
			    pixels[(app->padding + b * app->line_height + y) *
					   320 +
				   x])
				return false;
	return true;
}
static void number_options(void)
{
	struct app app;
	assert(app_open(&app));
	uint32_t pixels[320 * 200];
	assert(editor_replace(&app.editor, 0, 0, "a\nb\nc\n", 6));
	app.editor.cursor = 2; /* the second line */
	/* number: 1 2 3, right-aligned. */
	assert(app_paint(&app, pixels, 320, 200, 320));
	int gutter = app.gutter, text_x = app.text_x;
	assert(gutter == 6 * app.cell && !gutter_ink(&app, pixels, 0, 0) &&
	       gutter_ink(&app, pixels, 0, 3) &&
	       !same_gutter(&app, pixels, 0, 2));
	/* relativenumber: 1 0 1. */
	editor_command(&app.editor, "set nonu rnu");
	assert(app_paint(&app, pixels, 320, 200, 320));
	assert(app.gutter == gutter && same_gutter(&app, pixels, 0, 2) &&
	       !same_gutter(&app, pixels, 0, 1) &&
	       gutter_ink(&app, pixels, 1, 3));
	/* Both: 1 2 1, with the cursor line's number right-aligned where its 0 was. */
	editor_command(&app.editor, "set nu");
	assert(app_paint(&app, pixels, 320, 200, 320));
	assert(same_gutter(&app, pixels, 0, 2) &&
	       !gutter_ink(&app, pixels, 1, 0) &&
	       gutter_ink(&app, pixels, 1, 3) &&
	       !same_gutter(&app, pixels, 0, 1));
	/* Neither: no gutter, and the text moves left. */
	editor_command(&app.editor, "set nonu nornu");
	assert(app_paint(&app, pixels, 320, 200, 320));
	assert(app.gutter == 0 && app.text_x == text_x - gutter);
	assert(app_hit_test(&app, app.padding, app.padding) == 0);
	app_close(&app);
}

/* With Ctrl held, the link under the mouse is underlined and a click there opens it; without
 * Ctrl, or off a link, nothing is. */
static void link_hover(void)
{
	struct app app;
	assert(app_open(&app));
	static uint32_t plain[640 * 200], hovered[640 * 200];
	const char *text = "see [docs](a.md) and https://x.io end";
	assert(editor_replace(&app.editor, 0, app.editor.length, text,
			      strlen(text)));
	app.editor.cursor = 0;
	assert(app_paint(&app, plain, 640, 200, 640));
	int y = app.title_height + app.padding + app.line_height / 2;
#define COLUMN(c) (app.text_x + (c) * app.cell + app.cell / 2)
	assert(app_hit_test(&app, COLUMN(5), y) == 5);
	assert(!app_hover(&app, COLUMN(5), y, false) && !app.hover_end);
	assert(app_hover(&app, COLUMN(5), y, true) && app.hover_open);
	assert(app.hover_start == 4 && app.hover_end == 16 && app.dirty);
	assert(app_paint(&app, hovered, 640, 200, 640));
	assert(memcmp(plain, hovered, sizeof(plain)));
	assert(app_hover(&app, COLUMN(24), y, true));
	assert(app.hover_start == 21 && app.hover_end == 33);
	assert(!app_hover(&app, COLUMN(18), y, true) && !app.hover_end);
	assert(!app_hover(&app, COLUMN(5), 199, true)); /* the status line */
	assert(app_hover(&app, COLUMN(5), y, true));
	app_input(&app, KEY_TEXT,
		  "l"); /* keys may move the text, so start over */
	assert(!app.hover_open && !app.hover_end);
	assert(app_paint(&app, hovered, 640, 200, 640));
	app.editor.cursor = 0;
	assert(app_paint(&app, hovered, 640, 200, 640));
	assert(!memcmp(plain, hovered, sizeof(plain)));
#undef COLUMN
	/* A double click opens images only; links take Ctrl. */
	app.editor.cursor = 5;
	assert(!app_double_click_opens(&app));
	app_close(&app);
}

/* Opening and page switches light the cursor's row, which fades out and stops redrawing. */
static void cursor_beacon(void)
{
	struct app app;
	assert(app_open(&app));
	static uint32_t plain[320 * 200], lit[320 * 200];
	assert(app_paint(&app, plain, 320, 200, 320));
	assert(!app_deadline(&app));
	app.beacon = true;
	assert(app_paint(&app, lit, 320, 200, 320));
	assert(app.beacon_at && !app.beacon && app_deadline(&app));
	assert(memcmp(plain, lit, sizeof(plain)));
	app.dirty = false;
	app_tick(&app);
	assert(app.dirty && app.beacon_at);
	app.beacon_at = 1; /* long over */
	app_tick(&app);
	assert(!app.beacon_at && !app_deadline(&app));
	assert(app_paint(&app, lit, 320, 200, 320));
	assert(!memcmp(plain, lit, sizeof(plain)));
	/* A key means the cursor has been found. */
	app.beacon = true;
	assert(app_paint(&app, lit, 320, 200, 320) && app.beacon_at);
	app_input(&app, KEY_ESCAPE, "");
	assert(!app.beacon_at && !app_deadline(&app));
	app_close(&app);
}

int main(void)
{
	link_hover();
	cursor_beacon();
	gutter_layout();
	number_options();
	theme_layout();
	color_column();
	wrap_width();
	search_visibility();
	toast();
	decoration_layout();
	rounded_window();
	render_modes();
	help_document();
	code_blocks();
	code_copy_button();
	code_copied_label();
	markdown_notes();
	markdown_tables();
	normal_markdown_styles();
	heading_layout();
	colorschemes();
	struct app app;
	assert(app_open(&app));
	app_input(&app, KEY_TEXT, "i");
	app_input(&app, KEY_TEXT, "Shared editor\n");
	app_input(&app, KEY_ESCAPE, "");
	assert(app.editor.modified && app.running);
	app_request_close(&app);
	assert(app.running && strstr(app.editor.message, "Unsaved"));
	enum { WIDTH = 320, HEIGHT = 200, STRIDE = 337 };
	uint32_t *packed = malloc(WIDTH * HEIGHT * sizeof(*packed));
	uint32_t *padded = malloc(STRIDE * HEIGHT * sizeof(*padded));
	assert(packed && padded);
	for (int i = 0; i < STRIDE * HEIGHT; ++i)
		padded[i] = 0x12345678;
	app.scale120 = 180;
	assert(app_paint(&app, packed, WIDTH, HEIGHT, WIDTH));
	assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
	for (int y = 0; y < HEIGHT; ++y) {
		assert(!memcmp(packed + y * WIDTH, padded + y * STRIDE,
			       WIDTH * sizeof(*packed)));
		for (int x = WIDTH; x < STRIDE; ++x)
			assert(padded[y * STRIDE + x] == 0x12345678);
	}
	/* Commands replace the status row without changing the text viewport. */
	app.editor.message[0] = 0;
	assert(app_paint(&app, packed, WIDTH, HEIGHT, WIDTH));
	int status_y = HEIGHT - app.line_height - 2 * app.padding;
	assert(app.editor.viewport_rows ==
	       (unsigned)((status_y - app.padding) / app.line_height));
	app_input(&app, KEY_TEXT, ":");
	app_input(&app, KEY_TEXT, "set view=normal");
	assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
	assert(app.editor.viewport_rows ==
	       (unsigned)((status_y - app.padding) / app.line_height));
	bool status_changed = false;
	for (int y = status_y; y < HEIGHT; ++y)
		status_changed |= memcmp(packed + y * WIDTH,
					 padded + y * STRIDE,
					 WIDTH * sizeof(*packed)) != 0;
	assert(status_changed);
	app_input(&app, KEY_ESCAPE, "");
	assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
	for (int y = 0; y < HEIGHT; ++y)
		assert(!memcmp(packed + y * WIDTH, padded + y * STRIDE,
			       WIDTH * sizeof(*packed)));
	assert(!app_paint(&app, packed, WIDTH, HEIGHT, WIDTH - 1));
	free(packed);
	free(padded);
	/* A visual put uses the clipboard path and replaces, as one undo step. */
	editor_change_end(&app.editor);
	assert(editor_replace(&app.editor, 0, app.editor.length, "one two", 7));
	app.editor.cursor = 0;
	app.editor.mode = NORMAL;
	const char *commands = "yiwwvep";
	while (*commands) {
		char key[2] = { *commands++, 0 };
		app_input(&app, KEY_TEXT, key);
	}
	assert(!strcmp(app.editor.text, "one one"));
	app_input(&app, KEY_TEXT, "u");
	assert(!strcmp(app.editor.text, "one two"));
	app_input(&app, KEY_TEXT, ":");
	app_input(&app, KEY_TEXT, "q!");
	app_input(&app, KEY_ENTER, "");
	assert(!app.running);
	app_close(&app);
	assert(!app.font && !app.editor.text);
	puts("Shared input, close protection, and pitched rendering passed");
}
