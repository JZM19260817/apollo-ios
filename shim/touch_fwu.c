/*
 * Upload the NT36675 firmware over the bit-banged bus.
 *
 * Port of the vendor's nvt_download_firmware() (nt36xxx_fw_update.c):
 *
 *   reset low, 1 ms -> eng reset (0x5a to 0x7fff80) -> reset high, 10 ms
 *   -> bootloader reset (0x69 to 0x3f0fe, 5 ms, 0 to 0x3f310)
 *   -> clear EVENT_MAP_RESET_COMPLETE (0x22d60)
 *   -> for each header section: set page to its SRAM address and write the
 *      payload in chunks of NVT_TRANSFER_LEN
 *   -> set the boot-ready bit and wait for the reset state to reach 0xa0
 *
 * The firmware blob is the ihex from the sm8250 tree, converted to raw bytes.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"
#include "nvt_fw_array.h"

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL
#define TILE_SOUTH	0x00500000UL

#define PIN_CS		11
#define PIN_RST		38

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CS		CFG(PIN_CS, TILE_NORTH)
#define C_RST		CFG(PIN_RST, TILE_SOUTH)
#define C_CLK		CFG(10, TILE_NORTH)
#define C_MOSI		CFG(9, TILE_NORTH)
#define C_MISO		CFG(8, TILE_NORTH)

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

static void pin_in(uintptr_t cfg)
{
	uint32_t v = rd(cfg);

	v &= ~0x3ffu;
	v |= 3u | (6u << 6);
	wr(cfg, v);
}

static uint8_t xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		pin_out(C_MOSI, (out >> i) & 1);
		delay_us(1);
		pin_out(C_CLK, 1);
		delay_us(1);
		in = (uint8_t)((in << 1) | (uint8_t)(rd(C_MISO + 4) & 1u));
		pin_out(C_CLK, 0);
		delay_us(1);
	}
	return in;
}

static void cs(int sel)
{
	pin_out(C_CS, !sel);
	delay_us(2);
}

static void page(uint32_t addr)
{
	cs(1);
	(void)xfer(0xffu);
	(void)xfer((uint8_t)((addr >> 15) & 0xffu));
	(void)xfer((uint8_t)((addr >> 7) & 0xffu));
	cs(0);
	delay_us(2);
}

static void read_addr(uint32_t addr, uint8_t *buf, unsigned n)
{
	page(addr);
	cs(1);
	(void)xfer((uint8_t)(addr & 0x7fu));
	(void)xfer(0u);
	for (unsigned i = 0; i < n; i++) {
		buf[i] = xfer(0u);
	}
	cs(0);
	delay_us(2);
}

static void write_addr(uint32_t addr, uint8_t value)
{
	page(addr);
	cs(1);
	(void)xfer((uint8_t)((addr & 0x7fu) | 0x80u));
	(void)xfer(value);
	cs(0);
	delay_us(2);
}

/* Write one payload chunk: page to the SRAM address, then offset + data. */
static void write_chunk(uint32_t addr, const unsigned char *data, unsigned n)
{
	page(addr);
	cs(1);
	(void)xfer((uint8_t)(addr & 0x7fu));
	for (unsigned i = 0; i < n; i++) {
		(void)xfer(data[i]);
	}
	cs(0);
	delay_us(2);
}

static uint32_t w32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct section {
	uint32_t bin;
	uint32_t sram;
	uint32_t size;
};

void test_touch(void)
{
	uint8_t id[8], st[2];
	struct section sec[16];
	unsigned nsec = 0;
	uint32_t hdr_end, pos, ovly_sec, info_sec, i;

	con_str("FWU", "upload NT36675 firmware");

	/* SPI pins as GPIO, reset held low while the engine reset goes out. */
	pin_out(C_CLK, 0);
	pin_out(C_MOSI, 0);
	pin_in(C_MISO);
	pin_out(C_CS, 1);
	pin_out(C_RST, 0);
	delay_us(1000);
	write_addr(0x7FFF80u, 0x5Au);
	pin_out(C_RST, 1);
	delay_us(10000);
	write_addr(0x3F0FEu, 0x69u);
	delay_us(5000);
	write_addr(0x3F310u, 0x00u);
	delay_us(2000);
	write_addr(0x22D60u, 0x00u);
	con_str("FWU", "in bootloader");

	/* Header: [0..3] size, [4..11] ILM, [12..23] DLM, [0x30..] info sections */
	hdr_end = w32(nvt_fw);
	con_hex("header size", hdr_end);

	for (i = 0; i < 2; i++) {
		sec[nsec].bin = w32(&nvt_fw[0 + i * 12]);
		sec[nsec].sram = w32(&nvt_fw[4 + i * 12]);
		sec[nsec].size = w32(&nvt_fw[8 + i * 12]);
		con_hex2(i == 0 ? "ILM" : "DLM", "sram", sec[nsec].sram,
		    "size", sec[nsec].size);
		nsec++;
	}
	ovly_sec = (nvt_fw[0x28] & 0x10u) ? (nvt_fw[0x28] & 0x0Fu) : 0u;
	info_sec = 0;
	for (pos = 0x30; pos < hdr_end; pos += 0x10u) {
		info_sec++;
	}
	con_hex2("sections", "ovly", ovly_sec, "info", info_sec);

	/* info sections: SRAM : size : BIN : crc (16 bytes each) */
	for (i = 0; i < info_sec && nsec < 16; i++) {
		uint32_t base = 0x30u + i * 16u;

		sec[nsec].sram = w32(&nvt_fw[base]);
		sec[nsec].size = w32(&nvt_fw[base + 4]);
		sec[nsec].bin = w32(&nvt_fw[base + 8]);
		nsec++;
	}

	/* Write every section. */
	for (i = 0; i < nsec; i++) {
		uint32_t size = sec[i].size + 1u;	/* vendor writes size+1 */
		uint32_t off = 0;

		con_hex2("write", "sram", sec[i].sram, "size", size);
		while (off < size) {
			unsigned n = (size - off) > 1024u ? 1024u : (size - off);

			if (sec[i].bin + off + n > NVT_FW_SIZE) {
				con_str("FWU", "section overruns the blob");
				break;
			}
			write_chunk(sec[i].sram + off, &nvt_fw[sec[i].bin + off], n);
			off += n;
		}
	}
	con_str("FWU", "all sections written");

	/* Boot ready bit, then wait for the reset state. */
	read_addr(0x3F10Du, st, 2);
	con_hex("boot ready before", st[0]);
	write_addr(0x3F10Du, 0x01u);
	for (i = 0; i < 100; i++) {
		read_addr(0x3F10Du, st, 2);
		if (st[0] == 0x01u) {
			break;
		}
		delay_us(2000);
	}
	con_hex("boot ready after", st[0]);

	for (i = 0; i < 200; i++) {
		read_addr(0x22D60u, st, 2);
		if (st[0] >= 0xA0u && st[0] <= 0xAFu) {
			break;
		}
		delay_us(10000);
	}
	con_hex("reset state", st[0]);

	read_addr(0x3F004u, id, 8);
	con_hex2("id", "a", ((uint32_t)id[0] << 8) | id[1], "b",
	    ((uint32_t)id[2] << 8) | id[3]);

	con_str("FWU", "done, halted");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
