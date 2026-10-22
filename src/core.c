#include <string.h>
#include "core.h"
#include "tinf.h"

/* ============================================================== helpers == */
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint8_t *cbuf;   /* 圧縮データ読み込み用 */
static uint8_t *g_font; /* WIKI.FNT 全体 */

/* ================================================================= init == */
#define FONT_CAP 262144
int core_init(void)
{
	g_body = plat_alloc(MAX_BODY);
	cbuf = plat_alloc(MAX_BODY);
	g_jpg = plat_alloc(MAX_JPEG);
	g_font = plat_alloc(FONT_CAP);
	doc_txt = plat_alloc(MAX_TXT);
	doc_lnk = plat_alloc(MAX_TXT);
	doc_lines = plat_alloc(MAX_LINES * sizeof(struct dline));
	return (g_body && cbuf && g_jpg && g_font && doc_txt && doc_lnk && doc_lines) ? 0 : -1;
}

/* ================================================================= pack == */
uint8_t *g_body;
uint8_t *g_jpg;

int pack_open(struct pack *p)
{
	uint8_t h[32];
	if (plat_read(KIND_IDX, 0, 0, h, 32) != 32)
		return PACK_E_READ;
	if (memcmp(h, "XWK1", 4) != 0 || le16(h + 4) != 1)
		return PACK_E_MAGIC;
	if (h[6] != 1) /* 0=utf-8 1=cp932 2=euc-jp。ビューアは cp932 専用 */
		return PACK_E_ENC;
	p->n_art = le32(h + 8);
	p->n_key = le32(h + 12);
	p->n_d = le16(h + 16);
	p->n_i = le16(h + 18);
	p->key_size = le16(h + 20);
	p->title_size = le16(h + 22);
	if (p->key_size > KEY_MAX || p->title_size > TITLE_MAX || p->key_size < 2 || p->title_size < 2)
		return PACK_E_SIZE;
	p->key_off = 32;
	p->art_off = p->key_off + p->n_key * (p->key_size + 4);
	p->art_rec = 28 + p->title_size;
	return PACK_OK;
}

int pack_key(const struct pack *p, uint32_t idx, uint8_t *key, int *klen, uint32_t *aid)
{
	uint8_t b[KEY_MAX + 4];
	uint32_t rec = p->key_size + 4;
	int n, i;
	if (idx >= p->n_key)
		return -1;
	if (plat_read(KIND_IDX, 0, p->key_off + idx * rec, b, rec) != (int)rec)
		return -1;
	for (n = 0; n < p->key_size && b[n]; n++)
		key[n] = b[n];
	key[n] = 0;
	*klen = n;
	*aid = le32(b + p->key_size);
	(void)i;
	return 0;
}

static int cmp_key(const uint8_t *k, int klen, const uint8_t *q, int qlen)
{
	int n = klen < qlen ? klen : qlen, i;
	for (i = 0; i < n; i++)
		if (k[i] != q[i])
			return k[i] < q[i] ? -1 : 1;
	return klen == qlen ? 0 : (klen < qlen ? -1 : 1);
}

