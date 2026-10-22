/* main.c - EX-word DATAPLUS 6 用 オフライン Wikipedia ビューア
 *   画面: 528x320 RGB565 / 文字: WIKI.FNT(16x16 全角 + 8x16 半角)
 *   データ: WIKI.IDX / WIKI.Dnn / WIKI.Inn (wikipack.py で作成。文字コードは cp932)
 */
#include <stdint.h>
#include <string.h>
#include <graphics/drawing.h>
#include <graphics/color.h>
#include <graphics/lcdc.h>
#include <graphics/text.h>
#include <sh4a/input/keypad.h>
#include <syscalls/syscalls.h>
#include "core.h"
#include "img.h"
#include "strings.h"
#include "libc/memmgr.h"

#define SW 528
#define SH 320
#define LH 18          /* 行の高さ */
#define BAR_H 20
#define BODY_Y 24
#define VIS 15         /* 本文の表示行数 */
#define X0 8
#define TEXT_W 504
#define LIST_ROWS 12
#define LIST_Y 52
#define HIST 32
#define VIEW_IMG_W 240
#define VIEW_IMG_H 160

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BG     RGB(255, 255, 255)
#define C_FG     RGB(20, 20, 20)
#define C_LINK   RGB(0, 0, 200)
#define C_HEAD   RGB(150, 0, 0)
#define C_BAR    RGB(30, 60, 140)
#define C_WHITE  RGB(255, 255, 255)
#define C_SEL    RGB(0, 0, 200)
#define C_TRACK  RGB(210, 210, 210)
#define C_THUMB  RGB(90, 90, 90)

/* ================================================================ platform */
static char base[64];
static int fds[4][100];
static int nopen;
static int base_ok;

/* ---- 診断用の記録(起動に失敗したとき画面に出す) ---- */
#define DBG_N 12
static char dbg_path[DBG_N][48];
static int dbg_fd[DBG_N];
static int dbg_cnt;
static unsigned long dbg_drv;
static char dbg_id[16];
static int dbg_read_ret;

static void dbg_rec(const char *path, int fd)
{
	if (dbg_cnt < DBG_N) {
		int i;
		for (i = 0; i < 47 && path[i]; i++)
			dbg_path[dbg_cnt][i] = path[i];
		dbg_path[dbg_cnt][i] = 0;
		dbg_fd[dbg_cnt] = fd;
		dbg_cnt++;
	}
}

/* 開く。だめなら、ファイル名部分を小文字にしてもう一度試す(送信時に小文字化される場合に備える) */
static int try_open(const char *path)
{
	char low[96];
	int fd, i, name = 0;
	fd = sys_open(path, FILE_RD);
	dbg_rec(path, fd);
	if (fd >= 0)
		return fd;
	for (i = 0; path[i] && i < 95; i++) {
		low[i] = path[i];
		if (path[i] == '\\')
			name = i + 1;
	}
	low[i] = 0;
	for (i = name; low[i]; i++)
		if (low[i] >= 'A' && low[i] <= 'Z')
			low[i] += 'a' - 'A';
	if (strcmp(low, path) == 0)
		return fd;
	fd = sys_open(low, FILE_RD);
	dbg_rec(low, fd);
	return fd;
}

static void make_name(char *dst, const char *b, int kind, int no)
{
	char t[4];
	strcpy(dst, b);
	if (kind == KIND_IDX) {
		strcat(dst, "WIKI.IDX");
	} else if (kind == KIND_FNT) {
		strcat(dst, "WIKI.FNT");
	} else {
		strcat(dst, "WIKI.");
		t[0] = kind == KIND_DAT ? 'D' : 'I';
		t[1] = '0' + (no / 10) % 10;
		t[2] = '0' + no % 10;
		t[3] = 0;
		strcat(dst, t);
	}
}

/* libexword の send で送ったファイルはドライブのルート(\\drv0\ か \\crd0\)に置かれる。
 * 念のためアプリ用フォルダ(<APPID>\_USER\ と <APPID>\)も候補にし、
 * 最初に WIKI.IDX が開けた場所を以後ずっと使う。起動したメディアを先に試す。 */
