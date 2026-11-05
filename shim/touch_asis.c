/*
 * Read the controller's state exactly as the bootloader left it.
 *
 * No reset, no engine reset, no page writes beyond the read itself: just map the
 * pins as they already are, read the chip id and the firmware handshake byte at
 * 0x22d51. If the handshake reads 0xa0-0xaf here, the firmware is running when
 * Android starts and something we do later is what stops it.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL

#define PIN_CS		11

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CS		CFG(PIN_CS, TILE_NORTH)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t t;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 1920ull / 100u;

	while (ticks() < end)
		;
}

static void pin_out(uintptr_t cfg, int level)
{
	uint32_t v = rd(cfg + 4);

	wr(cfg + 4, (v & ~2u) | (level ? 2u : 0u));
	v = rd(cfg);
	v &= ~0x3ffu;
	v |= (6u << 6) | 0x200u;
	wr(cfg, v);
}

static int pin_in(uintptr_t cfg)
{
	uint32_t v = rd(cfg);

	v &= ~0x3ffu;
	v |= 3u | (6u << 6);
	wr(cfg, v);
	return (int)(rd(cfg + 4) & 1u);
}

static void bb_xfer(uint8_t out, uint8_t *in)
{
	uint8_t r = 0;

	for (int i = 7; i >= 0; i--) {
		pin_out(CFG(9, TILE_NORTH), (out >> i) & 1);
		delay_us(1);
		pin_out(CFG(10, TILE_NORTH), 1);
		delay_us(1);
		r = (uint8_t)((r << 1) | (uint8_t)pin_in(CFG(8, TILE_NORTH)));
		pin_out(CFG(10, TILE_NORTH), 0);
		delay_us(1);
	}
	if (in) {
		*in = r;
	}
}

static void bb_cs(int sel)
{
	pin_out(C_CS, !sel);
	delay_us(2);
}

static void bb_read(uint32_t addr, uint8_t *buf, unsigned n)
{
	uint8_t junk;

	bb_cs(1);
	bb_xfer(0xffu, 0);
	bb_xfer((uint8_t)((addr >> 15) & 0xffu), 0);
	bb_xfer((uint8_t)((addr >> 7) & 0xffu), 0);
	bb_cs(0);
	delay_us(2);
	bb_cs(1);
	bb_xfer((uint8_t)(addr & 0x7fu), 0);
	bb_xfer(0u, 0);
	for (unsigned i = 0; i < n; i++) {
		bb_xfer(0u, &buf[i]);
	}
	bb_cs(0);
	delay_us(2);
	(void)junk;
}

void test_touch(void)
{
	uint8_t id[8], hs[2];

	con_str("ASIS", "state as the bootloader left it");
	(void)rpmh_ldo11c_on(3000);

	/* Only the four SPI pins: leave reset and irq untouched. */
	pin_out(CFG(10, TILE_NORTH), 0);
	pin_out(CFG(9, TILE_NORTH), 0);
	pin_out(C_CS, 1);
	(void)pin_in(CFG(8, TILE_NORTH));

	bb_read(0x3f004u, id, sizeof(id));
	con_hex2("id", "a", ((uint32_t)id[0] << 8) | id[1], "b",
	    ((uint32_t)id[2] << 8) | id[3]);
	con_hex2("id", "c", ((uint32_t)id[4] << 8) | id[5], "d",
	    ((uint32_t)id[6] << 8) | id[7]);

	bb_read(0x22d51u, hs, 2);
	con_hex("handshake", hs[0]);
	bb_read(0x22d60u, hs, 2);
	con_hex("reset state", hs[0]);
	bb_read(0x22d78u, id, 8);
	con_hex2("fwinfo", "a", ((uint32_t)id[0] << 8) | id[1], "b",
	    ((uint32_t)id[2] << 8) | id[3]);

	con_str("ASIS", "done, halted");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
