/*
 * Dense dump of XNU's persistent console log, 64 KiB back from the end.
 *
 * One 16 KiB SCSI read per page, printed at 8x16 so a photo captures ~20000
 * characters. After printing it waits 45 s and then resets through PSCI (the
 * APSS watchdog does not fire in this state), with the IMEM restart reason set
 * so the phone comes back to the bootloader instead of bootlooping Android.
 *
 * Log layout on disk (XNU patch 0047), at the end of the partition:
 *   block 0        "SWEETLOG" | u32 version | u32 ring_size | u64 total
 *   blocks 1..N    the ring, byte n at ring[n % ring_size]
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"
#include "logdump.h"

#define RING_MAX	(4u << 20)
#define CHUNK		(4u * 4096)
#define BACK		(5u * CHUNK)	/* window: 64 KiB before the end */

static uint8_t buf[CHUNK] __attribute__((aligned(4096)));

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static void delay_ms(unsigned ms)
{
	uint64_t f, end;

	__asm__ volatile("isb; mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(end));
	end += f / 1000 * ms;
	for (;;) {
		uint64_t now;
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
		if (now >= end)
			break;
	}
}

static void psci_call(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
}

static void __attribute__((noreturn)) reboot(void)
{
	/* qcom,msm-imem restart_reason@65c = 0x77665500 -> bootloader */
	*(volatile uint32_t *)0x146bf65cUL = 0x77665500U;
	__asm__ volatile("dsb sy" ::: "memory");

	psci_call(0xc4000012UL);	/* SYSTEM_RESET2 */
	delay_ms(200);
	psci_call(0x84000009UL);	/* SYSTEM_RESET */
	delay_ms(200);
	*(volatile uint32_t *)0x0c264000UL = 0;	/* PSHOLD */
	__asm__ volatile("dsb sy" ::: "memory");

	/* last resort */
	*(volatile uint32_t *)(0x17c10000UL + 0x08) = 5000;
	*(volatile uint32_t *)(0x17c10000UL + 0x0c) = 6000;
	*(volatile uint32_t *)(0x17c10000UL + 0x04) = 1;
	for (;;)
		__asm__ volatile("wfe");
}

void logdump(void)
{
	uint32_t lba = 0, nblk = 0, base, ring_blk, in_ofs;
	uint64_t total, start;
	int rc;

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

	start = total > BACK ? total - BACK : 0;
	ring_blk = (uint32_t)(start / 4096);
	in_ofs = (uint32_t)(start % 4096);

	rc = ufs_read(base + 1 + ring_blk, CHUNK / 4096, buf);
	con_hex2("READ", "rc=", (uint32_t)rc, "blk=", ring_blk);
	if (rc)
		goto out;

	con_skip();
	con_raw1((const char *)&buf[in_ofs], CHUNK - in_ofs);

out:
	con_str("END", "reboot in 45s");
	delay_ms(45000);
	reboot();
}