static int find_base(void)
{
	static const char *media[2] = {"\\\\drv0\\", "\\\\crd0\\"};
	char id[16], path[96];
	int m, s, fd;
	memset(id, 0, sizeof id);
	sys_dict_info(&dbg_drv, id);
	memcpy(dbg_id, id, sizeof id);
	for (m = 0; m < 2; m++) {
		int mm = ((int)dbg_drv == 1) ? 1 - m : m;
		for (s = 0; s < 3; s++) {
			strcpy(base, media[mm]);
			if (s > 0) {
				strcat(base, id);
				strcat(base, s == 1 ? "\\_USER\\" : "\\");
			}
			make_name(path, base, KIND_IDX, 0);
			fd = try_open(path);
			if (fd >= 0) {
				sys_close(fd);
				return 0;
			}
		}
	}
	return -1;
}

int plat_read(int kind, int no, uint32_t off, void *buf, uint32_t len)
{
	int *slot;
	uint32_t got = 0;
	if (!base_ok) {
		int i, j;
		for (i = 0; i < 4; i++)
			for (j = 0; j < 100; j++)
				fds[i][j] = -1;
		if (find_base() < 0)
			return -1;
		base_ok = 1;
	}
	if (no < 0 || no > 99)
		return -1;
	slot = &fds[kind][no];
	if (*slot < 0) {
		char path[96];
		if (nopen >= 8) { /* 開きすぎないよう、一旦全部閉じる */
			int i, j;
			sys_closeall();
			for (i = 0; i < 4; i++)
				for (j = 0; j < 100; j++)
					fds[i][j] = -1;
			nopen = 0;
		}
		make_name(path, base, kind, no);
		*slot = try_open(path);
		if (*slot < 0)
			return -1;
		nopen++;
	}
	/* 1回の sys_read に上限があるかもしれないので、16KB ずつ、毎回 seek し直して読む */
	while (got < len) {
		uint32_t c = len - got > 16384 ? 16384 : len - got;
		int r;
		if (sys_seek(*slot, (int)(off + got), 0) < 0)
			break;
		r = sys_read(*slot, (char *)buf + got, (int)c);
		dbg_read_ret = r;
		if (r <= 0)
			break;
		got += (uint32_t)r;
	}
	return (int)got;
}

void *plat_alloc(uint32_t n)
{
	return memmgr_alloc(n);
}

/* ================================================================== video */
static uint16_t *vram;
static uint16_t *imgbuf;
static int img_w, img_h;

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
	int xx, yy;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > SW) w = SW - x;
	if (y + h > SH) h = SH - y;
	for (yy = 0; yy < h; yy++) {
		uint16_t *d = vram + (y + yy) * SW + x;
		for (xx = 0; xx < w; xx++)
			d[xx] = c;
	}
}

static void blit_bits(int x, int y, const uint8_t *g, int w, int h, uint16_t fg, uint16_t bg, int opaque)
{
	int sb = (w + 7) >> 3, r, c;
	for (r = 0; r < h; r++) {
		const uint8_t *row = g ? g + r * sb : 0;
		int py = y + r;
		uint16_t *dst;
		if (py < 0 || py >= SH)
			continue;
		dst = vram + py * SW;
		for (c = 0; c < w; c++) {
			int px = x + c;
			if (px < 0 || px >= SW)
				continue;
			if (row && (row[c >> 3] & (0x80 >> (c & 7))))
				dst[px] = fg;
			else if (opaque)
				dst[px] = bg;
		}
	}
}

/* s の先頭1文字(全角/半角)のグリフと幅を返す。戻り値は消費バイト数 */
static int glyph_at(const uint8_t *s, int avail, const uint8_t **g, int *w)
{
	int row, cell;
	if (sjis_is_lead(s[0]) && avail >= 2) {
		*g = (sjis_to_rc(s[0], s[1], &row, &cell) == 0) ? font_full(row, cell) : 0;
		*w = g_fw;
		return 2;
	}
	*g = font_half(s[0]);
	*w = g_hw;
	return 1;
}

static int draw_sjis(int x, int y, const uint8_t *s, int len, uint16_t fg, uint16_t bg, int opaque)
{
	int i = 0;
	while (i < len && s[i]) {
		const uint8_t *g;
		int w, step = glyph_at(s + i, len - i, &g, &w);
		if (x + w > SW)
			break;
		blit_bits(x, y, g, w, g_fh, fg, bg, opaque);
		x += w;
		i += step;
	}
	return x;
}

