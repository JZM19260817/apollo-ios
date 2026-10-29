/*
 * Novatek NT36xxx SPI protocol layer.
 *
 * Framing taken from the vendor driver (nt36672c/nt36xxx.c, GPL-2.0):
 *   write : first byte | 0x80
 *   read  : first byte & 0x7F, one dummy byte, then N data bytes
 *   page  : write { 0xFF, (addr >> 15) & 0xFF, (addr >> 7) & 0xFF }
 *   offset: addr & 0x7F
 *
 * The transport is abstracted so the same code runs on the shim (bit-banged
 * GPIO) and on the host (a mock that replays captured bytes).
 */
#ifndef NVT_H
#define NVT_H

#include <stdint.h>

#define NVT_MAX_FINGERS	10
#define NVT_POINT_LEN	65	/* vendor POINT_DATA_LEN */

/*
 * Transport hooks, implemented by the caller (bit-banged GPIO on the shim,
 * a mock in the host test). Plain functions rather than a vtable: the shim is
 * loaded without relocation, so no absolute pointers in data.
 */
void nvt_hw_cs(int assert);		/* 1 = CS low (selected) */
uint8_t nvt_hw_xfer(uint8_t out);	/* one byte, returns MISO byte */

struct nvt_fw_info {
	uint8_t fw_ver;
	uint8_t fw_type;
	uint8_t x_num;
	uint8_t y_num;
	uint16_t abs_x_max;
	uint16_t abs_y_max;
	uint16_t pid;
};

struct nvt_point {
	uint8_t id;		/* 1..NVT_MAX_FINGERS */
	uint8_t status;		/* 1 = down, 2 = move */
	uint16_t x;
	uint16_t y;
	uint8_t weight;
	uint8_t pressure;
};


/* Transport helpers (exposed for tests). */
void nvt_set_page(uint32_t addr);
int nvt_write_addr(uint32_t addr, uint8_t value);
int nvt_read_addr(uint32_t addr, uint8_t *buf, unsigned len);

/* Higher level: read the FW info block, which carries the panel geometry. */
int nvt_get_fw_info(struct nvt_fw_info *out);

/*
 * Parse one point-data packet. Returns the number of fingers found and fills
 * up to `max` entries. `len` is expected to be NVT_POINT_LEN.
 */
int nvt_parse_points(const uint8_t *data, unsigned len, struct nvt_point *pts, int max);

#endif /* NVT_H */
