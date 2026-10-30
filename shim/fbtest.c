/*
 * Framebuffer probe: paint the panel memory with three colour bands and wait.
 *
 * The kernel claims it copies 60 compositor frames per second into
 * 0x9c000000 and reports zero skips, yet the panel keeps showing the boot
 * logo. Either the panel scans a different buffer or something overlays it.
 * Filling the buffer with unmistakable colours tells the two apart: if the
 * screen turns red/green/blue the scanout is right, if it stays on the logo
 * the display controller is pointed elsewhere.
 */
#include <stdint.h>
#include "console.h"
#include "fbtest.h"

#define FB_BASE_PA	0x9c000000UL
#define FB_W		1080u
#define FB_H		2400u
#define FB_STRIDE	4320u

static void fill(uint32_t y0, uint32_t y1, uint32_t colour)
{
	volatile uint32_t *base = (volatile uint32_t *)FB_BASE_PA;

	for (uint32_t y = y0; y < y1; y++) {
		volatile uint32_t *row = base + (uint64_t)y * (FB_STRIDE / 4);
		for (uint32_t x = 0; x < FB_W; x++)
			row[x] = colour;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

void fbtest(void)
{
	/* Leave a readable trace of the *old* content before overwriting it. */
	volatile uint32_t *base = (volatile uint32_t *)FB_BASE_PA;
	uint32_t p0 = base[0];
	uint32_t p1 = base[(FB_STRIDE / 4) * 1200 + 540];

	fill(0, 800, 0xffff0000u);
	fill(800, 1600, 0xff00ff00u);
	fill(1600, 2400, 0xff0000ffu);

	con_clear(0xff101010u);
	con_str("FB TEST", "bands on 0x9c000000: red/green/blue");
	con_hex("old[0]", p0);
	con_hex("old[mid]", p1);

	/* Hold the pattern long enough to look at, then come back to fastboot. */
	for (volatile uint64_t i = 0; i < 600000000ULL; i++)
		;

	*(volatile uint32_t *)0x146bf65cUL = 0x77665500U;
	__asm__ volatile("dsb sy" ::: "memory");
	{
		register uint64_t x0 __asm__("x0") = 0xc4000012UL;
		register uint64_t x1 __asm__("x1") = 0;
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1) :: "memory");
	}
	for (;;)
		__asm__ volatile("wfe");
}
