/*
 * Which pin is MISO?
 *
 * The dts muxes gpio8/9/10 to the qup4 function but never says which of them
 * carries CLK, MOSI and MISO, and the GENI engine's pin assignment is fixed in
 * silicon. Bit-banging is the only way to try the alternatives: drive CLK and
 * MOSI ourselves and sample each candidate as an input.
 *
 * For every mapping: hold reset low, send the vendor's engine reset over the
 * bit-banged bus, release reset high, wait 10 ms, then read CHIP_VER_TRIM
 * (0x3f004). A live controller answers 0E 00 00 72 66 03 - 0x036672, NT36675.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL
#define TILE_SOUTH	0x00500000UL

#define PIN_CS		11
#define PIN_RST		38

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CS		CFG(PIN_CS, TILE_NORTH)
#define C_RST		CFG(PIN_RST, TILE_SOUTH)

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
	v |= (0u << 2) | (6u << 6) | 0x200u;	/* gpio, 6 mA, output */
	wr(cfg, v);
}

static int pin_in(uintptr_t cfg)
{
	uint32_t v = rd(cfg);

	v &= ~0x3ffu;
	v |= (0u << 2) | 3u | (6u << 6);		/* gpio, pull-up, input */
	wr(cfg, v);
	return (int)(rd(cfg + 4) & 1u);
}

/* current mapping */
static uintptr_t c_clk, c_mosi, c_miso;

static void bb_setup(unsigned clk_pin, unsigned mosi_pin, unsigned miso_pin)
{
	c_clk = CFG(clk_pin, TILE_NORTH);
	c_mosi = CFG(mosi_pin, TILE_NORTH);
	c_miso = CFG(miso_pin, TILE_NORTH);
	pin_out(c_clk, 0);
	pin_out(c_mosi, 0);
	pin_out(C_CS, 1);
	(void)pin_in(c_miso);
}

static uint8_t bb_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		pin_out(c_mosi, (out >> i) & 1);
		delay_us(1);
		pin_out(c_clk, 1);
		delay_us(1);
		in = (uint8_t)((in << 1) | (uint8_t)pin_in(c_miso));
		pin_out(c_clk, 0);
		delay_us(1);
	}
	return in;
}

static void bb_cs(int sel)
{
	pin_out(C_CS, sel ? 0 : 1);
	delay_us(2);
}

static void bb_write(uint32_t addr, uint8_t val)
{
	bb_cs(1);
	(void)bb_xfer(0xffu);
	(void)bb_xfer((uint8_t)((addr >> 15) & 0xffu));
	(void)bb_xfer((uint8_t)((addr >> 7) & 0xffu));
	bb_cs(0);
	delay_us(2);
	bb_cs(1);
	(void)bb_xfer((uint8_t)((addr & 0x7fu) | 0x80u));
	(void)bb_xfer(val);
	bb_cs(0);
	delay_us(2);
}

static void bb_read(uint32_t addr, uint8_t *buf, unsigned n)
{
	bb_cs(1);
	(void)bb_xfer(0xffu);
	(void)bb_xfer((uint8_t)((addr >> 15) & 0xffu));
	(void)bb_xfer((uint8_t)((addr >> 7) & 0xffu));
	bb_cs(0);
	delay_us(2);
	bb_cs(1);
	(void)bb_xfer((uint8_t)(addr & 0x7fu));
	(void)bb_xfer(0u);
	for (unsigned i = 0; i < n; i++) {
		buf[i] = bb_xfer(0u);
	}
	bb_cs(0);
	delay_us(2);
}

static void try_map(const char *tag, unsigned clk, unsigned mosi, unsigned miso)
{
	uint8_t b[8];

	con_str(tag, "reset low, eng_reset, reset high");
	/* reset asserted (low) while the engine reset goes out */
	{
		uint32_t v = rd(C_RST + 4);

		wr(C_RST + 4, v & ~2u);
		v = rd(C_RST);
		v &= ~0x3ffu;
		v |= (0u << 2) | (8u << 6) | 0x200u;
		wr(C_RST, v);
	}
	delay_us(2000);
	bb_setup(clk, mosi, miso);
	bb_write(0x7FFF80u, 0x5Au);
	{
		uint32_t v = rd(C_RST + 4);

		wr(C_RST + 4, (v & ~2u) | 2u);	/* release high */
	}
	delay_us(15000);

	bb_read(0x3F004u, b, 8);
	con_hex2(tag, "a", ((uint32_t)b[0] << 8) | b[1], "b", ((uint32_t)b[2] << 8) | b[3]);
	con_hex2(tag, "c", ((uint32_t)b[4] << 8) | b[5], "d", ((uint32_t)b[6] << 8) | b[7]);
}

void test_touch(void)
{
	con_str("BB", "which pin is MISO?");
	con_hex("rpmh", (uint32_t)rpmh_ldo11c_on(3000));

	try_map("MAP clk8 mo9 mi10", 8, 9, 10);
	try_map("MAP clk8 mo10 mi9", 8, 10, 9);
	try_map("MAP clk9 mo8 mi10", 9, 8, 10);
	try_map("MAP clk10 mo9 mi8", 10, 9, 8);

	con_str("BB", "done, halted");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
