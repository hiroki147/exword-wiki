#include <string.h>
#include "img.h"
#include "picojpeg.h"

static const uint8_t *jp_src;
static uint32_t jp_pos, jp_len;

static unsigned char need_bytes(unsigned char *buf, unsigned char size, unsigned char *got, void *cb)
{
	uint32_t n = jp_len - jp_pos;
	(void)cb;
	if (n > size)
		n = size;
	memcpy(buf, jp_src + jp_pos, n);
	jp_pos += n;
	*got = (unsigned char)n;
	return 0;
}

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

int img_decode(const uint8_t *src, uint32_t len, uint16_t *out, int cap_w, int cap_h, int *w, int *h)
{
	pjpeg_image_info_t info;
	int mcu_x = 0, mcu_y = 0;
	jp_src = src;
	jp_pos = 0;
	jp_len = len;
	if (pjpeg_decode_init(&info, need_bytes, 0, 0) != 0)
		return -1;
	if (info.m_width > cap_w || info.m_height > cap_h)
		return -2;
	*w = info.m_width;
	*h = info.m_height;
	for (;;) {
		int y, x, st = pjpeg_decode_mcu();
		if (st == PJPG_NO_MORE_BLOCKS)
			break;
		if (st)
			return -3;
		if (mcu_y >= info.m_MCUSPerCol)
			return -4;
		for (y = 0; y < info.m_MCUHeight; y += 8) {
			for (x = 0; x < info.m_MCUWidth; x += 8) {
				uint32_t ofs = (x * 8U) + (y * 16U);
				int by, bx;
				for (by = 0; by < 8; by++) {
					int py = mcu_y * info.m_MCUHeight + y + by;
					if (py >= info.m_height)
						break;
					for (bx = 0; bx < 8; bx++) {
						int px = mcu_x * info.m_MCUWidth + x + bx;
						uint32_t so = ofs + bx + by * 8;
						uint8_t r, g, b;
						if (px >= info.m_width)
							break;
						r = info.m_pMCUBufR[so];
						if (info.m_scanType == PJPG_GRAYSCALE) {
							g = b = r;
						} else {
							g = info.m_pMCUBufG[so];
							b = info.m_pMCUBufB[so];
						}
						out[py * info.m_width + px] = RGB565(r, g, b);
					}
				}
			}
		}
		if (++mcu_x == info.m_MCUSPerRow) {
			mcu_x = 0;
			mcu_y++;
		}
	}
	return 0;
}
