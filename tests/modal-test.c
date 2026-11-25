#include "editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct editor ed;
static void keys(const char *s)
{
	while (*s) {
		char c[2] = { *s++, 0 };
		editor_input(&ed, KEY_TEXT, c);
	}
}
static void reset(const char *s)
{
	editor_destroy(&ed);
	ed = (struct editor){ 0 };
	assert(editor_open(&ed, NULL));
	editor_input(&ed, KEY_TEXT, "i");
	editor_input(&ed, KEY_TEXT, s);
	editor_input(&ed, KEY_ESCAPE, "");
	ed.cursor = 0;
	/* Start with no change to repeat, as in a freshly opened file. */
	free(ed.dot);
	ed.dot = NULL;
	ed.dot_length = 0;
}
static void expect(const char *s)
{
	if (strcmp(ed.text, s)) {
		fprintf(stderr, "Expected [%s], got [%s]\n", s, ed.text);
		assert(0);
	}
}
static void readonly_mode(void)
{
	reset("one two\nthree\n");
	uint64_t revision = ed.revision;
	editor_command(&ed, "set view=reading");
	assert(ed.view == VIEW_READING && ed.mode == NORMAL);
	const char *edits[] = { "i",  "a",  "I",  "A", "o", "O", "s", "S",
				"dd", "cw", "x",  "X", "p", "P", "u", "rX",
				"~",  ">>", "<<", "J", "D", "C" };
	for (unsigned i = 0; i < sizeof(edits) / sizeof(edits[0]); ++i) {
		keys(edits[i]);
		expect("one two\nthree\n");
		assert(ed.revision == revision && ed.mode == NORMAL &&
		       !(ed.requests & ED_PASTE));
	}
	const enum editor_key special[] = { KEY_CUT,	KEY_CTRL_V,
					    KEY_DELETE, KEY_BACKSPACE,
					    KEY_TAB,	KEY_SHIFT_TAB,
					    KEY_CTRL_W, KEY_CTRL_R };
	for (unsigned i = 0; i < sizeof(special) / sizeof(special[0]); ++i) {
		editor_input(&ed, special[i], "");
		expect("one two\nthree\n");
		assert(ed.revision == revision &&
		       strstr(ed.message, "read-only"));
	}
	const char *commands[] = { "%s/one/X/g", "1d",	 "1,2>", "1,2<",
				   "1,2j",	 "undo", "redo" };
	for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		editor_command(&ed, commands[i]);
		expect("one two\nthree\n");
		assert(ed.revision == revision &&
		       strstr(ed.message, "read-only"));
	}
	assert(!editor_replace(&ed, 0, 0, "X", 1));
	editor_put(&ed, "X", 1, false, false, 1);
	expect("one two\nthree\n");
	ed.cursor = 0;
	keys("w");
	assert(ed.cursor == 4);
	keys("/three");
	editor_input(&ed, KEY_ENTER, "");
	assert(ed.cursor == 8);
	ed.cursor = 0;
	keys("vly");
	assert(ed.mode == NORMAL && ed.register_length == 2 &&
	       !strcmp(ed.register_text, "on"));
	editor_command(&ed, "set view=normal");
	assert(ed.view == VIEW_NORMAL);
	keys("u");
	expect(""); /* rejected edits did not consume or clear undo history */
}

static void search_visibility(void)
{
	reset("sd other sd\n");
	keys("/sd");
	editor_input(&ed, KEY_ENTER, "");
	assert(!ed.search_hidden && !strcmp(ed.search, "sd"));
	keys("i");
	assert(ed.mode == INSERT && ed.search_hidden &&
	       !strcmp(ed.search, "sd"));
	editor_input(&ed, KEY_ESCAPE, "");
	assert(ed.mode == NORMAL && ed.search_hidden);
	size_t cursor = ed.cursor;
	keys("n");
	assert(!ed.search_hidden && ed.cursor != cursor &&
	       !strcmp(ed.search, "sd"));
	keys("a");
	assert(ed.search_hidden);
	editor_input(&ed, KEY_ESCAPE, "");
	keys("N");
	assert(!ed.search_hidden && !strcmp(ed.search, "sd"));
	const char *inserts[] = { "I", "A",  "o",  "O", "s",
				  "S", "cw", "cc", "C", "vc" };
	for (unsigned i = 0; i < sizeof(inserts) / sizeof(inserts[0]); ++i) {
		reset("sd other sd\n");
		keys("/sd");
		editor_input(&ed, KEY_ENTER, "");
		keys(inserts[i]);
		assert(ed.mode == INSERT && ed.search_hidden &&
		       !strcmp(ed.search, "sd"));
	}
}

