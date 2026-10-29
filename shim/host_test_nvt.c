/*
 * Host test for the Novatek protocol layer (shim/nvt.c).
 *
 * Build:  gcc -std=c99 -Wall -Wextra -o nvt_test host_test_nvt.c nvt.c
 * Run:    ./nvt_test
 *
 * It checks the exact bytes the protocol puts on MOSI (the vendor driver's
 * framing) and decodes a synthetic touch packet.
 */
#include <stdio.h>
#include <string.h>

#include "nvt.h"

static unsigned char tx[256];
static unsigned tx_n;
static const unsigned char *rx;
static unsigned rx_len, rx_i;

static int txn_bytes;

/* The chip only drives MISO after the 2-byte read header (address + dummy). */
void nvt_hw_cs(int assert)
{
	(void)assert;
	txn_bytes = 0;
}

static int txn_is_read;

unsigned char nvt_hw_xfer(unsigned char out)
{
	if (tx_n < sizeof(tx))
		tx[tx_n] = out;
	tx_n++;
	txn_bytes++;
	/* A transaction whose first byte has bit7 clear is a read. */
	if (txn_bytes == 1)
		txn_is_read = ((out & 0x80) == 0);
	if (!txn_is_read || txn_bytes <= 2)
		return 0x00;
	return (rx && rx_i < rx_len) ? rx[rx_i++] : 0;
}

static int failures;

static void check(const char *what, int cond)
{
	printf("%-46s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

int main(void)
{
	/* ---- 1. framing of a page select + read ---------------------------- */
	unsigned char rxbuf[64];
	unsigned char stream[64];

	memset(stream, 0, sizeof(stream));
	for (int i = 0; i < 17; i++)
		stream[i] = (unsigned char)(0x10 + i);

	rx = stream;
	rx_len = sizeof(stream);
	rx_i = 0;
	tx_n = 0;

	nvt_read_addr(0x3F078, rxbuf, 17);

	/* page select: 0xFF, (addr>>15)&0xFF, (addr>>7)&0xFF */
	check("page select byte 0 = 0xFF", tx[0] == 0xFF);
	check("page select byte 1 = (addr>>15)", tx[1] == 0x07);
	check("page select byte 2 = (addr>>7)", tx[2] == 0xE0);
	/* read: address byte (bit7 clear), one dummy, then data */
	check("read address byte = addr & 0x7F", tx[3] == 0x78);
	check("read dummy byte = 0x00", tx[4] == 0x00);
	check("first data byte = 0x10", rxbuf[0] == 0x10);
	check("last data byte", rxbuf[16] == 0x20);

	/* ---- 2. FW info block ---------------------------------------------- */
	unsigned char fw[64];
	struct nvt_fw_info info;

	memset(fw, 0, sizeof(fw));
	fw[0] = 0x78;			/* buf[0] */
	fw[1] = 17;			/* fw_ver */
	fw[2] = (unsigned char)~17;	/* ~fw_ver, the vendor's sanity check */
	fw[3] = 18;			/* x_num */
	fw[4] = 32;			/* y_num */
	fw[5] = 0x04; fw[6] = 0x38;	/* 1080 */
	fw[7] = 0x09; fw[8] = 0x60;	/* 2400 */

	rx = fw;
	rx_len = sizeof(fw);
	rx_i = 0;
	tx_n = 0;

	int ret = nvt_get_fw_info(&info);
	check("fw info accepted", ret == 0);
	check("fw_ver = 17", info.fw_ver == 17);
	check("x_num = 18", info.x_num == 18);
	check("y_num = 32", info.y_num == 32);
	check("abs_x_max = 1080", info.abs_x_max == 1080);
	check("abs_y_max = 2400", info.abs_y_max == 2400);

	/* a broken block must be rejected */
	fw[2] = 0x00;
	rx_i = 0;
	check("broken fw info rejected", nvt_get_fw_info(&info) != 0);

	/* ---- 3. point parsing ---------------------------------------------- */
	unsigned char pkt[NVT_POINT_LEN];
	struct nvt_point pts[NVT_MAX_FINGERS];

	memset(pkt, 0, sizeof(pkt));

	/* finger 1: id=1 downs at (500,1200), weight 7, pressure 33 */
	unsigned pos0 = 1;
	pkt[pos0 + 0] = (1 << 3) | 0x01;
	pkt[pos0 + 1] = (500 >> 4);
	pkt[pos0 + 2] = (1200 >> 4);
	pkt[pos0 + 3] = (unsigned char)(((500 & 0xF) << 4) | (1200 & 0xF));
	pkt[pos0 + 4] = 7;
	pkt[pos0 + 5] = 33;

	/* finger 2: id=2 moves at (1079, 2399) */
	unsigned pos1 = 1 + 6;
	pkt[pos1 + 0] = (2 << 3) | 0x02;
	pkt[pos1 + 1] = (1079 >> 4);
	pkt[pos1 + 2] = (2399 >> 4);
	pkt[pos1 + 3] = (unsigned char)(((1079 & 0xF) << 4) | (2399 & 0xF));
	pkt[pos1 + 4] = 3;
	pkt[pos1 + 5] = 11;

	int n = nvt_parse_points(pkt, sizeof(pkt), pts, NVT_MAX_FINGERS);
	check("two fingers parsed", n == 2);
	check("finger 1 id/status", pts[0].id == 1 && pts[0].status == 1);
	check("finger 1 x = 500", pts[0].x == 500);
	check("finger 1 y = 1200", pts[0].y == 1200);
	check("finger 2 id/status", pts[1].id == 2 && pts[1].status == 2);
	check("finger 2 x = 1079", pts[1].x == 1079);
	check("finger 2 y = 2399", pts[1].y == 2399);

	/* a released finger (status 0) must be ignored */
	pkt[pos0 + 0] = (1 << 3) | 0x00;
	n = nvt_parse_points(pkt, sizeof(pkt), pts, NVT_MAX_FINGERS);
	check("released finger ignored", n == 1 && pts[0].id == 2);

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}