static int draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int opaque)
{
	return draw_sjis(x, y, (const uint8_t *)s, (int)strlen(s), fg, bg, opaque);
}

static void draw_bar_top(const uint8_t *title)
{
	fill_rect(0, 0, SW, BAR_H, C_BAR);
	draw_sjis(X0, 2, title, (int)strlen((const char *)title), C_WHITE, C_BAR, 0);
}

static void draw_bar_bottom(const char *help)
{
	fill_rect(0, SH - BAR_H, SW, BAR_H, C_BAR);
	draw_str(X0, SH - BAR_H + 2, help, C_WHITE, C_BAR, 0);
}

/* ================================================================== state */
static struct pack pk;
enum { M_SEARCH, M_ARTICLE };
static int mode = M_SEARCH;

/* search */
static char rom[48];
static int romlen, abc_mode;
static uint8_t qbuf[64];
static int qlen;
static char tail[8];
static uint32_t lb, sel, top_idx;

/* article */
static uint8_t cur_title[TITLE_MAX];
static uint32_t cur_id;
static int top, cur_link;
static struct { uint32_t id; int top; } hist[HIST];
static int hist_n;

static void refresh_query(void)
{
	if (abc_mode) {
		int i;
		for (i = 0; i < romlen && i < (int)sizeof qbuf - 1; i++)
			qbuf[i] = (uint8_t)rom[i];
		qlen = i;
		tail[0] = 0;
	} else {
		qlen = romaji_to_kana(rom, qbuf, sizeof qbuf, tail, sizeof tail);
	}
	lb = pack_lower_bound(&pk, qbuf, qlen);
	sel = top_idx = lb;
}

static int match_at(uint32_t idx, uint32_t *aid)
{
	uint8_t key[KEY_MAX + 1];
	int klen;
	if (idx >= pk.n_key || pack_key(&pk, idx, key, &klen, aid) < 0)
		return 0;
	return key_has_prefix(key, klen, qbuf, qlen);
}

static int max_top(void) { return doc_nlines > VIS ? doc_nlines - VIS : 0; }

static int open_article(uint32_t aid)
{
	struct art a;
	int rc, jl;
	if (pack_art(&pk, aid, &a) < 0)
		return -1;
	rc = pack_body(&pk, &a);
	if (rc < 0)
		return -1;
	img_w = img_h = 0;
	jl = pack_image(&pk, &a);
	if (jl > 0 && img_decode(g_jpg, (uint32_t)jl, imgbuf, VIEW_IMG_W, VIEW_IMG_H, &img_w, &img_h) != 0)
		img_w = img_h = 0;
	doc_build((uint32_t)rc, img_w ? (img_h + LH - 1) / LH + 1 : 0, TEXT_W);
	memcpy(cur_title, a.title, TITLE_MAX);
	cur_id = aid;
	top = 0;
	cur_link = 0;
	return 0;
}

/* ================================================================== draws */
static void draw_doc_line(int li, int y)
{
	const struct dline *L = &doc_lines[li];
	uint32_t j = L->off, end = L->off + L->len;
	int x = X0;
	uint16_t base_fg = (L->flags & LF_HEAD) ? C_HEAD : C_FG;
	while (j < end) {
		const uint8_t *g;
		int w, link = doc_lnk[j], selected = (link && link == cur_link);
		int step = glyph_at(doc_txt + j, (int)(end - j), &g, &w);
		uint16_t fg = base_fg;
		if (link)
			fg = selected ? C_WHITE : C_LINK;
		blit_bits(x, y, g, w, g_fh, fg, C_SEL, selected);
		if (link && !selected)
			fill_rect(x, y + g_fh, w, 1, C_LINK);
		x += w;
		j += step;
	}
	if (L->flags & LF_HEAD)
		fill_rect(X0, y + g_fh, TEXT_W, 1, C_HEAD);
}