static void insert_end(void)
{
	reset("abc\n");
	keys("i");
	editor_input(&ed, KEY_END, "");
	keys("!");
	expect("abc!\n");
	editor_input(&ed, KEY_ESCAPE, "");
	keys("u");
	expect("abc\n");
	reset("before\nlast\n");
	ed.cursor = 7;
	keys("dd");
	expect("before\n");
	keys("uGdd");
	expect("before\nlast");
	reset("caf\xc3\xa9");
	keys("i");
	editor_input(&ed, KEY_END, "");
	keys("!");
	expect("caf\xc3\xa9!");
}

static void case_operators(void)
{
	reset("one two\nthree\n");
	keys("gUiw");
	expect("ONE two\nthree\n");
	keys("wg~e");
	expect("ONE TWO\nthree\n");
	keys("guu");
	expect("one two\nthree\n");
	keys("gUj");
	expect("ONE TWO\nTHREE\n");
	keys("gugu");
	expect("one two\nTHREE\n");
	keys("jg~~");
	expect("one two\nthree\n");
	keys("u");
	expect("one two\nTHREE\n");
	editor_command(&ed, "set view=reading");
	keys("gU");
	expect("one two\nTHREE\n");
	assert(strstr(ed.message, "read-only") && !ed.pending[0]);
}

static void unknown_commands(void)
{
	/* Unsupported commands consume their own keys and leave the text alone. */
	reset("abc def\n");
	const char *ignored[] = { "gZ", "zq", "qa", "@a", "Zx", "dZ", "gR" };
	for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
		keys(ignored[i]);
		expect("abc def\n");
		assert(ed.mode == NORMAL && !ed.pending[0] && ed.message[0]);
	}
	keys("x");
	expect("bc def\n");
	keys("zz");
	assert(!ed.message[0]);
}

static void number_options(void)
{
	/* :set number and relativenumber take Vim's short names, no/inv prefixes and ! ? &. */
	reset("abc\n");
	assert(ed.number && !ed.relativenumber);
	editor_command(&ed, "set nonu");
	assert(!ed.number);
	editor_command(&ed, "set nu?");
	assert(!strcmp(ed.message, "nonumber"));
	editor_command(&ed, "set number");
	assert(ed.number);
	editor_command(&ed, "set nu!");
	assert(!ed.number);
	editor_command(&ed, "set invnumber");
	assert(ed.number);
	editor_command(&ed, "set rnu");
	assert(ed.relativenumber);
	editor_command(&ed, "set relativenumber?");
	assert(!strcmp(ed.message, "  relativenumber"));
	editor_command(&ed, "set nornu nonumber");
	assert(!ed.relativenumber && !ed.number);
	editor_command(&ed, "set  rnu!  nu&");
	assert(ed.relativenumber && ed.number);
	editor_command(&ed, "set norelativenumber");
	assert(!ed.relativenumber);
	editor_command(&ed, "set nonu?");
	assert(strstr(ed.message, "Unknown option") && ed.number);
	editor_command(&ed, "set nu numbers");
	assert(!strcmp(ed.message, "Unknown option: numbers"));
	expect("abc\n");
}

