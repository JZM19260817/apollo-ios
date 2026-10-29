/*
 * Dense dump of XNU's persistent console log: one 16 KiB read, printed at the
 * smallest font size so a single photo captures ~20000 characters.
 *
 * Why one read: earlier versions issued one SCSI READ per block and the
 * controller stopped answering after the first few. A single transfer of four
 * blocks keeps the command count at one.
 *
 * Layout of the log on disk (XNU patch 0047), at the end of the partition:
 *   block 0        "SWEETLOG" | u32 version | u32 ring_size | u64 total
 *   blocks 1..N    the ring, byte n at ring[n % ring_size]
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"
#include "logdump.h"

#define RING_MAX	(4u << 20)
#define CHUNK		(4u * 4096)	/* four blocks in one command */

static uint8_t buf[CHUNK] __attribute__((aligned(4096)));

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

void logdump(void)
{
	uint32_t lba = 0, nblk = 0, base;
	uint64_t total, start;
	int rc;

	/*
	 * Arm the watchdog before touching the disk: if anything below hangs the
	 * phone still comes back to the bootloader on its own.
	 */
	*(volatile uint32_t *)0x146bf65cUL = 0x77665500U;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(0x17c10000UL + 0x08) = 120000;
	*(volatile uint32_t *)(0x17c10000UL + 0x0c) = 125000;
	*(volatile uint32_t *)(0x17c10000UL + 0x04) = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	con_clear(0xff101010u);
	con_str("XNU LOG", "v9 dump, 64 KiB back");

	if (ufs_init()) {
		con_str("FAIL", "UFS host not ready");
		goto out;
	}
	if (ufs_find("userdata", &lba, &nblk) || !lba) {
		con_str("FAIL", "no userdata");
		goto out;
	}
	base = lba + nblk - (RING_MAX / 4096 + 1);
	if (ufs_read(base, 1, buf)) {
		con_str("FAIL", "header read failed");
		goto out;
	}
	if (buf[0] != 'S' || buf[1] != 'W' || buf[2] != 'E' || buf[3] != 'E' ||
	    buf[4] != 'T' || buf[5] != 'L' || buf[6] != 'O' || buf[7] != 'G') {
		con_str("FAIL", "no SWEETLOG header");
		goto out;
	}
	total = le64(buf + 16);
	con_hex2("LOG", "bytes=", (uint32_t)total, "ring=", le32(buf + 12));

	start = total > 5 * CHUNK ? total - 5 * CHUNK : 0;
	uint32_t ring_blk = (uint32_t)(start / 4096);
	uint32_t in_ofs = (uint32_t)(start % 4096);

	rc = ufs_read(base + 1 + ring_blk, CHUNK / 4096, buf);
	con_hex2("READ", "rc=", (uint32_t)rc, "blk=", ring_blk);
	if (rc)
		goto out;

	con_skip();
	con_raw1((const char *)&buf[in_ofs], CHUNK - in_ofs);

out:
	*(volatile uint32_t *)0x146bf65cUL = 0x77665500U;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(0x17c10000UL + 0x08) = 120000;
	*(volatile uint32_t *)(0x17c10000UL + 0x0c) = 125000;
	*(volatile uint32_t *)(0x17c10000UL + 0x04) = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
}