static void draw_article(void)
{
	int r, y0;
	fill_rect(0, 0, SW, SH, C_BG);
	draw_bar_top(cur_title);
	if (img_w) {
		y0 = BODY_Y - top * LH + 2;
		for (r = 0; r < img_h; r++) {
			int py = y0 + r, c;
			if (py < BODY_Y || py >= BODY_Y + VIS * LH)
				continue;
			for (c = 0; c < img_w; c++)
				vram[py * SW + X0 + c] = imgbuf[r * img_w + c];
		}
	}
	for (r = 0; r < VIS; r++) {
		int li = top + r;
		if (li >= doc_nlines)
			break;
		if (doc_lines[li].len)
			draw_doc_line(li, BODY_Y + r * LH);
	}
	if (doc_nlines > VIS) { /* スクロールバー */
		int th = VIS * LH, bh = th * VIS / doc_nlines, by;
		if (bh < 8) bh = 8;
		by = (th - bh) * top / max_top();
		fill_rect(SW - 6, BODY_Y, 4, th, C_TRACK);
		fill_rect(SW - 6, BODY_Y + by, 4, bh, C_THUMB);
	}
	draw_bar_bottom(S_HELP_ART);
}

static void draw_search(void)
{
	uint32_t i, aid, idx;
	int x, n = 0;
	fill_rect(0, 0, SW, SH, C_BG);
	draw_bar_top((const uint8_t *)S_TITLE_SEARCH);
	x = draw_str(X0, 28, S_PROMPT, C_FG, C_BG, 0);
	x = draw_sjis(x + 4, 28, qbuf, qlen, C_FG, C_BG, 0);
	if (tail[0])
		x = draw_str(x, 28, tail, C_HEAD, C_BG, 0);
	fill_rect(x + 1, 28, 2, g_fh, C_FG); /* カーソル */
	draw_str(SW - 64, 28, abc_mode ? S_MODE_ABC : S_MODE_KANA, C_LINK, C_BG, 0);
	fill_rect(X0, 48, SW - 2 * X0, 1, C_TRACK);
	for (i = 0; i < LIST_ROWS; i++) {
		struct art a;
		idx = top_idx + i;
		if (!match_at(idx, &aid) || pack_art(&pk, aid, &a) < 0)
			break;
		if (idx == sel) {
			fill_rect(X0 - 2, LIST_Y + i * LH - 1, SW - 2 * X0 + 4, LH, C_SEL);
			draw_sjis(X0, LIST_Y + i * LH, a.title, (int)strlen((char *)a.title), C_WHITE, C_SEL, 0);
		} else {
			draw_sjis(X0, LIST_Y + i * LH, a.title, (int)strlen((char *)a.title), C_FG, C_BG, 0);
		}
		n++;
	}
	if (!n)
		draw_str(X0, LIST_Y, S_NOHIT, C_HEAD, C_BG, 0);
	draw_bar_bottom(S_HELP_SEARCH);
}

/* ---- 起動失敗の表示。日本語フォントが無くても読めるよう、組み込みの英数字フォントで出す ---- */
static int dbg_y;
static void dbg_line(const char *t)
{
	render_text(6, dbg_y, (char *)t);
	dbg_y += 13;
}

static void num(char *dst, int v)
{
	char t[12];
	int n = 0, neg = v < 0;
	unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
	do { t[n++] = '0' + u % 10; u /= 10; } while (u);
	if (neg) *dst++ = '-';
	while (n) *dst++ = t[--n];
	*dst = 0;
}

static void hex2(char *dst, uint8_t v)
{
	static const char H[] = "0123456789abcdef";
	dst[0] = H[v >> 4];
	dst[1] = H[v & 15];
	dst[2] = 0;
}

static void kv(const char *k, int v)
{
	char b[48], n[12];
	strcpy(b, k);
	num(n, v);
	strcat(b, n);
	dbg_line(b);
}

