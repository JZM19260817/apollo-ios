/*
 * RPMh write vote for the PM8150L L11C rail, ported from the kernel driver
 * (bsd/dev/sweet_rpmh.c) so the bare-metal shim can power the touch controller
 * before probing it.
 *
 * Everything here was validated on the phone by the kernel driver's log:
 *   rsc=0x18220000 tcs=0x18220d00 DRV=2 TCS=8 ncpt=16
 *   cmd-db pa=0x80860000 size=0x10c0, ldoc11 addr=0x41300 slave=4
 *   TCS0 ACTIVE 3200 mV mode=7 enable=1 -> done=1, cmd-status 0x10101
 *
 * The shim runs with the MMU off, so these physical addresses are used直接.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define RSC_PA		0x18220000u
#define TCS_PA		0x18220d00u
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

/* L11C from the kernel's command-db lookup (ldoc11, slave 4). */
#define LDO11C_ADDR	0x41300u

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

static int wait_eq(uintptr_t a, uint32_t want)
{
	for (unsigned i = 0; i < 2000; i++) {
		if (rd(a) == want)
			return 0;
	}
	return -1;
}

static int tcs_free(unsigned t)
{
	uintptr_t b = TCS_BASE(t);

	return rd(b + RH_STATUS) != 0 && rd(b + RH_STATUS) != 0xffffffffu &&
	    rd(b + RH_CONTROL) == 0 && rd(b + RH_ENABLE) == 0 &&
	    rd(b + RH_WAIT) == 0;
}

int rpmh_ldo11c_on(unsigned mv)
{
	uint32_t id = rd(RSC_PA + 0);
	uint32_t config = rd(RSC_PA + 0x0c);
	unsigned count = (config >> 12) & 0x3fu;	/* DRV=2 */

	con_hex2("RPMH", "id=", id, "config=", config);
	if (config == 0xffffffffu || count < ACTIVE_N) {
		con_str("RPMH", "unsupported RSC geometry");
		return -1;
	}

	int chosen = -1;
	for (unsigned t = 0; t < ACTIVE_N; t++) {
		uintptr_t b = TCS_BASE(t);

		con_hex2("RPMH", "tcs", t, "status=", rd(b + RH_STATUS));
		if (chosen < 0 && tcs_free(t))
			chosen = (int)t;
	}
	if (chosen < 0) {
		con_str("RPMH", "no free ACTIVE TCS");
		return -2;
	}

	unsigned t = (unsigned)chosen;
	uintptr_t b = TCS_BASE(t);
	const uint32_t offsets[3] = {0, 8, 4};
	const uint32_t values[3] = {mv, 7, 1};	/* mV, HPM mode, enable */

	for (unsigned c = 0; c < 3; c++) {
		uintptr_t cmd = b + c * CMD_STRIDE;

		wr(cmd + RH_MSGID, RH_WRITE_ACK);
		wr(cmd + RH_ADDR, LDO11C_ADDR + offsets[c]);
		wr(cmd + RH_DATA, values[c]);
	}
	/*
	 * The kernel version verifies these readbacks, but in the shim the WAIT
	 * register does not read back the written mask (only CPU0 is running).
	 * Log the real values so a mismatch tells us whether the write landed.
	 */
	wr(b + RH_WAIT, 7);
	wr(b + RH_ENABLE, 7);
	con_hex2("RPMH w/e", "wait=", rd(b + RH_WAIT), "enable=", rd(b + RH_ENABLE));
	wr(b + RH_CONTROL, 0);
	wr(b + RH_CONTROL, RH_AMC_EN);
	wr(b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);

	unsigned done = 0, status = 0;

	for (unsigned i = 0; i < 20000; i++) {
		uint32_t cmd_status = rd(b + RH_CMD_STATUS);

		status = rd(b + RH_STATUS);
		if (status != 0 && status != 0xffffffffu &&
		    (cmd_status & (RH_ISSUED | RH_COMPLETE)) ==
		    (RH_ISSUED | RH_COMPLETE)) {
			done = 1;
			break;
		}
	}
	con_hex2("RPMH", "done=", done, "status=", status);
	/* Leave our commands behind: the rail must stay on for the probe. */
	return done ? 0 : -4;
}
