#ifndef IMG_H
#define IMG_H
#include <stdint.h>
/* JPEG(baseline)を RGB565 にデコードして out(cap_w*cap_h)に詰める。
 * 成功で 0。*w,*h に実サイズ(行ピッチは *w)。 */
int img_decode(const uint8_t *src, uint32_t len, uint16_t *out, int cap_w, int cap_h, int *w, int *h);
#endif
