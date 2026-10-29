/*
 * Novatek NT36xxx protocol implementation. See nvt.h for the framing notes.
 */
#include "nvt.h"

/* Register addresses inside the "event buffer" page used by the vendor code. */
#define NVT_PAGE_EVENT_BUF	0x00000	/* filled in by nvt_get_fw_info if unknown */
#define EVENT_MAP_FWINFO	0x78
#define EVENT_MAP_PROJECTID	0x9A

static void write_bytes(const uint8_t *b, unsigned n)
{
	nvt_hw_cs(1);
	nvt_hw_xfer((uint8_t)(b[0] | 0x80));
	for (unsigned i = 1; i < n; i++)
		nvt_hw_xfer(b[i]);
	nvt_hw_cs(0);
}

void nvt_set_page(uint32_t addr)
{
	uint8_t b[3] = {
		0xFF,
		(uint8_t)((addr >> 15) & 0xFF),
		(uint8_t)((addr >> 7) & 0xFF),
	};

	write_bytes(b, 3);
}

int nvt_write_addr(uint32_t addr, uint8_t value)
{
	uint8_t b[2];

	nvt_set_page(addr);
	b[0] = (uint8_t)(addr & 0x7F);
	b[1] = value;
	write_bytes(b, 2);
	return 0;
}

int nvt_read_addr(uint32_t addr, uint8_t *buf, unsigned len)
{
	nvt_set_page(addr);

	nvt_hw_cs(1);
	nvt_hw_xfer((uint8_t)(addr & 0x7F));
	nvt_hw_xfer(0x00);			/* dummy byte */
	for (unsigned i = 0; i < len; i++)
		buf[i] = nvt_hw_xfer(0x00);
	nvt_hw_cs(0);
	return 0;
}

int nvt_get_fw_info(struct nvt_fw_info *out)
{
	uint8_t buf[17];

	if (!out)
		return -1;
	nvt_read_addr(NVT_PAGE_EVENT_BUF | EVENT_MAP_FWINFO, buf, sizeof(buf));

	out->fw_ver = buf[1];
	out->fw_type = buf[2];
	out->x_num = buf[3];
	out->y_num = buf[4];
	out->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	out->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);
	out->pid = (uint16_t)((buf[9] << 8) | buf[10]);		/* placeholder, see read_pid */

	/* The vendor driver rejects a block whose version bytes do not invert. */
	if ((uint8_t)(buf[1] + buf[2]) != 0xFF)
		return -2;
	return 0;
}

int nvt_parse_points(const uint8_t *data, unsigned len, struct nvt_point *pts, int max)
{
	int found = 0;

	if (!data || len < NVT_POINT_LEN || !pts || max <= 0)
		return 0;

	for (int i = 0; i < NVT_MAX_FINGERS && found < max; i++) {
		unsigned pos = 1 + 6 * (unsigned)i;
		uint8_t id = (uint8_t)(data[pos] >> 3);
		uint8_t st = (uint8_t)(data[pos] & 0x07);

		if (id == 0 || id > NVT_MAX_FINGERS)
			continue;
		if (st != 0x01 && st != 0x02)		/* down or move */
			continue;

		pts[found].id = id;
		pts[found].status = st;
		pts[found].x = (uint16_t)((data[pos + 1] << 4) | (data[pos + 3] >> 4));
		pts[found].y = (uint16_t)((data[pos + 2] << 4) | (data[pos + 3] & 0x0F));
		pts[found].weight = data[pos + 4];
		pts[found].pressure = data[pos + 5];
		found++;
	}
	return found;
}