static void text_options(void)
{
	/* colorcolumn takes = += -= ? & and a bare name to show; wrapwidth is a number. */
	reset("abc\n");
	assert(!ed.colorcolumn[0] && ed.wrapwidth == 80);
	editor_command(&ed, "set cc=100,80");
	assert(!strcmp(ed.colorcolumn, "100,80"));
	unsigned columns[4];
	assert(editor_color_columns(&ed, columns, 4) == 2 && columns[0] == 80 &&
	       columns[1] == 100);
	editor_command(&ed, "set cc+=72 cc?");
	assert(!strcmp(ed.message, "  colorcolumn=100,80,72"));
	editor_command(&ed, "set cc-=80");
	assert(!strcmp(ed.colorcolumn, "100,72"));
	editor_command(&ed, "set cc=+1"); /* Vim's offset from textwidth */
	assert(!strcmp(ed.colorcolumn, "100,72") &&
	       strstr(ed.message, "plain columns"));
	editor_command(&ed, "set cc=80,,90");
	assert(!strcmp(ed.colorcolumn, "100,72") &&
	       strstr(ed.message, "80,100"));
	editor_command(&ed, "set colorcolumn=");
	assert(!ed.colorcolumn[0] && !editor_color_columns(&ed, columns, 4));
	editor_command(&ed, "set wrapwidth?");
	assert(!strcmp(ed.message, "  wrapwidth=80"));
	editor_command(&ed, "set wrapwidth=64 wrapwidth+=8");
	assert(ed.wrapwidth == 72);
	editor_command(&ed, "set wrapwidth=x");
	assert(ed.wrapwidth == 72 && strstr(ed.message, "number"));
	editor_command(&ed, "set wrapwidth=0");
	assert(!ed.wrapwidth);
	editor_command(&ed, "set wrapwidth&"); /* back to the default */
	assert(ed.wrapwidth == 80);
	/* textwidth is not supported: typing never adds line breaks, and :set says so. */
	editor_command(&ed, "set tw=20");
	assert(strstr(ed.message, "no textwidth"));
	editor_command(&ed, "set ccc=1");
	assert(strstr(ed.message, "Unknown option"));
	expect("abc\n");
}
static void tab(bool forward)
{
	editor_input(&ed, forward ? KEY_TAB : KEY_SHIFT_TAB, "");
}
static void colorscheme_completion(void)
{
	/* :colo <Tab> steps through the schemes in Vim's alphabetical order, showing each, then
	 * back to what was typed; Esc puts back the scheme from before. */
	reset("abc\n");
	editor_command(&ed, "colorscheme kanagawa");
	keys(":colo ");
	const char *order[] = { "catppuccin", "everforest", "kanagawa", "mib",
				"tokyonight" };
	for (unsigned i = 0; i < 5; ++i) {
		tab(true);
		char expected[32];
		snprintf(expected, sizeof(expected), "colo %s", order[i]);
		assert(!strcmp(ed.command, expected) &&
		       !strcmp(editor_colorscheme_name(ed.colorscheme),
			       order[i]));
	}
	tab(true);
	assert(!strcmp(ed.command, "colo ") &&
	       ed.colorscheme == COLOR_KANAGAWA);
	tab(false);
	assert(!strcmp(ed.command, "colo tokyonight") &&
	       ed.colorscheme == COLOR_TOKYONIGHT);
	editor_input(&ed, KEY_ESCAPE, "");
	assert(!ed.command_active && ed.colorscheme == COLOR_KANAGAWA);
	/* A single match completes and stays; Enter keeps it. */
	keys(":colorscheme e");
	tab(true);
	tab(true);
	assert(!strcmp(ed.command, "colorscheme everforest") &&
	       ed.colorscheme == COLOR_EVERFOREST);
	editor_input(&ed, KEY_ENTER, "");
	assert(ed.colorscheme == COLOR_EVERFOREST && !ed.message[0]);
	/* Editing ends the cycle, and leaving with Backspace also restores. */
	keys(":colo m");
	tab(true);
	assert(ed.colorscheme == COLOR_MIB);
	keys("x");
	tab(true);
	assert(!strcmp(ed.command, "colo mibx"));
	while (ed.command_active)
		editor_input(&ed, KEY_BACKSPACE, "");
	assert(ed.colorscheme == COLOR_EVERFOREST);
	/* Other commands and searches don't complete. */
	keys(":set ");
	tab(true);
	assert(!strcmp(ed.command, "set "));
	editor_input(&ed, KEY_ESCAPE, "");
	keys("/colo ");
	tab(true);
	assert(!strcmp(ed.command, "colo "));
	editor_input(&ed, KEY_ESCAPE, "");
	expect("abc\n");
}
static void word_at_line_end(void)
{
	reset("one two\n  three\n");
	keys("wdw");
	expect("one \n  three\n");
	reset("one two\n  three\n");
	keys("w\"ayw$\"ap");
	expect("one twotwo\n  three\n");
	reset("one two\n  three four\n");
	keys("d3w");
	expect("four\n");
	reset("last word\n");
	keys("wdw");
	expect("last \n");
}

