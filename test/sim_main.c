/* PC 上で src/main.c を動かすシミュレータ。画面を /tmp/frames/fNN.ppm に書き出す。
 * 使い方: sim <packdir> <outdir> <keyscript>   keyscript は "neko,ENTER,DOWN,RIGHT,ENTER,BACK" のような列 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sh4a/input/exword_keys.h>

static const char *packdir, *sddir, *outdir;
static uint16_t vram[528 * 320];
static int frame;

void *lcdc_get_vram_address(void) { return vram; }
void lcdc_copy_vram(void)
{
	char fn[512];
	FILE *f;
	int i;
	snprintf(fn, sizeof fn, "%s/f%02d.ppm", outdir, frame++);
	f = fopen(fn, "wb");
	fprintf(f, "P6\n528 320\n255\n");
	for (i = 0; i < 528 * 320; i++) {
		uint16_t c = vram[i];
		fputc(((c >> 11) & 31) << 3, f);
		fputc(((c >> 5) & 63) << 2, f);
		fputc((c & 31) << 3, f);
	}
	fclose(f);
}
void set_pen(uint16_t c) { (void)c; }
void render_text(uint16_t x, uint16_t y, char *t) { printf("TEXT(%d,%d): %s\n", x, y, t); }
void memmgr_init(void) {}
void *memmgr_alloc(unsigned long n) { return malloc(n); }

/* ---- files: \\drv0\ は packdir(内蔵)、\\crd0\ は sddir(SD) に対応させる。
 * アプリ用フォルダ(<ID>\\_USER\\ など)は空として扱う ---- */
#include <dirent.h>
#include <fnmatch.h>
static FILE *files[64];
void sys_dict_info(unsigned long *drv, char *id) { *drv = 0; strcpy(id, "XWIKI"); }

/* "\\\\crd0\\foo" -> sd の foo, "\\\\drv0\\foo" -> 内蔵の foo。サブフォルダ付きは NULL */
static int map_path(const char *p, char *out, size_t n)
{
	const char *dir = packdir;
	const char *rest = p;
	int slashes = 0;
	const char *q;
	if (!strncmp(p, "\\\\crd0\\", 7)) { dir = sddir; rest = p + 7; }
	else if (!strncmp(p, "\\\\drv0\\", 7)) { rest = p + 7; }
	if (!dir) return -1;
	for (q = rest; *q; q++) if (*q == '\\') slashes++;
	if (slashes) return -1;
	snprintf(out, n, "%s/%s", dir, rest);
	return 0;
}

int sys_open(const char *p, int m)
{
	char fn[512];
	int i;
	(void)m;
	if (map_path(p, fn, sizeof fn) < 0) return -1;
	for (i = 0; i < 64; i++)
		if (!files[i]) { files[i] = fopen(fn, "rb"); return files[i] ? i : -1; }
	return -1;
}
int sys_close(int fd) { if (files[fd]) fclose(files[fd]); files[fd] = 0; return 0; }
int sys_closeall(void) { int i; for (i = 0; i < 64; i++) sys_close(i); return 0; }
int sys_seek(int fd, int pos, int w) { (void)w; return fseek(files[fd], pos, SEEK_SET); }
int sys_read(int fd, void *buf, int len) { return (int)fread(buf, 1, len, files[fd]); }

/* sys_findfirst: 大文字小文字を区別するマスク照合(実機が区別するかは未確認なので、区別する前提で試す) */
static DIR *fdir[8];
static char fmask[8][64];
int sys_findfirst(const char *mask, int *h, char *name, unsigned long *type)
{
	char dirpath[512], tmp[512];
	const char *base = strrchr(mask, '\\');
	int i;
	size_t dn = base ? (size_t)(base - mask + 1) : 0;
	char dm[64];
	memcpy(dm, mask, dn); dm[dn] = 0;
	snprintf(tmp, sizeof tmp, "%s%s", dm, "x");
	if (map_path(tmp, dirpath, sizeof dirpath) < 0) return -1;
	*strrchr(dirpath, '/') = 0;
	for (i = 0; i < 8; i++) if (!fdir[i]) break;
	if (i == 8) return -1;
	fdir[i] = opendir(dirpath);
	if (!fdir[i]) return -1;
	strcpy(fmask[i], base ? base + 1 : mask);
	*h = i;
	return sys_findnext(i, name, type);
}
int sys_findnext(int h, char *name, unsigned long *type)
{
	struct dirent *e;
	while ((e = readdir(fdir[h]))) {
		if (e->d_type == DT_DIR) continue;
		if (fnmatch(fmask[h], e->d_name, 0) == 0) {
			strcpy(name, e->d_name);
			*type = 1;
			return 0;
		}
	}
	return -1;
}
int sys_findclose(int h) { if (fdir[h]) closedir(fdir[h]); fdir[h] = 0; return 0; }

/* ---- keys ---- */
static const struct { const char *n; int code; } NAMES[] = {
	{"ENTER", KEY_ENTER}, {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
	{"BACK", KEY_BACK}, {"BS", KEY_BACKSPACE}, {"PGDN", KEY_PAGE_DOWN}, {"PGUP", KEY_PAGE_UP},
	{"SHIFT", KEY_SHIFT}, {"SYM", KEY_SYMBOL}, {"F1", KEY_FUNC_1}, {0, 0}
};
static const int LET[26] = {KEY_CHAR_A, KEY_CHAR_B, KEY_CHAR_C, KEY_CHAR_D, KEY_CHAR_E, KEY_CHAR_F, KEY_CHAR_G,
	KEY_CHAR_H, KEY_CHAR_I, KEY_CHAR_J, KEY_CHAR_K, KEY_CHAR_L, KEY_CHAR_M, KEY_CHAR_N, KEY_CHAR_O, KEY_CHAR_P,
	KEY_CHAR_Q, KEY_CHAR_R, KEY_CHAR_S, KEY_CHAR_T, KEY_CHAR_U, KEY_CHAR_V, KEY_CHAR_W, KEY_CHAR_X, KEY_CHAR_Y, KEY_CHAR_Z};
static int script[512], nscript, cur = -1, idle;

static void parse(char *s)
{
	char *t = strtok(s, ",");
	while (t) {
		int i, done = 0;
		for (i = 0; NAMES[i].n; i++)
			if (!strcmp(t, NAMES[i].n)) { script[nscript++] = NAMES[i].code; done = 1; }
		if (!done) { /* 英字列は1文字ずつ入力 */
			for (i = 0; t[i]; i++)
				script[nscript++] = t[i] == '-' ? KEY_SYMBOL : LET[t[i] - 'a'];
		}
		t = strtok(0, ",");
	}
}

void keypad_read(void)
{
	/* 各ループで 1 キー。キーの間に1回「何も押さない」ループを挟む(描画を促す) */
	if (idle) { cur = -1; idle = 0; return; }
	if (++cur >= 0 && cur >= 0) {}
	static int pos;
	if (pos >= nscript) { cur = -1; static int tail; if (++tail > 2) exit(0); return; }
	cur = script[pos++];
	idle = 1;
}
uint16_t get_key_pressed(uint8_t code) { return cur == code; }

int main(int argc, char **argv);
int real_main(void);
int main(int argc, char **argv)
{
	static char buf[4096];
	packdir = argv[1];
	outdir = argv[2];
	sddir = argc > 4 && strcmp(argv[4], "-") ? argv[4] : 0;
	if (!strcmp(packdir, "-")) packdir = 0;
	strncpy(buf, argv[3], sizeof buf - 1);
	parse(buf);
	return real_main();
}
