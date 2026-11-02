/*
 * RPMh support for the bare-metal shim.
 *
 * Two parts:
 *   - the TCS write vote (proven: L11C turns on with a single-shot write),
 *   - a command-db lookup so a rail can be addressed by name, which lets the
 *     touch test try every plausible supply instead of guessing one.
 *
 * Layout and field offsets mirror the kernel driver (bsd/dev/sweet_rpmh.c),
 * which was validated on this phone: rsc=0x18220000 tcs=0x18220d00 DRV=2,
 * cmd-db at the frame the locator reports, ncpt=16.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define RSC_PA		0x18220000u
#define TCS_PA		0x18220d00u
#define DICT_PA		0x0c3f000cu
#define TCS_STRIDE	0x2a0u
#define CMD_STRIDE	0x14u
#define ACTIVE_N	2u
#define NCPT		16u

#define RH_IRQ_STATUS	0x04u
#define RH_WAIT		0x10u
#define RH_CONTROL	0x14u
#define RH_STATUS	0x18u
#define RH_ENABLE	0x1cu
#define RH_MSGID	0x30u
#define RH_ADDR		0x34u
#define RH_DATA		0x38u
#define RH_CMD_STATUS	0x3cu

#define RH_AMC_EN	(1u << 16)
#define RH_TRIGGER	(1u << 24)
#define RH_ISSUED	(1u << 8)
#define RH_COMPLETE	(1u << 16)
#define RH_WRITE_ACK	(8u | (1u << 8) | (1u << 16))

#define RH_DB_HEADER	144u
#define RH_DB_ENTRY	24u
#define RH_DB_MAGIC	0x0c0330dbu
#define RH_VRM		4u

#define TCS_BASE(t)	(TCS_PA + (t) * TCS_STRIDE)

static uint32_t rd(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint16_t db16(uint32_t base, uint32_t off)
{
	uint32_t v = rd(base + (off & ~3u));

	return (uint16_t)(v >> (8u * (off & 3u)));
}

/* Resolve a regulator name such as "ldoc11" to its RPMh address. */
int rpmh_find(const char *name, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	uint32_t pa = rd(DICT_PA);
	uint32_t size = rd(DICT_PA + 4);

	if (!pa || !size || pa < 0x80000000u) {
		return -1;
	}
	if (rd(pa + 4) != RH_DB_MAGIC) {
		return -2;
	}
	for (unsigned i = 0; i < 8 && name[i]; i++) {
		key[i / 4] |= (uint32_t)(uint8_t)name[i] << (8u * (i % 4));
	}
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = db16(pa, h);

		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + db16(pa, h + 2);
		unsigned count = db16(pa, h + 6);

		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;

			if (rd(pa + e) != key[0] || rd(pa + e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rd(pa + e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM) {
				return -3;
			}
			*address = addr;
			return 0;
		}
	}
	return -4;
}

/* Single-shot vote, as validated for L11C: write once, read once. */
int rpmh_send(uint32_t address, unsigned mv)
{
	const uint32_t offsets[3] = {0, 8, 4};
	uint32_t values[3] = {0, 7, 1};
	uintptr_t b;
	unsigned used = 0, done = 0;

	values[0] = mv;
	for (unsigned t = 0; t < ACTIVE_N; t++) {
		uintptr_t tb = TCS_BASE(t);

		if (rd(tb + RH_STATUS) != 0 && rd(tb + RH_STATUS) != 0xffffffffu &&
		    rd(tb + RH_CONTROL) == 0 && rd(tb + RH_ENABLE) == 0 &&
		    rd(tb + RH_WAIT) == 0) {
			used = t;
			break;
		}
		used = t;
	}
	b = TCS_BASE(used);
	wr(b + RH_IRQ_STATUS, rd(b + RH_IRQ_STATUS));
	for (unsigned c = 0; c < 3; c++) {
		uintptr_t cmd = b + c * CMD_STRIDE;

		wr(cmd + RH_MSGID, RH_WRITE_ACK);
		wr(cmd + RH_ADDR, address + offsets[c]);
		wr(cmd + RH_DATA, values[c]);
	}
	wr(b + RH_WAIT, 7);
	wr(b + RH_ENABLE, 7);
	wr(b + RH_CONTROL, 0);
	wr(b + RH_CONTROL, RH_AMC_EN);
	wr(b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	for (unsigned i = 0; i < 20000u; i++) {
		uint32_t st = rd(b + RH_CMD_STATUS);

		if ((st & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE)) {
			done = 1;
			break;
		}
	}
	return done ? 0 : -1;
}

int rpmh_ldo11c_on(unsigned mv)
{
	uint32_t addr = 0;

	if (rpmh_find("ldoc11", &addr)) {
		/* Fall back to the address measured from the kernel log. */
		addr = 0x41300u;
	}
	return rpmh_send(addr, mv);
}
