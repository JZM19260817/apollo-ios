/*
 * Read-only SPMI access to the PMIC arbiter, plus a QPNP regulator scan.
 *
 * Register model and command format come from the vendor kernel
 * (drivers/spmi/spmi-pmic-arb.c, GPL-2.0). Only reads are implemented.
 *
 * Live DT node qcom,spmi@c440000:
 *   core 0x0C440000 (0x1100), chnls 0x0C600000, obsrvr 0x0E600000,
 *   cnfg 0x0C40A000, qcom,ee = 0, qcom,channel = 0.
 *
 * Command word (v2+): (opc << 27) | ((addr & 0xFF) << 4) | (len - 1)
 * Read opcode 15; v5 observer window 0x10000*ee + 0x80*apid.
 */
#include <stdint.h>
#include "console.h"
#include "spmi.h"

#define SPMI_CORE	0x0C440000UL
#define SPMI_OBS	0x0E600000UL
#define SPMI_CFG	0x0C40A000UL

#define REG_VERSION	0x0000

#define CH_CMD		0x00
#define CH_STATUS	0x08
#define CH_RDATA0	0x18

#define ST_DONE		1u
#define ST_FAILURE	2u
#define ST_DENIED	4u
#define ST_DROPPED	8u

#define APID_MAX	512
#define APID_MAP	0x900		/* v5 mapping table inside core */
#define OWNER_TABLE	0x700		/* inside cnfg; 3 bits per APID */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void wait_us(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

uint32_t spmi_version(void)
{
	return rd32(SPMI_CORE + REG_VERSION);
}

int spmi_find_apid(uint32_t ppid, uint32_t *out)
{
	for (uint32_t i = 0; i < APID_MAX; i++) {
		uint32_t v = rd32(SPMI_CORE + APID_MAP + 4 * i);

		if (!v)
			continue;
		if (((v >> 8) & 0xFFF) == (ppid & 0xFFF)) {
			*out = i;
			return 0;
		}
	}
	return -1;
}

int spmi_read(uint8_t sid, uint8_t periph, uint8_t reg, uint8_t *buf, unsigned len)
{
	uint32_t apid;
	uint32_t ppid = ((uint32_t)sid << 8) | periph;
	uint32_t base;
	uint32_t cmd;
	uint8_t bc;

	if (!len || len > 8)
		return -3;
	if (spmi_find_apid(ppid, &apid))
		return -1;

	base = SPMI_OBS + 0x80u * apid;		/* ee 0 */
	bc = (uint8_t)(len - 1);
	cmd = (15u << 27) | ((uint32_t)reg << 4) | (bc & 0x7u);

	wr32(base + CH_CMD, cmd);

	for (unsigned i = 0; i < 2000; i++) {
		uint32_t st = rd32(base + CH_STATUS);

		if (st & ST_DONE) {
			if (st & (ST_FAILURE | ST_DENIED | ST_DROPPED))
				return (int)st;
			uint32_t d0 = rd32(base + CH_RDATA0);
			for (unsigned b = 0; b < len && b < 4; b++)
				buf[b] = (uint8_t)(d0 >> (8 * b));
			if (len > 4) {
				uint32_t d1 = rd32(base + CH_RDATA0 + 4);
				for (unsigned b = 4; b < len; b++)
					buf[b] = (uint8_t)(d1 >> (8 * (b - 4)));
			}
			return 0;
		}
		wait_us(1);
	}
	return -2;
}

/* ---- QPNP regulator scan ---------------------------------------------- */

#define REG_TYPE	0x04
#define REG_SUBTYPE	0x05
#define REG_ENABLE	0x46
#define REG_VOLT_LSB	0x70
#define REG_VOLT_MSB	0x71

static void scan_sid(uint8_t sid, uint8_t first, uint8_t last, const char *tag)
{
	static const char hx[] = "0123456789ABCDEF";

	for (uint8_t per = first; per <= last; per++) {
		uint32_t apid;
		uint8_t type = 0, sub = 0, en = 0, vl = 0, vh = 0;
		uint32_t owner = 0xFu;

		if (spmi_find_apid(((uint32_t)sid << 8) | per, &apid))
			continue;
		if (spmi_read(sid, per, REG_TYPE, &type, 1))
			continue;
		if (type == 0x00 || type == 0xFF)	/* peripheral not present */
			continue;
		spmi_read(sid, per, REG_SUBTYPE, &sub, 1);
		spmi_read(sid, per, REG_ENABLE, &en, 1);
		spmi_read(sid, per, REG_VOLT_LSB, &vl, 1);
		spmi_read(sid, per, REG_VOLT_MSB, &vh, 1);
		owner = rd32(SPMI_CFG + OWNER_TABLE + 4 * apid) & 0x7u;

		uint32_t mv = (uint32_t)(((vh & 0x3F) << 8) | vl) * 8;
		char line[22];
		int c = 0;

		line[c++] = hx[(sid >> 4) & 0xF];
		line[c++] = hx[sid & 0xF];
		line[c++] = ':';
		line[c++] = hx[(per >> 4) & 0xF];
		line[c++] = hx[per & 0xF];
		line[c++] = ' ';
		line[c++] = (char)('0' + ((en >> 7) & 1));
		line[c++] = ' ';
		line[c++] = hx[(sub >> 4) & 0xF];
		line[c++] = hx[sub & 0xF];
		line[c++] = ' ';
		line[c++] = (char)('0' + (char)owner);
		line[c++] = (char)('0' + (char)((mv / 1000) / 10 % 10));
		line[c++] = (char)('0' + (char)((mv / 1000) % 10));
		line[c++] = '.';
		line[c++] = (char)('0' + (char)((mv % 1000) / 100));
		line[c] = 0;
		con_str(tag, line);
	}
}

void spmi_probe(void)
{
	uint32_t ver = spmi_version();

	con_hex2("SPMI", "ver=", ver, "core=", (uint32_t)SPMI_CORE);
	if (ver == 0 || ver == 0xFFFFFFFFu) {
		con_str("SPMI", "arbiter not readable, stop");
		return;
	}

	/*
	 * PM8150 = SID 0, PM8150L = SID 1, PM8150B = SID 2 on kona. Each
	 * regulator owns one SPMI peripheral starting at 0x14.
	 */
	scan_sid(0, 0x10, 0x2F, "R S0");
	scan_sid(1, 0x10, 0x2F, "R S1");
	scan_sid(2, 0x10, 0x2F, "R S2");
	con_str("R  fmt", "sid:per on sub ee mV");
}