uint32_t pack_lower_bound(const struct pack *p, const uint8_t *q, int qlen)
{
	uint32_t lo = 0, hi = p->n_key;
	uint8_t key[KEY_MAX + 1];
	int klen;
	uint32_t aid;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (pack_key(p, mid, key, &klen, &aid) < 0)
			return p->n_key;
		if (cmp_key(key, klen, q, qlen) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int key_has_prefix(const uint8_t *key, int klen, const uint8_t *q, int qlen)
{
	int i;
	if (klen < qlen)
		return 0;
	for (i = 0; i < qlen; i++)
		if (key[i] != q[i])
			return 0;
	return 1;
}

int pack_art(const struct pack *p, uint32_t id, struct art *a)
{
	uint8_t b[28 + TITLE_MAX];
	int i;
	if (id >= p->n_art)
		return -1;
	if (plat_read(KIND_IDX, 0, p->art_off + id * p->art_rec, b, p->art_rec) != (int)p->art_rec)
		return -1;
	a->dno = le16(b + 0);
	a->ino = le16(b + 2);
	a->off = le32(b + 4);
	a->clen = le32(b + 8);
	a->rlen = le32(b + 12);
	a->ioff = le32(b + 16);
	a->ilen = le32(b + 20);
	a->iw = le16(b + 24);
	a->ih = le16(b + 26);
	for (i = 0; i < p->title_size && b[28 + i]; i++)
		a->title[i] = b[28 + i];
	a->title[i] = 0;
	return 0;
}

int pack_body(const struct pack *p, const struct art *a)
{
	unsigned int dl = MAX_BODY;
	(void)p;
	if (a->clen > MAX_BODY || a->rlen > MAX_BODY)
		return -2;
	if (plat_read(KIND_DAT, a->dno, a->off, cbuf, a->clen) != (int)a->clen)
		return -1;
	if (tinf_uncompress(g_body, &dl, cbuf, a->clen) != TINF_OK)
		return -3;
	if (dl != a->rlen)
		return -4;
	return (int)dl;
}

int pack_image(const struct pack *p, const struct art *a)
{
	(void)p;
	if (!a->ilen || a->ilen > MAX_JPEG)
		return 0;
	if (plat_read(KIND_IMG, a->ino, a->ioff, g_jpg, a->ilen) != (int)a->ilen)
		return 0;
	return (int)a->ilen;
}

/* ================================================================== font == */
uint8_t g_fw = 16, g_fh = 16, g_hw = 8;
static uint32_t half_off, full_off, full_bytes, half_bytes;

int g_font_n, g_font_need;
uint8_t g_font_magic[4];

int font_load(void)
{
	int n = plat_read(KIND_FNT, 0, 0, g_font, FONT_CAP);
	g_font_n = n;
	g_font_need = 16 + 256 * 16 + 84 * 94 * 32;
	if (n >= 4)
		memcpy(g_font_magic, g_font, 4);
	if (n < 16 || memcmp(g_font, "XFN1", 4) != 0)
		return -1;
	g_fw = g_font[4];
	g_fh = g_font[5];
	g_hw = g_font[6];
	full_bytes = ((g_fw + 7) / 8) * g_fh;
	half_bytes = ((g_hw + 7) / 8) * g_fh;
	half_off = 16;
	full_off = half_off + 256 * half_bytes;
	if ((uint32_t)n < full_off + 84 * 94 * full_bytes)
		return -2;
	return 0;
}

const uint8_t *font_full(int row, int cell)
{
	return g_font + full_off + ((row - 1) * 94 + (cell - 1)) * full_bytes;
}

const uint8_t *font_half(uint8_t code)
{
	return g_font + half_off + code * half_bytes;
}

int sjis_is_lead(uint8_t b)
{
	return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

int sjis_to_rc(uint8_t c1, uint8_t c2, int *row, int *cell)
{
	int j1, j2;
	if (c2 < 0x40 || c2 == 0x7F || c2 > 0xFC)
		return -1;
	if (c1 >= 0xE0)
		c1 -= 0x40;
	if (c2 >= 0x80)
		c2 -= 1;
	if (c2 >= 0x9E) {
		j1 = (c1 - 0x70) * 2;
		j2 = c2 - 0x7D;
	} else {
		j1 = (c1 - 0x70) * 2 - 1;
		j2 = c2 - 0x1F;
	}
	*row = j1 - 0x20;
	*cell = j2 - 0x20;
	if (*row < 1 || *row > 84 || *cell < 1 || *cell > 94)
		return -1;
	return 0;
}

/* =============================================================== romaji == */
struct rom { const char *r; const char *k; };

static const struct rom ROM[] = {
	{"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
	{"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
	{"sa","さ"},{"si","し"},{"shi","し"},{"su","す"},{"se","せ"},{"so","そ"},
	{"ta","た"},{"ti","ち"},{"chi","ち"},{"tu","つ"},{"tsu","つ"},{"te","て"},{"to","と"},
	{"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
	{"ha","は"},{"hi","ひ"},{"hu","ふ"},{"fu","ふ"},{"he","へ"},{"ho","ほ"},
	{"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
	{"ya","や"},{"yu","ゆ"},{"yo","よ"},
	{"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
	{"wa","わ"},{"wi","うぃ"},{"we","うぇ"},{"wo","を"},
	{"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
	{"za","ざ"},{"zi","じ"},{"ji","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
	{"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
	{"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
	{"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
	{"kya","きゃ"},{"kyu","きゅ"},{"kyo","きょ"},
	{"sya","しゃ"},{"syu","しゅ"},{"syo","しょ"},{"sha","しゃ"},{"shu","しゅ"},{"sho","しょ"},
	{"tya","ちゃ"},{"tyu","ちゅ"},{"tyo","ちょ"},{"cha","ちゃ"},{"chu","ちゅ"},{"cho","ちょ"},
	{"cya","ちゃ"},{"cyu","ちゅ"},{"cyo","ちょ"},
	{"nya","にゃ"},{"nyu","にゅ"},{"nyo","にょ"},
	{"hya","ひゃ"},{"hyu","ひゅ"},{"hyo","ひょ"},
	{"mya","みゃ"},{"myu","みゅ"},{"myo","みょ"},
	{"rya","りゃ"},{"ryu","りゅ"},{"ryo","りょ"},
	{"gya","ぎゃ"},{"gyu","ぎゅ"},{"gyo","ぎょ"},
	{"ja","じゃ"},{"ju","じゅ"},{"jo","じょ"},{"jya","じゃ"},{"jyu","じゅ"},{"jyo","じょ"},
	{"zya","じゃ"},{"zyu","じゅ"},{"zyo","じょ"},
	{"bya","びゃ"},{"byu","びゅ"},{"byo","びょ"},
	{"pya","ぴゃ"},{"pyu","ぴゅ"},{"pyo","ぴょ"},
	{"fa","ふぁ"},{"fi","ふぃ"},{"fe","ふぇ"},{"fo","ふぉ"},
	{"xa","ぁ"},{"xi","ぃ"},{"xu","ぅ"},{"xe","ぇ"},{"xo","ぉ"},
	{"la","ぁ"},{"li","ぃ"},{"lu","ぅ"},{"le","ぇ"},{"lo","ぉ"},
	{"xya","ゃ"},{"xyu","ゅ"},{"xyo","ょ"},{"xtu","っ"},{"xtsu","っ"},{"ltu","っ"},
	{0, 0}
};

/* UTF-8 のひらがな1文字(3バイト)を Shift_JIS 2バイトにする。長音(ー)も扱う */
static int u8_to_sjis(const uint8_t *s, uint8_t *out)
{
	uint32_t cp;
	if (s[0] != 0xE3)
		return 0;
	cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
	if (cp >= 0x3041 && cp <= 0x3093) {
		uint32_t v = 0x829F + (cp - 0x3041);
		out[0] = v >> 8;
		out[1] = v & 0xFF;
		return 3;
	}
	return 0;
}

static int emit_kana(const char *k, uint8_t *out, int o, int cap)
{
	const uint8_t *s = (const uint8_t *)k;
	while (*s && o + 2 <= cap) {
		int n = u8_to_sjis(s, out + o);
		if (!n)
			break;
		o += 2;
		s += n;
	}
	return o;
}

static int is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

int romaji_to_kana(const char *rom, uint8_t *out, int cap, char *tail, int tcap)
{
	int i = 0, o = 0, n = (int)strlen(rom);
	tail[0] = 0;
	while (i < n && o + 2 <= cap) {
		char c = rom[i];
		int len, j, hit = 0, k;
		if (c == '-') {
			out[o++] = 0x81;
			out[o++] = 0x5B; /* ー */
			i++;
			continue;
		}
		if (i + 1 < n && rom[i + 1] == c && c != 'n' && !is_vowel(c)) {
			o = emit_kana("っ", out, o, cap);
			i++;
			continue;
		}
		if (c == 'n') {
			if (i + 1 < n && rom[i + 1] == 'n') {
				/* "nn": 後ろが母音/y なら 1個目だけ「ん」("konnichiha"→こんにちは)、
				 * それ以外("kann" "nnk")は2個で「ん」 */
				o = emit_kana("ん", out, o, cap);
				if (i + 2 < n && (is_vowel(rom[i + 2]) || rom[i + 2] == 'y'))
					i += 1;
				else
					i += 2;
				continue;
			}
			if (i + 1 >= n) {
				tail[0] = 'n';
				tail[1] = 0;
				break;
			}
			if (!is_vowel(rom[i + 1]) && rom[i + 1] != 'y') {
				o = emit_kana("ん", out, o, cap);
				i++;
				continue;
			}
		}
		for (len = 4; len >= 1 && !hit; len--) {
			if (i + len > n)
				continue;
			for (j = 0; ROM[j].r; j++) {
				if ((int)strlen(ROM[j].r) == len && memcmp(ROM[j].r, rom + i, len) == 0) {
					o = emit_kana(ROM[j].k, out, o, cap);
					i += len;
					hit = 1;
					break;
				}
			}
		}
		if (hit)
			continue;
		/* 未確定の末尾(後続の入力で確定しうる)か、無効な文字か */
		{
			int rest = n - i, pre = 0;
			for (j = 0; ROM[j].r && !pre; j++)
				if ((int)strlen(ROM[j].r) > rest && memcmp(ROM[j].r, rom + i, rest) == 0)
					pre = 1;
			if (pre && rest < tcap) {
				for (k = 0; k < rest; k++)
					tail[k] = rom[i + k];
				tail[rest] = 0;
				break;
			}
			i++; /* 無効文字は捨てる */
		}
	}
	return o;
}

/* ============================================================== document == */
uint8_t *doc_txt;
uint8_t *doc_lnk;
uint32_t doc_len;
struct dlink doc_links[MAX_LINKS + 1];
int doc_nlinks;
uint16_t doc_link_line[MAX_LINKS + 1];
struct dline *doc_lines;
int doc_nlines;

static void push_line(uint32_t off, uint32_t len, uint8_t flags)
{
	if (doc_nlines >= MAX_LINES)
		return;
	doc_lines[doc_nlines].off = off;
	doc_lines[doc_nlines].len = (uint16_t)len;
	doc_lines[doc_nlines].flags = flags;
	doc_lines[doc_nlines].pad = 0;
	doc_nlines++;
}

void doc_build(uint32_t blen, int img_rows, int maxw)
{
	uint32_t i = 0, n = 0;
	int cur = 0, li, k;

	/* ---- cook: マークアップを外し、リンク範囲を記録 ---- */
	doc_nlinks = 0;
	while (i < blen && n < MAX_TXT - 1) {
		uint8_t c = g_body[i];
		if (c == 0x01) { /* 見出し: 0x01 + レベル数字 */
			doc_txt[n] = 0x01;
			doc_lnk[n++] = 0;
			i += 2;
		} else if (c == 0x02) { /* リンク開始: 0x02 <10進ID> 0x03 */
			uint32_t id = 0;
			i++;
			while (i < blen && g_body[i] != 0x03) {
				if (g_body[i] >= '0' && g_body[i] <= '9')
					id = id * 10 + (g_body[i] - '0');
				i++;
			}
			i++;
			if (doc_nlinks < MAX_LINKS) {
				doc_nlinks++;
				doc_links[doc_nlinks].id = id;
				doc_links[doc_nlinks].start = n;
				cur = doc_nlinks;
			}
		} else if (c == 0x04) {
			cur = 0;
			i++;
		} else {
			doc_txt[n] = c;
			doc_lnk[n++] = (uint8_t)cur;
			i++;
		}
	}
	doc_len = n;

	/* ---- wrap ---- */
	doc_nlines = 0;
	for (k = 0; k < img_rows; k++)
		push_line(0, 0, 0);
	i = 0;
	while (i < n) {
		uint32_t e = i, s;
		uint8_t flags = 0;
		int x = 0;
		while (e < n && doc_txt[e] != '\n')
			e++;
		s = i;
		if (s < e && doc_txt[s] == 0x01) {
			flags = LF_HEAD;
			s++;
		}
		if (s >= e) {
			push_line(s, 0, flags);
		} else {
			uint32_t start = s;
			while (s < e) {
				int step = 1, w = g_hw;
				if (sjis_is_lead(doc_txt[s]) && s + 1 < e) {
					step = 2;
					w = g_fw;
				}
				if (x + w > maxw && s > start) {
					push_line(start, s - start, flags);
					start = s;
					x = 0;
				}
				x += w;
				s += step;
			}
			push_line(start, s - start, flags);
		}
		i = e + 1;
	}

	/* ---- link -> 行番号 ---- */
	for (k = 1; k <= doc_nlinks; k++) {
		doc_link_line[k] = 0;
		for (li = 0; li < doc_nlines; li++) {
			if (doc_links[k].start >= doc_lines[li].off &&
			    doc_links[k].start < doc_lines[li].off + doc_lines[li].len) {
				doc_link_line[k] = (uint16_t)li;
				break;
			}
		}
	}
}
