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
	/* SPI_WRITE_MASK(): the first byte carries the write opcode in bit 7. */
	(void)xfer((uint8_t)((addr & 0x7fu) | 0x80u));
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


/* Write n bytes starting at addr (page select once, then offset + payload). */
static void write_bytes(uint32_t addr, const uint8_t *d, unsigned n)
{
	page(addr);
	cs(1);
	/* CTP_SPI_WRITE() applies SPI_WRITE_MASK() to the first byte. */
	(void)xfer((uint8_t)((addr & 0x7fu) | 0x80u));
	for (unsigned i = 0; i < n; i++) {
		(void)xfer(d[i]);
	}
	cs(0);
	delay_us(2);
}

/* Vendor CheckSum(): sums len+1 bytes, adds len, negates. */
static uint32_t fw_checksum(const unsigned char *data, uint32_t len)
{
	uint32_t sum = 0;

	for (uint32_t i = 0; i < len + 1u; i++) {
		sum += data[i];
	}
	sum += len;
	return ~sum + 1u;
}

static void bld_crc_bank(uint32_t des, uint32_t sram, uint32_t length_addr,
    uint32_t size, uint32_t gsum, uint32_t crc)
{
	uint8_t b[4];

	b[0] = (uint8_t)sram;
	b[1] = (uint8_t)(sram >> 8);
	b[2] = (uint8_t)(sram >> 16);
	write_bytes(des, b, 3);

	b[0] = (uint8_t)size;
	b[1] = (uint8_t)(size >> 8);
	b[2] = (uint8_t)((size >> 16) & 1u);
	write_bytes(length_addr, b, 3);

	b[0] = (uint8_t)crc;
	b[1] = (uint8_t)(crc >> 8);
	b[2] = (uint8_t)(crc >> 16);
	b[3] = (uint8_t)(crc >> 24);
	write_bytes(gsum, b, 4);
}

static void printf_line_st(unsigned i, uint8_t v)
{
	con_hex2("state", "i", i, "v", v);
}