static void dot_repeat(void)
{
	reset("a b c d e f g h\n");
	keys(".");
	expect("a b c d e f g h\n");
	keys("dw..");
	expect("d e f g h\n");
	keys("2.");
	expect("f g h\n");
	keys(".");
	expect("h\n");
	/* Inserts repeat with their typed keys, including Enter and Backspace. */
	reset("one\ntwo\n");
	keys("A!");
	editor_input(&ed, KEY_BACKSPACE, "");
	keys("?");
	editor_input(&ed, KEY_ENTER, "");
	keys("x");
	editor_input(&ed, KEY_ESCAPE, "");
	keys("j.");
	expect("one?\nx\ntwo?\nx\n");
	keys("u");
	expect("one?\nx\ntwo\n");
	/* Yanks, motions, undo, searches and : commands leave the last change alone. */
	reset("aaa bbb ccc\n");
	keys("ciwX");
	editor_input(&ed, KEY_ESCAPE, "");
	keys("wyiw");
	editor_command(&ed, "s/c/C/");
	expect("X bbb Ccc\n");
	keys("0w.");
	expect("X X Ccc\n");
	keys("u");
	expect("X bbb Ccc\n");
	keys("0w.");
	expect("X X Ccc\n");
	/* The register is part of the change. */
	reset("one\ntwo\nthree\n");
	keys("\"Add.");
	assert(!strcmp(ed.registers[1].text, "one\ntwo\n"));
	expect("three\n");
	/* The read-only reading view blocks the repeat. */
	reset("abc\n");
	keys("x");
	editor_command(&ed, "set view=reading");
	keys(".");
	expect("bc\n");
	assert(strstr(ed.message, "read-only"));
}

static void marks_and_jumps(void)
{
	reset("one\ntwo\nthree\nfour\n");
	keys("jlma");
	assert(ed.marks[0] == 6);
	/* Marks follow insertions and deletions before them. */
	keys("ggOzero");
	editor_input(&ed, KEY_ESCAPE, "");
	keys("gg`ax");
	expect("zero\none\nto\nthree\nfour\n");
	keys("ggdd'ax");
	expect("one\no\nthree\nfour\n");
	keys("u");
	/* Deleting the text a mark sits in keeps it at the start of the deletion. */
	reset("abc def\n");
	keys("lma0dw`ax");
	expect("ef\n");
	/* '' and `` return to where a jump started; Ctrl-O and Ctrl-I walk the list. */
	reset("1\n2\n3\n4\n5\n");
	keys("3G''");
	assert(editor_line_number(&ed, ed.cursor) == 0);
	keys("''");
	assert(editor_line_number(&ed, ed.cursor) == 2);
	keys("5G");
	editor_input(&ed, KEY_CTRL_O, "");
	assert(editor_line_number(&ed, ed.cursor) == 2);
	editor_input(&ed, KEY_CTRL_O, "");
	assert(editor_line_number(&ed, ed.cursor) == 0);
	editor_input(&ed, KEY_CTRL_O, "");
	assert(editor_line_number(&ed, ed.cursor) == 0);
	editor_input(&ed, KEY_TAB, "");
	assert(editor_line_number(&ed, ed.cursor) == 2);
	editor_input(&ed, KEY_CTRL_I, "");
	assert(editor_line_number(&ed, ed.cursor) == 4);
	editor_input(&ed, KEY_CTRL_I, "");
	assert(editor_line_number(&ed, ed.cursor) == 4);
	/* Searches and :n are jumps too. */
	keys("gg/4");
	editor_input(&ed, KEY_ENTER, "");
	assert(editor_line_number(&ed, ed.cursor) == 3);
	keys("''");
	assert(editor_line_number(&ed, ed.cursor) == 0);
	editor_command(&ed, "3");
	keys("``");
	assert(ed.cursor == 0);
	/* Unset and unsupported marks report themselves without moving. */
	keys("'b");
	assert(strstr(ed.message, "Mark not set") && ed.cursor == 0);
	keys("mA");
	assert(strstr(ed.message, "not supported"));
	/* The read-only reading view still allows jumping. */
	editor_command(&ed, "set view=reading");
	keys("5G");
	editor_input(&ed, KEY_CTRL_O, "");
	assert(ed.cursor == 0 && !ed.message[0]);
}

