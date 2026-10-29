/*
 * Novatek NT36675 probe on the phone: bit-banged SPI driving the verified
 * protocol layer in nvt.c.
 *
 * The touch controller sits on GENI SE4 (0x990000), but GENI needs the GCC
 * clock tree that XNU does not have. The V60 port hit the same wall and fell
 * back to bit-banging, so we do the same: the QUP pins become plain GPIOs and
 * nvt.c clocks the vendor protocol by hand.
 *
 * TLMM: 0x0F000000 + tile (NORTH 0x00900000, SOUTH 0x00500000) + 0x1000 * pin
 *       ctl: mux[5:2], pull[1:0], drive[8:6], OE bit9
 *       io : in bit0, out bit1
 * Pins: CLK GPIO8, MOSI GPIO9, MISO GPIO10, CS GPIO11 (NORTH),
 *       RESET GPIO38, IRQ GPIO39 (SOUTH).
 */
#include <stdint.h>
#include "console.h"
#include "nvt.h"

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL
#define TILE_SOUTH	0x00500000UL

#define PIN_CLK		8
#define PIN_MOSI	9
#define PIN_MISO	10
#define PIN_CS		11
#define PIN_RST		38
#define PIN_IRQ		39

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CLK		CFG(PIN_CLK, TILE_NORTH)
#define C_MOSI		CFG(PIN_MOSI, TILE_NORTH)
#define C_MISO		CFG(PIN_MISO, TILE_NORTH)
#define C_CS		CFG(PIN_CS, TILE_NORTH)
#define C_RST		CFG(PIN_RST, TILE_SOUTH)
#define C_IRQ		CFG(PIN_IRQ, TILE_SOUTH)

#define PULL_NO		0
#define PULL_DOWN	1
#define PULL_UP		3

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
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

static void gpio_out(uintptr_t b)
{
	uint32_t v = rd(b);
	v &= ~(0xfu << 2);
	v |= (1u << 9);
	wr(b, v);
}

static void gpio_in(uintptr_t b, uint32_t pull)
{
	uint32_t v = rd(b);
	v &= ~(0xfu << 2);
	v &= ~(1u << 9);
	v &= ~3u;
	v |= pull & 3u;
	wr(b, v);
}

static void gpio_set(uintptr_t b, int v)
{
	uint32_t x = rd(b + 4);
	if (v)
		x |= 2u;
	else
		x &= ~2u;
	wr(b + 4, x);
}

static int gpio_get(uintptr_t b) { return (int)(rd(b + 4) & 1u); }

void nvt_hw_cs(int assert)
{
	gpio_set(C_CS, assert ? 0 : 1);	/* CS is active low */
	delay_us(2);
}

uint8_t nvt_hw_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		gpio_set(C_MOSI, (out >> i) & 1);
		delay_us(2);
		gpio_set(C_CLK, 1);
		delay_us(2);
		in = (uint8_t)(in << 1) | (uint8_t)gpio_get(C_MISO);
		gpio_set(C_CLK, 0);
		delay_us(2);
	}
	return in;
}

static void dump(const char *tag, const uint8_t *b, unsigned n)
{
	char line[3 * 16 + 1];
	static const char hx[] = "0123456789ABCDEF";
	unsigned c = 0;

	if (n > 16)
		n = 16;
	for (unsigned i = 0; i < n; i++) {
		line[c++] = hx[b[i] >> 4];
		line[c++] = hx[b[i] & 0xf];
		line[c++] = ' ';
	}
	line[c] = 0;
	con_str(tag, line);
}

void test_touch(void)
{
	struct nvt_fw_info info;
	uint8_t b[16];

	con_str("TP", "novatek NT36675 probe v6 (nvt.c protocol)");

	gpio_out(C_CLK);
	gpio_out(C_MOSI);
	gpio_out(C_CS);
	gpio_out(C_RST);
	gpio_in(C_MISO, PULL_NO);
	gpio_in(C_IRQ, PULL_NO);
	gpio_set(C_CS, 1);
	gpio_set(C_CLK, 0);
	gpio_set(C_MOSI, 0);

	con_hex2("TP tlmm", "clk=", (uint32_t)C_CLK, "miso=", (uint32_t)C_MISO);

	/* Vendor probe order: reset high, eng-reset command, reset high, 10 ms. */
	gpio_set(C_RST, 1);
	delay_us(5000);
	nvt_write_addr(0x7FFF80, 0x5A);		/* nvt_eng_reset() */
	gpio_set(C_RST, 1);
	delay_us(15000);

	con_hex2("TP lines", "rst=", (uint32_t)gpio_get(C_RST),
		 "irq=", (uint32_t)gpio_get(C_IRQ));

	nvt_read_addr(0x3F000, b, 8);
	dump("TP 3F000", b, 8);

	nvt_read_addr(0x1F600, b, 8);
	dump("TP 1F600", b, 8);

	if (nvt_get_fw_info(&info) == 0) {
		con_hex2("TP fw", "ver=", info.fw_ver, "type=", info.fw_type);
		con_hex2("TP geom", "x=", info.abs_x_max, "y=", info.abs_y_max);
		con_str("TP verdict", "chip answered, firmware info valid");
	} else {
		con_str("TP verdict", "no firmware info (0x0000 / bus silent)");
	}
}
