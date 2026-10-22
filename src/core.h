/* core.h - 描画・入出力に依存しない部分(パック読み出し/フォント/ローマ字/レイアウト)
 * 実機では main.c が plat_read を実装し、PC では test/host_test.c が実装する。 */
#ifndef CORE_H
#define CORE_H

#include <stdint.h>

/* ---- platform hook ---------------------------------------------------- */
#define KIND_IDX 0 /* WIKI.IDX */
#define KIND_DAT 1 /* WIKI.Dnn */
#define KIND_IMG 2 /* WIKI.Inn */
#define KIND_FNT 3 /* WIKI.FNT */
/* off から len バイト読む。戻り値は読めたバイト数、失敗は負 */
int plat_read(int kind, int no, uint32_t off, void *buf, uint32_t len);
/* 大きなバッファ用のメモリ確保。実機の静的RAMは約32KBしかないので、実体はヒープ(memmgr)から取る */
void *plat_alloc(uint32_t n);
/* 内部バッファを確保する。成功で 0 */
int core_init(void);

/* ---- pack ------------------------------------------------------------- */
#define KEY_MAX   40
#define TITLE_MAX 64

struct pack {
	uint32_t n_art, n_key;
	uint16_t n_d, n_i, key_size, title_size;
	uint32_t key_off, art_off, art_rec;
};

struct art {
	uint16_t dno, ino, iw, ih;
	uint32_t off, clen, rlen, ioff, ilen;
	uint8_t title[TITLE_MAX];
};

#define PACK_OK        0
#define PACK_E_READ   -1
#define PACK_E_MAGIC  -2
#define PACK_E_ENC    -3 /* cp932 以外 */
#define PACK_E_SIZE   -4

int pack_open(struct pack *p);
int pack_key(const struct pack *p, uint32_t idx, uint8_t *key, int *klen, uint32_t *aid);
uint32_t pack_lower_bound(const struct pack *p, const uint8_t *q, int qlen);
int key_has_prefix(const uint8_t *key, int klen, const uint8_t *q, int qlen);
int pack_art(const struct pack *p, uint32_t id, struct art *a);
/* 本文を g_body に展開。戻り値は長さ、失敗は負 */
int pack_body(const struct pack *p, const struct art *a);
/* 画像(JPEG)を g_jpg に読む。戻り値は長さ、無し/失敗は 0 */
int pack_image(const struct pack *p, const struct art *a);

#define MAX_BODY 65536
#define MAX_JPEG 65536
extern uint8_t *g_body; /* MAX_BODY バイト */
extern uint8_t *g_jpg;  /* MAX_JPEG バイト */

/* ---- font (WIKI.FNT) -------------------------------------------------- */
extern uint8_t g_fw, g_fh, g_hw;
int font_load(void);
extern int g_font_n, g_font_need; /* 診断用: 読めたバイト数 / 必要なバイト数 */
extern uint8_t g_font_magic[4];
const uint8_t *font_full(int row, int cell); /* row 1..84, cell 1..94 */
const uint8_t *font_half(uint8_t code);
int sjis_is_lead(uint8_t b);
int sjis_to_rc(uint8_t b0, uint8_t b1, int *row, int *cell); /* 0=ok */

/* ---- romaji ----------------------------------------------------------- */
/* rom(小文字ローマ字/'-')をひらがな(Shift_JIS)に変換。
 * out に変換済み部分、tail に「まだ確定しない末尾」(例 "k","sh","n")を返す。戻り値は out の長さ */
int romaji_to_kana(const char *rom, uint8_t *out, int cap, char *tail, int tcap);

/* ---- document (cook + wrap) ------------------------------------------- */
#define MAX_TXT    65536
#define MAX_LINKS  255
#define MAX_LINES  6000
#define LF_HEAD    1

struct dlink { uint32_t id; uint32_t start; };
struct dline { uint32_t off; uint16_t len; uint8_t flags; uint8_t pad; };

extern uint8_t *doc_txt; /* マークアップ除去後の本文(\n区切り, 見出し行は先頭 0x01) */
extern uint8_t *doc_lnk; /* バイトごとのリンク番号(0=なし) */
extern uint32_t doc_len;
extern struct dlink doc_links[MAX_LINKS + 1]; /* 1始まり */
extern int doc_nlinks;
extern uint16_t doc_link_line[MAX_LINKS + 1];
extern struct dline *doc_lines;
extern int doc_nlines;

/* g_body[0..blen) を解析して折り返す。先頭に img_rows 行の空き(画像用)を置く */
void doc_build(uint32_t blen, int img_rows, int maxw);

#endif
