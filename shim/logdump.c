/*
 * Read XNU's persistent console log out of the flashed partition and print a
 * filtered view of it.
 *
 * XNU (patch 0047) keeps a 4 MiB ring at the END of the mounted partition:
 *
 *   <partition end - 4 MiB - 4 KiB>  header block:
 *        char  magic[8] = "SWEETLOG"
 *        u32   version  = 1
 *        u32   ring_size
 *        u64   total bytes written
 *   ...followed by ring_size bytes of ring, byte n at ring[n % ring_size].
 *
 * Printing only the raw tail drowns in repeating daemon restarts, so this
 * build scans the whole written ring and keeps the last KEEP_LINES lines that
 * match a keyword set (panic / launchd / SpringBoard / dyld / failed / ...),
 * skipping the SWEET KEYS and SWEET AKS spam. The kept lines are printed in
 * order, oldest first.
 *
 * Built only into the LOGDUMP variant: it replaces booting the payload.
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"
#include "logdump.h"

#define RING_MAX	(4u << 20)
#define LINE_MAX	(3 * 45)	/* three console rows of 45 columns */
#define KEEP_LINES	30

static uint8_t block[4096] __attribute__((aligned(4096)));
static char kept[KEEP_LINES][LINE_MAX];
static unsigned nlines, line_next, total_matches;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int has(const char *s, const char *needle)
{
	for (; *s; s++) {
		const char *a = s, *b = needle;
		while (*a && *b && *a == *b) {
			a++;
			b++;
		}
		if (!*b)
			return 1;
	}
	return 0;
}

static int interesting(const char *s)
{
	static const char *keys[] = {
		"panic", "Panic", "PANIC", "launchd", "SpringBoard", "backboardd",
		"dyld", "boot task", "failed", "Failed", "error", "Error",
		"crash", "watchdog", "No such", "timeout", "AppleKeyStore",
		"IOPlatform", "BootCache", "KernelCache", "lockdown", "userland",
	};

	if (has(s, "SWEET KEYS") || has(s, "SWEET AKS"))
		return 0;
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (has(s, keys[i]))
			return 1;
	return 0;
}

static void keep(const char *s, unsigned len)
{
	if (!len)
		return;
	if (len >= LINE_MAX)
		len = LINE_MAX - 1;
	char *dst = kept[line_next];

	for (unsigned i = 0; i < len; i++) {
		char c = s[i];
		dst[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	dst[len] = 0;
	line_next = (line_next + 1) % KEEP_LINES;
	if (nlines < KEEP_LINES)
		nlines++;
	total_matches++;
}

void logdump(void)
{
	uint32_t lba = 0, nblk = 0, base;
	uint64_t total, start, pos;
	char cur[LINE_MAX];
	unsigned curlen = 0;
	int in_line = 0;

	con_clear(0xff101010u);
	con_str("XNU LOG", "v7 filtered view, 64 KiB window");

	if (ufs_init()) {
		con_str("FAIL", "UFS host not ready");
		goto out;
	}
	if (ufs_find("userdata", &lba, &nblk) || !lba) {
		con_str("FAIL", "cannot find userdata");
		goto out;
	}
	base = lba + nblk - (RING_MAX / 4096 + 1);
	if (ufs_read(base, 1, block)) {
		con_str("FAIL", "cannot read the log header");
		goto out;
	}
	if (block[0] != 'S' || block[1] != 'W' || block[2] != 'E' || block[3] != 'E' ||
	    block[4] != 'T' || block[5] != 'L' || block[6] != 'O' || block[7] != 'G') {
		con_str("FAIL", "no SWEETLOG header");
		goto out;
	}
	total = le64(block + 16);
	con_hex2("LOG", "bytes=", (uint32_t)total, "ring=", le32(block + 12));

	/* Only the recent end of the ring: one UFS transaction per block is
	 * slow enough that scanning all 4 MiB would outlast the watchdog. */
	start = total > 65536 ? total - 65536 : 0;
	for (pos = start; pos < total;) {
		uint32_t off = (uint32_t)(pos % RING_MAX);
		uint32_t blk = off / 4096, inblk = off % 4096;
		uint32_t n = 4096 - inblk;

		if ((uint64_t)n > total - pos)
			n = (uint32_t)(total - pos);
		if (ufs_read_raw(base + 1 + blk, 1, block))
			{ con_str("READ", "failed"); break; }
		for (uint32_t i = 0; i < n; i++) {
			char c = (char)block[inblk + i];
			if (c == '\n') {
				cur[curlen] = 0;
				if (in_line && interesting(cur))
					keep(cur, curlen);
				curlen = 0;
				in_line = 1;
			} else if (curlen < LINE_MAX - 1) {
				cur[curlen++] = c;
				in_line = 1;
			}
		}
		pos += n;
	}
	if (in_line) {
		cur[curlen] = 0;
		if (interesting(cur))
			keep(cur, curlen);
	}

	con_hex2("MATCH", "lines=", total_matches, "shown=", nlines);

	unsigned first = (nlines == KEEP_LINES) ? line_next : 0;
	for (unsigned i = 0; i < nlines; i++) {
		const char *s = kept[(first + i) % KEEP_LINES];
		unsigned n = 0;

		while (s[n])
			n++;
		if (!n)
			continue;
		con_raw(s, n);
		con_raw("\n", 1);
	}
out:
	con_skip();
	con_str("END", "rebooting in 120s");

	/* back to the bootloader, not into a bootlooping Android */
	*(volatile uint32_t *)0x146bf65cUL = 0x77665500U;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(0x17c10000UL + 0x08) = 120000;
	*(volatile uint32_t *)(0x17c10000UL + 0x0c) = 125000;
	*(volatile uint32_t *)(0x17c10000UL + 0x04) = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
}
