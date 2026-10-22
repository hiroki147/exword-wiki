/* PC 上で core.c / img.c を検証するテスト。使い方: host_test <packdir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core.h"
#include "../src/img.h"

static const char *dir;
void *plat_alloc(uint32_t n) { return malloc(n); }
int plat_read(int kind, int no, uint32_t off, void *buf, uint32_t len)
{
	char path[512];
	FILE *f;
	size_t n;
	switch (kind) {
	case KIND_IDX: snprintf(path, sizeof path, "%s/WIKI.IDX", dir); break;
	case KIND_DAT: snprintf(path, sizeof path, "%s/WIKI.D%02d", dir, no); break;
	case KIND_IMG: snprintf(path, sizeof path, "%s/WIKI.I%02d", dir, no); break;
	default: snprintf(path, sizeof path, "%s/WIKI.FNT", dir); break;
	}
	f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, off, SEEK_SET);
	n = fread(buf, 1, len, f);
	fclose(f);
	return (int)n;
}

static void hex(const char *tag, const uint8_t *s, int n)
{
	int i;
	printf("%s ", tag);
	for (i = 0; i < n; i++) printf("%02x", s[i]);
	printf("\n");
}

static uint16_t imgbuf[240 * 160];

int main(int argc, char **argv)
{
	struct pack p;
	struct art a;
	uint8_t q[64], key[KEY_MAX + 1];
	char tail[8];
	int qn, klen, i, rc;
	uint32_t lb, aid;
	const char *tests[] = {"neko", "konnichiha", "kyouto", "hokkaido", "sapporo", "nn", "kanna", "gakkou", "ra-men", "sh", "tsu", "xtu", 0};
	dir = argv[1];

	core_init();
	rc = pack_open(&p);
	printf("OPEN %d n_art=%u n_key=%u\n", rc, p.n_art, p.n_key);
	rc = font_load();
	printf("FONT %d fw=%d fh=%d hw=%d\n", rc, g_fw, g_fh, g_hw);

	for (i = 0; tests[i]; i++) {
		qn = romaji_to_kana(tests[i], q, sizeof q, tail, sizeof tail);
		printf("ROM %s ", tests[i]);
		hex("kana", q, qn);
		printf("TAIL %s|\n", tail);
	}

	/* 全キー一覧 */
	for (i = 0; i < (int)p.n_key; i++) {
		pack_key(&p, i, key, &klen, &aid);
		pack_art(&p, aid, &a);
		printf("KEYIDX %d aid=%u ", i, aid);
		hex("key", key, klen);
		hex("title", a.title, (int)strlen((char *)a.title));
	}

	/* 検索: "ね" -> ネコ */
	qn = romaji_to_kana("ne", q, sizeof q, tail, sizeof tail);
	lb = pack_lower_bound(&p, q, qn);
	pack_key(&p, lb, key, &klen, &aid);
	printf("SEARCH ne lb=%u hit=%d\n", lb, key_has_prefix(key, klen, q, qn));
	pack_art(&p, aid, &a);
	hex("hit_title", a.title, (int)strlen((char *)a.title));

	/* 記事を開く */
	rc = pack_body(&p, &a);
	printf("BODY rc=%d rlen=%u\n", rc, a.rlen);
	{
		int imgrows = 0, w = 0, h = 0, il = pack_image(&p, &a);
		if (il) {
			rc = img_decode(g_jpg, il, imgbuf, 240, 160, &w, &h);
			printf("IMG rc=%d %dx%d jpeglen=%d\n", rc, w, h, il);
			if (rc == 0) {
				FILE *f = fopen("/tmp/wk/decoded.ppm", "wb");
				int y, x;
				fprintf(f, "P6\n%d %d\n255\n", w, h);
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++) {
						uint16_t c = imgbuf[y * w + x];
						fputc(((c >> 11) & 31) << 3, f);
						fputc(((c >> 5) & 63) << 2, f);
						fputc((c & 31) << 3, f);
					}
				fclose(f);
			}
			imgrows = (h + 17) / 18 + 1;
		}
		doc_build(rc >= 0 ? a.rlen : 0, imgrows, 512);
		printf("DOC lines=%d links=%d imgrows=%d\n", doc_nlines, doc_nlinks, imgrows);
		for (i = 1; i <= doc_nlinks; i++)
			printf("LINK %d id=%u start=%u line=%u\n", i, doc_links[i].id, doc_links[i].start, doc_link_line[i]);
		for (i = 0; i < doc_nlines; i++) {
			char tag[32];
			snprintf(tag, sizeof tag, "LINE %d f%d", i, doc_lines[i].flags);
			hex(tag, doc_txt + doc_lines[i].off, doc_lines[i].len);
		}
	}
	return 0;
}