/* gq isn't supported (there is no textwidth), but it takes its motion so the keys after it
 * don't run as commands. */
static void format(void)
{
	const char *text = "one two\nthree\n";
	reset(text);
	keys("gqip");
	expect(text);
	assert(ed.mode == NORMAL && strstr(ed.message, "not supported"));
	keys("gqq");
	expect(text);
	keys("Vjgq");
	expect(text);
	assert(ed.mode == NORMAL && strstr(ed.message, "not supported"));
}

static void replace_mode(void)
{
	reset("abc\n");
	keys("Rab");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("abc\n");
	assert(ed.cursor == 1);
	keys("0lRXYZW");
	expect("aXYZW\n");
	editor_input(&ed, KEY_BACKSPACE, "");
	editor_input(&ed, KEY_BACKSPACE, "");
	expect("aXY\n");
	editor_input(&ed, KEY_BACKSPACE, "");
	expect("aXc\n");
	editor_input(&ed, KEY_BACKSPACE, "");
	expect("abc\n");
	/* Backspace past the start only moves left. */
	editor_input(&ed, KEY_BACKSPACE, "");
	assert(ed.cursor == 0);
	keys("Q");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("Qbc\n");
	assert(ed.mode == NORMAL && !ed.replacing);
	/* One undo step, repeatable with ., and later inserts don't type over text. */
	keys("u");
	expect("abc\n");
	reset("abcdef\n");
	keys("R");
	editor_input(&ed, KEY_TEXT, "\xc3\xa9");
	keys("!");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("\xc3\xa9!cdef\n");
	keys("ll.");
	expect("\xc3\xa9!c\xc3\xa9!f\n");
	keys("0i-");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("-\xc3\xa9!c\xc3\xa9!f\n");
	/* Enter breaks the line, and the rest is still typed over. */
	reset("abcd\n");
	keys("lRx");
	editor_input(&ed, KEY_ENTER, "");
	keys("y");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("ax\nyd\n");
	editor_command(&ed, "set view=reading");
	keys("R");
	assert(ed.mode == NORMAL && strstr(ed.message, "read-only"));
}

static void registers(void)
{
	reset("one\ntwo\nthree\n");
	keys("\"ayy");
	assert(ed.registers[1].text && !strcmp(ed.registers[1].text, "one\n") &&
	       ed.registers[1].linewise);
	assert(!ed.registers[0].text && (ed.requests & ED_COPY));
	ed.requests = 0;
	keys("j\"Ayiw");
	assert(!strcmp(ed.registers[1].text, "one\ntwo\n") &&
	       ed.registers[1].linewise);
	keys("G\"ap");
	assert(!(ed.requests & ED_PASTE));
	expect("one\ntwo\nthree\none\ntwo\n");
	/* "_ leaves the clipboard alone; plain yanks fill "0 and deletes don't. */
	reset("one\ntwo\n");
	keys("\"_dd");
	expect("two\n");
	assert(!(ed.requests & ED_COPY) && !ed.register_text);
	keys("yiw");
	assert(!strcmp(ed.registers[0].text, "two") &&
	       !ed.registers[0].linewise);
	keys("x\"0P");
	expect("twowo\n");
	/* Empty and unsupported registers report themselves. */
	keys("\"zp");
	assert(strstr(ed.message, "Nothing in register z"));
	keys("\"1");
	assert(strstr(ed.message, "not supported") && !ed.pending[0]);
	/* Plain p and "+p still go through the clipboard. */
	ed.requests = 0;
	keys("\"+p");
	assert(ed.requests & ED_PASTE);
	/* Deleting an image clears registers that refer to it. */
	reset("![](assets/a.png)\nx\n");
	keys("\"byy");
	editor_forget_registers(&ed, "assets/a.png");
	assert(!ed.registers[1].text && !ed.register_text);
}

/* :ls (or :buffers, :files) asks the app for the list of pages and :b N for page N;
 * quitting also guards the app's other pages. */