static int memcmp8(const uint8_t *x, const uint8_t *y)
{
	for (unsigned i = 0; i < 8; i++) {
		if (x[i] != y[i]) {
			return 1;
		}
	}
	return 0;
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

	con_clear(0x00aa0000u);
	con_str("FWU", "upload NT36675 firmware");

	/*
	 * The vendor's hw_crc path starts with nvt_bootloader_reset() only: no
	 * hardware reset and no engine reset. Follow it exactly.
	 */
	pin_out(C_CLK, 0);
	pin_out(C_MOSI, 0);
	pin_in(C_MISO);
	pin_out(C_CS, 1);

	/* nvt_bootloader_reset(): 0x69 to SWRST_N8, 5 ms, leave fast-read mode. */
	write_addr(0x3F0FEu, 0x69u);
	delay_us(5000);
	write_addr(0x3F310u, 0x00u);
	delay_us(2000);
	con_str("FWU", "in bootloader");

	/* Does a known pattern survive a write with the corrected opcode? */
	{
		static const uint8_t pat[4] = {0xAA, 0x55, 0x12, 0x34};
		uint8_t rb[4];

		write_chunk(0x0u, pat, 4);
		read_addr(0x0u, rb, 4);
		con_hex2("pat rb", "a", ((uint32_t)rb[0] << 8) | rb[1], "b",
		    ((uint32_t)rb[2] << 8) | rb[3]);
		con_hex2("pat exp", "a", 0xAA55, "b", 0x1234);
	}

	/* Header: [0..3] size, [4..11] ILM, [12..23] DLM, [0x30..] info sections */
	hdr_end = w32(nvt_fw);

	for (i = 0; i < 2; i++) {
		sec[nsec].bin = w32(&nvt_fw[0 + i * 12]);
		sec[nsec].sram = w32(&nvt_fw[4 + i * 12]);
		sec[nsec].size = w32(&nvt_fw[8 + i * 12]);
		con_hex2(i == 0 ? "ILM" : "DLM", "sram", sec[nsec].sram,
		    "size", sec[nsec].size);
		nsec++;
	}
	/* Info sections follow at 0x30, 16 bytes each: sram, size, bin, crc. */
	for (i = 0; i < 13 && nsec < 16; i++) {
		uint32_t base = 0x30u + i * 16u;

		sec[nsec].sram = w32(&nvt_fw[base]);
		sec[nsec].size = w32(&nvt_fw[base + 4]);
		sec[nsec].bin = w32(&nvt_fw[base + 8]);
		if (sec[nsec].size != 0u) {
			con_hex2("info", "sram", sec[nsec].sram,
			    "size", sec[nsec].size);
			nsec++;
		}
	}
	(void)hdr_end;
	(void)ovly_sec;
	(void)info_sec;
	(void)pos;

	/* nvt_set_bld_hw_crc(): descriptors, lengths and golden sums first. */
	{
		/* hw_crc != 0: the golden sums come from the header, not from us. */
		uint32_t ilm_crc = w32(&nvt_fw[0x18]);
		uint32_t dlm_crc = w32(&nvt_fw[0x1C]);

		con_hex2("crc", "ilm", ilm_crc, "dlm", dlm_crc);
		bld_crc_bank(0x3F128u, sec[0].sram, 0x3F118u, sec[0].size,
		    0x3F100u, ilm_crc);
		bld_crc_bank(0x3F12Cu, sec[1].sram, 0x3F130u, sec[1].size,
		    0x3F104u, dlm_crc);
	}

	/* Write every section. */
	for (i = 0; i < nsec; i++) {
		uint32_t size = sec[i].size + 1u;	/* vendor writes size+1 */
		uint32_t off = 0;


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

	/* Do three samples of the written image match the source? */
	{
		static const uint32_t probes[3] = {0x0u, 0x1000u, 0x8000u};
		uint8_t rb[8];

		for (unsigned k = 0; k < 2; k++) {
			uint32_t off = probes[k];
			const unsigned char *e = &nvt_fw[sec[0].bin + off];

			read_addr(sec[0].sram + off, rb, 8);
			con_hex2("ilm rb", "a", ((uint32_t)rb[0] << 8) | rb[1], "b",
			    ((uint32_t)rb[2] << 8) | rb[3]);
			con_hex2("ilm ex", "a", ((uint32_t)e[0] << 8) | e[1], "b",
			    ((uint32_t)e[2] << 8) | e[3]);
		}
		for (unsigned k = 0; k < 3; k++) {
			uint32_t off = (k == 2) ? (sec[1].size - 8u) : probes[k];
			const unsigned char *e = &nvt_fw[sec[1].bin + off];

			read_addr(sec[1].sram + off, rb, 8);
			con_hex2("dlm rb", "a", ((uint32_t)rb[0] << 8) | rb[1], "b",
			    ((uint32_t)rb[2] << 8) | rb[3]);
			con_hex2("dlm ex", "a", ((uint32_t)e[0] << 8) | e[1], "b",
			    ((uint32_t)e[2] << 8) | e[3]);
		}
	}

	/*
	 * nvt_bld_crc_enable() is a read-modify-write that sets bit 7 of the byte
	 * at 0x3f30e; writing 0x01 there (as we did before) clears bit 7 and
	 * sets the wrong bit.
	 */
	{
		uint8_t b[2];

		read_addr(0x3F30Eu, b, 2);
		write_addr(0x3F30Eu, (uint8_t)(b[0] | 0x80u));
		read_addr(0x3F30Eu, b, 2);
	}

	/* nvt_fw_crc_enable(): clear the reset status, then HOST_CMD 0xae. */
	write_addr(0x22D60u, 0x00u);
	write_addr(0x22D50u, 0xAEu);

	/* Boot ready bit, then wait for the reset state. */
	read_addr(0x3F10Du, st, 2);
	write_addr(0x3F10Du, 0x01u);
	for (i = 0; i < 100; i++) {
		read_addr(0x3F10Du, st, 2);
		if (st[0] == 0x01u) {
			break;
		}
		delay_us(2000);
	}
	con_hex("boot ready after", st[0]);

	/* Wait for the baseline to finish: 0xa2 ready, 0xa3 normal run. */
	for (i = 0; i < 3000; i++) {
		read_addr(0x22D60u, st, 2);
		if ((i % 300u) == 0u) {
			con_hex2("state", "i", i / 100u, "v", st[0]);
		}
		if (st[0] >= 0xA2u && st[0] <= 0xAFu) {
			break;
		}
		delay_us(10000);
	}
	con_hex("reset state", st[0]);

	/* Watch the event buffer for 20 s: touch the screen during this window. */
	{
		uint8_t a[8], b[8];
		unsigned changes = 0;

		read_addr(0x22D00u, a, 8);
		for (i = 0; i < 2000; i++) {
			read_addr(0x22D00u, b, 8);
			if (memcmp8(a, b)) {
				changes++;
				for (unsigned k = 0; k < 8; k++) {
					a[k] = b[k];
				}
				if (changes <= 4) {
					con_hex2("ev chg", "a",
					    ((uint32_t)b[0] << 8) | b[1], "b",
					    ((uint32_t)b[2] << 8) | b[3]);
				}
			}
			delay_us(10000);
		}
		con_hex("ev changes", changes);
	}



	read_addr(0x3F004u, id, 8);
	con_hex2("id", "a", ((uint32_t)id[0] << 8) | id[1], "b",
	    ((uint32_t)id[2] << 8) | id[3]);

	con_str("FWU", "done, halted");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