/* what: 失敗した段階の名前(英数字) rc: その戻り値 */
static int fatal(const char *what, int rc)
{
	int i;
	char b[64], h[4];
	fill_rect(0, 0, SW, SH, C_BG);
	set_pen(0x0000);
	dbg_y = 6;
	{ char t[32], n[12]; strcpy(t, "EXWIKI build "); num(n, 4); strcat(t, n); strcat(t, ": start failed"); dbg_line(t); }
	strcpy(b, "step: ");
	strcat(b, what);
	dbg_line(b);
	kv("rc=", rc);
	strcpy(b, "drv=");
	{ char n[12]; num(n, (int)dbg_drv); strcat(b, n); strcat(b, " id="); strcat(b, dbg_id); }
	dbg_line(b);
	kv("font_read=", g_font_n);
	kv("font_need=", g_font_need);
	strcpy(b, "font_magic=");
	for (i = 0; i < 4; i++) { hex2(h, g_font_magic[i]); strcat(b, h); }
	dbg_line(b);
	kv("last_sys_read=", dbg_read_ret);
	dbg_line("open attempts (fd, path):");
	for (i = 0; i < dbg_cnt && dbg_y < SH - 30; i++) {
		char n[12];
		num(n, dbg_fd[i]);
		strcpy(b, n);
		strcat(b, " ");
		strcat(b, dbg_path[i]);
		dbg_line(b);
	}
	dbg_line("press BACK / ENTER / POWER");
	lcdc_copy_vram();
	for (;;) {
		keypad_read();
		if (get_key_pressed(KEY_POWER) || get_key_pressed(KEY_BACK) || get_key_pressed(KEY_ENTER))
			return -2;
	}
}

/* 記事の読み込み中表示(展開・画像デコードに少し時間がかかる。止まったときの目印にもなる) */
static void show_loading(void)
{
	fill_rect(0, SH - BAR_H, SW, BAR_H, C_BAR);
	set_pen(0xFFFF);
	render_text(6, SH - BAR_H + 4, "loading...");
	lcdc_copy_vram();
}

#define BUILD_ID 4

/* 起動の進み具合を英数字で画面に出す(止まった場所が分かるように) */
static void boot(const char *t, int v, int with_num)
{
	char b[64], n[12];
	strcpy(b, t);
	if (with_num) {
		num(n, v);
		strcat(b, n);
	}
	dbg_line(b);
	lcdc_copy_vram();
}

/* ================================================================== input */
static const struct { uint8_t code; char ch; } LETTERS[] = {
	{KEY_CHAR_A, 'a'}, {KEY_CHAR_B, 'b'}, {KEY_CHAR_C, 'c'}, {KEY_CHAR_D, 'd'}, {KEY_CHAR_E, 'e'},
	{KEY_CHAR_F, 'f'}, {KEY_CHAR_G, 'g'}, {KEY_CHAR_H, 'h'}, {KEY_CHAR_I, 'i'}, {KEY_CHAR_J, 'j'},
	{KEY_CHAR_K, 'k'}, {KEY_CHAR_L, 'l'}, {KEY_CHAR_M, 'm'}, {KEY_CHAR_N, 'n'}, {KEY_CHAR_O, 'o'},
	{KEY_CHAR_P, 'p'}, {KEY_CHAR_Q, 'q'}, {KEY_CHAR_R, 'r'}, {KEY_CHAR_S, 's'}, {KEY_CHAR_T, 't'},
	{KEY_CHAR_U, 'u'}, {KEY_CHAR_V, 'v'}, {KEY_CHAR_W, 'w'}, {KEY_CHAR_X, 'x'}, {KEY_CHAR_Y, 'y'},
	{KEY_CHAR_Z, 'z'},
};

static void show_link(void)
{
	int L = doc_link_line[cur_link];
	if (L < top)
		top = L;
	else if (L >= top + VIS)
		top = L - 2;
	if (top > max_top())
		top = max_top();
	if (top < 0)
		top = 0;
}