static void page_commands(void)
{
	reset("notes");
	const char *lists[] = { "ls", "buffers", "files" };
	for (unsigned i = 0; i < 3; ++i) {
		editor_command(&ed, lists[i]);
		assert(ed.requests == ED_PAGE_MAP && !ed.message[0]);
		ed.requests = 0;
	}
	editor_command(&ed, "ls x y");
	assert(!ed.requests && strstr(ed.message, "Unknown command"));
	ed.message[0] = 0;
	const char *switches[] = { "b 3", "b3", "buffer 9", "b  1" };
	const unsigned targets[] = { 2, 2, 8, 0 };
	for (unsigned i = 0; i < 4; ++i) {
		editor_command(&ed, switches[i]);
		assert(ed.requests == ED_PAGE_SWITCH &&
		       ed.page_target == targets[i] && !ed.message[0]);
		ed.requests = 0;
	}
	/* :wa and friends save this file and ask the app for the others; :wqa and :xa then quit. */
	const char *saves[] = { "wa", "wall", "wqa", "wqall", "xa", "xall" };
	editor_command(
		&ed, "wa"); /* this file has no name, so nothing more happens */
	assert(!ed.requests && strstr(ed.message, "No filename"));
	ed.modified = false;
	for (unsigned i = 0; i < 6; ++i) {
		editor_command(&ed, saves[i]);
		assert(ed.requests ==
			       (ED_SAVE_ALL | (i < 2 ? 0 : ED_QUIT_ALL)) &&
		       !ed.quit);
		ed.requests = 0;
	}
	const char *bad[] = { "b", "b 0", "b 10", "b x", "b3 4", "buffer" };
	for (unsigned i = 0; i < 6; ++i) {
		editor_command(&ed, bad[i]);
		assert(!ed.requests && strstr(ed.message, ":b 1 to :b 9"));
		ed.message[0] = 0;
	}
	ed.modified = false;
	ed.other_modified = 4;
	const char *quits[] = { "q", "x", "wq" };
	for (unsigned i = 0; i < 3; ++i) {
		if (i == 2)
			ed.filename =
				NULL; /* :wq fails to save an unnamed buffer before checking pages */
		editor_command(&ed, quits[i]);
		assert(!ed.quit);
		if (i < 2)
			assert(strstr(ed.message, "page 4"));
	}
	keys("i");
	editor_input(&ed, KEY_CTRL_Q, "");
	assert(!ed.quit);
	editor_input(&ed, KEY_ESCAPE, "");
	editor_command(&ed, "q!");
	assert(ed.quit);
	ed.quit = false;
	ed.other_modified = 0;
	ed.modified = false;
	editor_command(&ed, "q");
	assert(ed.quit);
	ed.quit = false;
}

int main(void)
{
	readonly_mode();
	page_commands();
	search_visibility();
	insert_end();
	case_operators();
	unknown_commands();
	number_options();
	text_options();
	colorscheme_completion();
	registers();
	word_at_line_end();
	dot_repeat();
	marks_and_jumps();
	format();
	replace_mode();
	reset("notes");
	const char *removed[] = { "noh",      "nohlsearch", "set md",
				  "set nomd", "set light",  "set dark" };
	for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
		editor_command(&ed, removed[i]);
		assert(strstr(ed.message, "Unknown"));
		expect("notes");
	}
	editor_command(&ed, "set view=source");
	assert(ed.view == VIEW_SOURCE);
	editor_command(&ed, "set view=normal");
	assert(ed.view == VIEW_NORMAL);
	editor_command(&ed, "set view?");
	assert(!strcmp(ed.message, "view=normal"));
	editor_command(&ed, "set view=polish"); /* the old names are gone */
	assert(ed.view == VIEW_NORMAL &&
	       strstr(ed.message, "source, normal and reading"));
	editor_command(&ed, "set raw");
	assert(ed.view == VIEW_NORMAL && strstr(ed.message, "Unknown"));
	/* :h, :help and :help with a topic all ask for the help document. */
	const char *helps[] = { "h", "he", "help", "help marks" };
	for (unsigned i = 0; i < 4; ++i) {
		ed.requests = 0;
		editor_command(&ed, helps[i]);
		assert(ed.requests == ED_HELP);
	}
	/* Vim options mib leaves out on purpose say what to use instead. */
	const struct {
		const char *command, *message;
	} missing[] = {
		{ "set tw=79", "no textwidth" },
		{ "set textwidth?", "no textwidth" },
		{ "set nowrap", "always soft-wraps" },
		{ "set wrap", "always soft-wraps" },
		{ "set lbr", "word boundaries" },
		{ "set clipboard=unnamedplus", "system clipboard" },
		{ "set cc=+1", "plain columns, such as 81" },
		{ "set cc+=-1", "plain columns, such as 81" },
		{ "set cc=abc", "such as 80,100" },
		{ "set tw=79 nu", "no textwidth" },
	};
	bool numbered = ed.number;
	char columns[sizeof(ed.colorcolumn)];
	memcpy(columns, ed.colorcolumn, sizeof(columns));
	for (unsigned i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
		ed.message[0] = 0;
		editor_command(&ed, missing[i].command);
		assert(strstr(ed.message, missing[i].message));
	}
	assert(ed.number == numbered && !strcmp(ed.colorcolumn, columns));
	editor_command(&ed, "set wrapscan");
	assert(strstr(ed.message, "Unknown option: wrapscan"));
	/* :ve, :version and the abbreviations between report the build, as mib --version does. */
	const char *versions[] = { "ve", "vers", "version" };
	for (unsigned i = 0; i < 3; ++i) {
		ed.message[0] = 0;
		editor_command(&ed, versions[i]);
		assert(!strncmp(ed.message, "mib ", 4) &&
		       strstr(ed.message, ", commit "));
	}
	editor_command(&ed, "version extra");
	assert(strstr(ed.message, "Unknown"));
	ed.requests = 0;
	uint64_t revision = ed.revision;
	const char *schemes[] = { "mib", "tokyonight", "catppuccin", "kanagawa",
				  "everforest" };
	for (unsigned i = 0; i < COLOR_COUNT; ++i) {
		char command[80];
		snprintf(command, sizeof(command), "colorscheme %s",
			 schemes[i]);
		editor_command(&ed, command);
		assert(ed.colorscheme == (enum color_scheme)i);
		editor_command(&ed, "colorscheme");
		assert(!strcmp(ed.message, schemes[i]));
		editor_command(&ed, "set background=light");
		assert(ed.light);
		editor_command(&ed, "set bg?");
		assert(!strcmp(ed.message, "background=light"));
		editor_command(&ed, "set bg=dark");
		assert(!ed.light);
		editor_command(&ed, "set background?");
		assert(!strcmp(ed.message, "background=dark"));
		assert(ed.revision == revision);
		expect("notes");
	}
	editor_command(&ed, "colo mib");
	assert(ed.colorscheme == COLOR_MIB);
	editor_command(&ed, "colorscheme missing");
	assert(strstr(ed.message, "Cannot find color scheme") &&
	       ed.colorscheme == COLOR_MIB);
	editor_command(&ed, "set background=invalid");
	assert(strstr(ed.message, "Unknown option") && !ed.light);
	editor_command(&ed, "set view=reading");
	editor_command(&ed, "set bg=light");
	editor_command(&ed, "colorscheme catppuccin");
	assert(ed.colorscheme == COLOR_CATPPUCCIN && ed.light &&
	       ed.view == VIEW_READING);
	const char *retired[] = { "default", "desert", "evening", "peachpuff" };
	for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
		char command[80];
		snprintf(command, sizeof(command), "colorscheme %s",
			 retired[i]);
		editor_command(&ed, command);
		assert(strstr(ed.message, "Cannot find color scheme") &&
		       ed.colorscheme == COLOR_CATPPUCCIN);
	}
	editor_command(&ed, "colorscheme mib");
	editor_command(&ed, "set view=normal");
	reset("one two three\nfour five\nsix\n");
	keys("2dw");
	expect("three\nfour five\nsix\n");
	keys("u");
	expect("one two three\nfour five\nsix\n");
	keys("d0");
	expect("one two three\nfour five\nsix\n");
	keys("2dd");
	expect("six\n");
	keys("u");
	keys("d3j");
	expect("");
	keys("u");
	keys("ciw");
	keys("hello");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("hello two three\nfour five\nsix\n");
	keys("u");
	expect("one two three\nfour five\nsix\n");
	editor_input(&ed, KEY_CTRL_R, "");
	expect("hello two three\nfour five\nsix\n");
	reset("a (two [three]) tail");
	keys("f(ci(");
	keys("four");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("a (four) tail");
	keys("u");
	expect("a (two [three]) tail");
	keys("%");
	assert(ed.text[ed.cursor] == ')');
	reset("one two\nthree four\nfive");
	keys("vwd");
	expect("wo\nthree four\nfive");
	keys("u");
	keys("Vjy");
	assert(ed.register_linewise &&
	       !strcmp(ed.register_text, "one two\nthree four\n"));
	reset("  alpha\n  beta\ngamma");
	keys(">>");
	expect("      alpha\n  beta\ngamma");
	keys("<<");
	expect("  alpha\n  beta\ngamma");
	keys("J");
	expect("  alpha beta\ngamma");
	keys("u");
	keys("A");
	editor_input(&ed, KEY_ENTER, "");
	keys("new");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("  alpha\n  new\n  beta\ngamma");
	keys("u");
	expect("  alpha\n  beta\ngamma");
	reset("foo Foo FOO food\nfoo bar foo");
	keys("/foo");
	assert(ed.cursor == 4);
	editor_input(&ed, KEY_ENTER, "");
	assert(ed.cursor == 4);
	keys("n");
	assert(ed.cursor == 8);
	keys("N");
	assert(ed.cursor == 4);
	keys("/FOO");
	editor_input(&ed, KEY_ENTER, "");
	assert(ed.cursor == 8);
	editor_command(&ed, "%s/\\(foo\\)/[\\1]/gi");
	expect("[foo] [Foo] [FOO] [foo]d\n[foo] bar [foo]");
	keys("u");
	expect("foo Foo FOO food\nfoo bar foo");
	editor_command(&ed, "%s/\\<foo\\>/X/g");
	expect("X Foo FOO food\nX bar X");
	keys("u");
	editor_command(&ed, "%s/f\\(oo\\|ood\\)/<\\1>/g");
	expect("<oo> Foo FOO <oo>d\n<oo> bar <oo>");
	keys("u");
	editor_command(&ed, "2s/o\\+/O/g");
	expect("foo Foo FOO food\nfO bar fO");
	keys("u");
	editor_command(&ed, "%s/^/start /g");
	expect("start foo Foo FOO food\nstart foo bar foo");
	keys("u");
	editor_command(&ed, "1,2>");
	expect("    foo Foo FOO food\n    foo bar foo");
	editor_command(&ed, "%<");
	expect("foo Foo FOO food\nfoo bar foo");
	editor_command(&ed, "2");
	assert(ed.cursor == 17);
	editor_command(&ed, "1d");
	expect("foo bar foo");
	keys("u");
	expect("foo Foo FOO food\nfoo bar foo");
	reset("abc");
	keys("A");
	for (int i = 0; i < 20000; ++i)
		keys("x");
	editor_input(&ed, KEY_ESCAPE, "");
	keys("u");
	expect("abc");
	editor_input(&ed, KEY_CTRL_R, "");
	assert(ed.length == 20003);
	keys("u");
	keys("ix");
	editor_input(&ed, KEY_ESCAPE, "");
	editor_input(&ed, KEY_CTRL_R, "");
	expect("abxc");
	reset("intro\n# One\ntext\n```\n# fake\n```\n## Two\nend");
	keys("]]");
	assert(editor_line_number(&ed, ed.cursor) == 1);
	keys("]]");
	assert(editor_line_number(&ed, ed.cursor) == 6);
	keys("[[");
	assert(editor_line_number(&ed, ed.cursor) == 1);
	reset("  text");
	keys("A");
	editor_input(&ed, KEY_TAB, "");
	expect("  text    ");
	editor_input(&ed, KEY_SHIFT_TAB, "");
	keys("!");
	editor_input(&ed, KEY_ESCAPE, "");
	expect("text    !");
	keys("u");
	expect("  text");
	reset("foo food foo");
	keys("*");
	assert(ed.cursor == 9);
	keys("n");
	assert(ed.cursor == 0);
	reset("a é b é c");
	keys("f");
	editor_input(&ed, KEY_TEXT, "é");
	assert(ed.cursor == 2);
	keys(";");
	assert(ed.cursor == 7);
	keys(",");
	assert(ed.cursor == 2);
	keys(";");
	assert(ed.cursor == 7);
	editor_destroy(&ed);
	puts("Modal operators, search, substitutions, grouped undo and redo passed");
}