int main(void)
{
	int dirty = 1, i;

	vram = (uint16_t *)lcdc_get_vram_address();
	fill_rect(0, 0, SW, SH, C_BG);
	set_pen(0x0000);
	dbg_y = 6;
	boot("EXWIKI build ", BUILD_ID, 1);
	memmgr_init();
	boot("1 memmgr_init ok", 0, 0);
	imgbuf = memmgr_alloc(VIEW_IMG_W * VIEW_IMG_H * 2);
	if (core_init() < 0 || !imgbuf)
		return fatal("alloc (memmgr)", imgbuf ? 0 : -1);
	boot("2 alloc ok", 0, 0);
	{
		int rc = font_load();
		boot("3 font bytes=", g_font_n, 1);
		if (rc < 0)
			return fatal("load WIKI.FNT", rc);
		rc = pack_open(&pk);
		if (rc != PACK_OK)
			return fatal("open WIKI.IDX", rc);
		boot("4 pack articles=", (int)pk.n_art, 1);
	}
	refresh_query();

	for (;;) {
		uint32_t aid;
		int up, down, left, right, enter, back, bs, pgu, pgd, shift, sym, f1;
		keypad_read();
		if (get_key_pressed(KEY_POWER))
			return -2;
		up = get_key_pressed(KEY_UP);
		down = get_key_pressed(KEY_DOWN);
		left = get_key_pressed(KEY_LEFT);
		right = get_key_pressed(KEY_RIGHT);
		enter = get_key_pressed(KEY_ENTER);
		back = get_key_pressed(KEY_BACK);
		bs = get_key_pressed(KEY_BACKSPACE);
		pgu = get_key_pressed(KEY_PAGE_UP);
		pgd = get_key_pressed(KEY_PAGE_DOWN);
		shift = get_key_pressed(KEY_SHIFT);
		sym = get_key_pressed(KEY_SYMBOL);
		f1 = get_key_pressed(KEY_FUNC_1);

		if (mode == M_SEARCH) {
			for (i = 0; i < 26; i++) {
				if (get_key_pressed(LETTERS[i].code) && romlen < (int)sizeof rom - 1) {
					rom[romlen++] = LETTERS[i].ch;
					rom[romlen] = 0;
					refresh_query();
					dirty = 1;
				}
			}
			if (sym && !abc_mode && romlen < (int)sizeof rom - 1) {
				rom[romlen++] = '-';
				rom[romlen] = 0;
				refresh_query();
				dirty = 1;
			}
			if (bs && romlen > 0) {
				rom[--romlen] = 0;
				refresh_query();
				dirty = 1;
			}
			if (shift) {
				abc_mode ^= 1;
				refresh_query();
				dirty = 1;
			}
			if (down && match_at(sel + 1, &aid)) {
				sel++;
				if (sel >= top_idx + LIST_ROWS)
					top_idx++;
				dirty = 1;
			}
			if (up && sel > lb) {
				sel--;
				if (sel < top_idx)
					top_idx--;
				dirty = 1;
			}
			if (enter && match_at(sel, &aid)) {
				hist_n = 0;
				show_loading();
				if (open_article(aid) == 0) {
					mode = M_ARTICLE;
					dirty = 1;
				} else {
					return fatal("open article", (int)cur_id);
				}
			}
			if (back)
				return -2;
			if (dirty) {
				if (mode == M_ARTICLE)
					draw_article();
				else
					draw_search();
				lcdc_copy_vram();
				dirty = 0;
			}
		} else {
			if (down && top < max_top()) { top++; dirty = 1; }
			if (up && top > 0) { top--; dirty = 1; }
			if (pgd) { top += VIS - 1; if (top > max_top()) top = max_top(); dirty = 1; }
			if (pgu) { top -= VIS - 1; if (top < 0) top = 0; dirty = 1; }
			if (right && doc_nlinks > 0) {
				cur_link = (cur_link < doc_nlinks) ? cur_link + 1 : 1;
				show_link();
				dirty = 1;
			}
			if (left && doc_nlinks > 0) {
				cur_link = (cur_link > 1) ? cur_link - 1 : doc_nlinks;
				show_link();
				dirty = 1;
			}
			if (enter && cur_link > 0 && doc_links[cur_link].id < pk.n_art) {
				uint32_t target = doc_links[cur_link].id;
				if (hist_n == HIST) {
					for (i = 1; i < HIST; i++)
						hist[i - 1] = hist[i];
					hist_n--;
				}
				hist[hist_n].id = cur_id;
				hist[hist_n].top = top;
				hist_n++;
				show_loading();
				if (open_article(target) != 0)
					return fatal("open article", (int)cur_id);
				dirty = 1;
			}
			if (back) {
				if (hist_n > 0) {
					hist_n--;
					if (open_article(hist[hist_n].id) != 0)
						return fatal("open article", (int)cur_id);
					top = hist[hist_n].top;
					if (top > max_top())
						top = max_top();
				} else {
					mode = M_SEARCH;
				}
				dirty = 1;
			}
			if (f1) {
				mode = M_SEARCH;
				dirty = 1;
			}
			if (dirty) {
				if (mode == M_ARTICLE)
					draw_article();
				else
					draw_search();
				lcdc_copy_vram();
				dirty = 0;
			}
		}
	}
}
