/*
 * GMU bring-up probe for apollo (SM8250, Adreno 650).
 *
 * The GPU's register block reads as zero until the GMU is running: on a6xx the
 * GMU owns the GPU's clocks and the core, which is why upstream's
 * gpucc-sm8250.c only exposes the two GDSCs and the GMU clock. So the next step
 * after powering the domains is to load a650_gmu.bin and start the Cortex-M3.
 *
 * Sequence from the msm a6xx_gmu driver:
 *   - the firmware is a vendor segment table: 16-byte entries of
 *     { type, size, dest, 0 } followed by the payload
 *   - segments with dest < 0x4000 go to the CM3 ITCM window (GMU + 0xc00)
 *   - segments with dest >= 0x10004000 go to the CM3 DTCM window (GMU + 0x1c00)
 *   - CM3_BOOT_CONFIG = 2, SYSRESET 1 then 0, then FW_INIT_RESULT tells the story
 *
 * Every step prints a marker first, so a hang says exactly where it happened.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"
#include "gmu_fw_array.h"

#define GPUCC		0x3D90000UL
#define GCC		0x100000UL
#define GPU		0x3D00000UL
#define GMU		0x3D6A000UL

/* GPUCC */
#define GPUCC_CX_GDSCR		0x106cu
#define GPUCC_CX_GDS_HW_CTRL	0x1540u
#define GPUCC_CX_CXO_CBCR	0x109cu
#define GPUCC_CX_AHB_CBCR	0x1078u
#define GPUCC_CX_GMU_CBCR	0x1098u
#define GPUCC_GMU_CLK_RCGR	0x1120u
#define GPUCC_HLOS1_VOTE_SMMU	0x5000u
#define GPUCC_GX_GDSCR		0x100cu
#define GPUCC_GX_BCR		0x1008u
#define GPUCC_GX_CLAMP_IO	0x1508u

/* GCC */
#define GCC_GPU_GPLL0_VOTE	0x52000u
#define GCC_GPU_MEMNOC_GFX	0x7100cu
#define GCC_GPU_SNOC_DVM_GFX	0x71018u

/* GPU */
#define REG_RBBM_STATUS		0x0210u
#define REG_CP_ALWAYS_ON_COUNTER 0x0980u

/* GMU (offsets are byte offsets in the GMU register window) */
#define GMU_CM3_ITCM_START	0x0c00u
#define GMU_CM3_DTCM_START	0x1c00u
#define GMU_CM3_SYSRESET	0x5000u
#define GMU_CM3_BOOT_CONFIG	0x5004u
#define GMU_CM3_FW_INIT_RESULT	0x5070u
#define GMU_CM3_CFG		0x50b4u
#define GMU_HFI_QTBL_INFO	0x6184u
#define GMU_HFI_QTBL_ADDR	0x6188u

#define CBCR_EN			(1u << 0)
#define CBCR_OFF		(1u << 31)
#define GDSC_COLLAPSE		(1u << 0)
#define GDSC_PWR_ON		(1u << 31)
#define SRC_GPLL0		5

static void
wait_ms(unsigned ms)
{
	uint64_t freq, end, t;

	__asm__ volatile("isb; mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(end));
	end += freq / 1000u * ms;
	for (;;) {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
		if (t >= end) {
			break;
		}
	}
}

static uint32_t rd(uintptr_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void wr(uintptr_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void rmw(uintptr_t base, uint32_t off, uint32_t clear, uint32_t set)
{
	wr(base, off, (rd(base, off) & ~clear) | set);
}

static int poll(uintptr_t base, uint32_t off, uint32_t mask, uint32_t want,
    unsigned ms)
{
	for (unsigned i = 0; i < ms; i++) {
		if ((rd(base, off) & mask) == want) {
			return 1;
		}
		wait_ms(1);
	}
	return 0;
}

static uint32_t fw32(unsigned off)
{
	return (uint32_t)gmu_fw[off] | ((uint32_t)gmu_fw[off + 1] << 8) |
	    ((uint32_t)gmu_fw[off + 2] << 16) | ((uint32_t)gmu_fw[off + 3] << 24);
}

/* Power the domains the GPU CC needs. Returns 1 if the GX domain reports on. */
static int
power_up(void)
{
	rmw(GCC, GCC_GPU_GPLL0_VOTE, 0, 1u << 15);
	rmw(GCC, GCC_GPU_MEMNOC_GFX, 0, 1u);
	rmw(GCC, GCC_GPU_SNOC_DVM_GFX, 0, 1u);

	rmw(GPUCC, GPUCC_CX_GDSCR, GDSC_COLLAPSE, 0);
	if (!poll(GPUCC, GPUCC_CX_GDS_HW_CTRL, GDSC_PWR_ON, GDSC_PWR_ON, 500)) {
		con_str("GPU", "cx failed");
		return 0;
	}
	rmw(GPUCC, GPUCC_CX_CXO_CBCR, 0, CBCR_EN);
	rmw(GPUCC, GPUCC_CX_AHB_CBCR, 0, CBCR_EN);

	wr(GPUCC, GPUCC_GMU_CLK_RCGR + 4, (SRC_GPLL0 << 8) | (2 * 3 - 1));
	rmw(GPUCC, GPUCC_GMU_CLK_RCGR, 0, 1u << 0);
	poll(GPUCC, GPUCC_GMU_CLK_RCGR, 1u << 0, 0, 500);

	rmw(GPUCC, GPUCC_CX_GMU_CBCR, 0xFF0u, 0xFF0u);
	rmw(GPUCC, GPUCC_CX_GMU_CBCR, 0, CBCR_EN);
	rmw(GPUCC, GPUCC_HLOS1_VOTE_SMMU, 0, CBCR_EN);

	rmw(GPUCC, GPUCC_GX_BCR, 0, 1);
	wait_ms(1);
	rmw(GPUCC, GPUCC_GX_BCR, 1, 0);
	rmw(GPUCC, GPUCC_GX_CLAMP_IO, 1, 0);
	rmw(GPUCC, GPUCC_GX_GDSCR, GDSC_COLLAPSE, 0);
	wait_ms(50);
	con_hex("power", rd(GPUCC, GPUCC_GX_GDSCR));
	return 1;
}

void test_touch(void)
{
	uint32_t lo, hi, c0, c1;
	unsigned off, segs = 0;

	con_clear(0x00aa0000u);
	con_str("GPU", "GMU bring-up probe");

	if (!power_up()) {
		con_str("GPU", "power failed, halted");
		return;
	}

	/* Is the CM3 already talking? */
	con_hex("gmu bridge", rd(GMU, GMU_CM3_CFG));
	con_hex("gmu init", rd(GMU, GMU_CM3_FW_INIT_RESULT));

	/* What does the RPMh dictionary actually call the rails? */
	{
		uint32_t pa = *(volatile uint32_t *)0x0c3f000cu;
		uint32_t size = *(volatile uint32_t *)0x0c3f0010u;
		unsigned shown = 0;

		con_hex2("dict", "pa", pa, "size", size);
		if (pa >= 0x80000000u && size && *(volatile uint32_t *)(pa + 4) ==
		    0x0c0330dbu) {
			for (unsigned i = 0; i < 8 && shown < 14; i++) {
				uint32_t h = 8u + 16u * i;
				uint32_t w = *(volatile uint32_t *)(pa + (h & ~3u));
				unsigned slave = (w >> (8u * (h & 3u))) & 0xffffu;
				uint32_t cw, count, offs, entries;

				if (!slave) {
					break;
				}
				cw = *(volatile uint32_t *)(pa + ((h + 6) & ~3u));
				count = (cw >> (8u * ((h + 6) & 3u))) & 0xffffu;
				cw = *(volatile uint32_t *)(pa + ((h + 2) & ~3u));
				offs = (cw >> (8u * ((h + 2) & 3u))) & 0xffffu;
				entries = 144u + offs;
				for (unsigned j = 0; j < count && shown < 14; j++) {
					uint32_t e = entries + 24u * j;
					uint32_t k0 = *(volatile uint32_t *)(pa + e);
					char name[9];
					int printable = 1;

					for (unsigned k = 0; k < 8; k++) {
						unsigned char ch = (k0 >> (8u * k)) & 0xffu;

						if (ch < 0x20 || ch > 0x7e) {
							printable = 0;
							break;
						}
						name[k] = (char)ch;
					}
					if (!printable) {
						continue;
					}
					name[8] = 0;
					if (!(name[0] == 'p' || name[0] == 's' ||
					    name[0] == 'l' || name[0] == 'v')) {
						continue;
					}
					con_str("rail", name);
					shown++;
				}
			}
		}
	}

	/* Walk the segment table and load what lands in the CM3 windows. */
	con_str("GPU", "walking segment table");
	/*
	 * The vendor image starts with eight zero bytes before the table, and each
	 * entry is {type=1, size, dest, 0}. The payload follows all entries, so
	 * first pass collects them and finds where the data begins.
	 */
	{
		unsigned first = 8;
		unsigned n = 0;
		uint32_t sizes[8], dests[8];

		off = first;
		while (n < 8 && off + 16 <= GMU_FW_SIZE) {
			uint32_t type = fw32(off);
			uint32_t size = fw32(off + 4);
			uint32_t dest = fw32(off + 8);

			if (type != 1u || size == 0 || size > 0x10000u) {
				break;
			}
			sizes[n] = size;
			dests[n] = dest;
			n++;
			off += 16;
		}
		con_hex("segments", n);
		con_hex("table end", off);

		/* Payload starts after the table; load each segment in order. */
		{
			unsigned data = off;

			for (unsigned i = 0; i < n; i++) {
				con_hex2("seg", "size", sizes[i], "dest", dests[i]);
				if (dests[i] < 0x4000u) {
					uint32_t c = sizes[i] > 0x4000u ? 0x4000u : sizes[i];

					for (uint32_t k = 0; k < c; k += 4) {
						wr(GMU, GMU_CM3_ITCM_START + dests[i] + k,
						    fw32(data + k));
					}
				} else if (dests[i] >= 0x10004000u &&
				    dests[i] < 0x10008000u) {
					uint32_t c = sizes[i] > 0x4000u ? 0x4000u : sizes[i];

					for (uint32_t k = 0; k < c; k += 4) {
						wr(GMU, GMU_CM3_DTCM_START +
						    (dests[i] - 0x10004000u) + k,
						    fw32(data + k));
					}
				}
				data += (sizes[i] + 3u) & ~3u;
			}
			con_hex("data end", data);
		}
	}
	con_hex("fw used", off);

	con_str("GPU", "starting cm3");
	wr(GMU, GMU_CM3_BOOT_CONFIG, 0x02u);
	wr(GMU, GMU_HFI_QTBL_INFO, 0x1u);
	wr(GMU, GMU_CM3_SYSRESET, 1u);
	wait_ms(1);
	wr(GMU, GMU_CM3_SYSRESET, 0u);
	wait_ms(10);
	con_hex("init result", rd(GMU, GMU_CM3_FW_INIT_RESULT));
	wait_ms(200);
	con_hex("init later", rd(GMU, GMU_CM3_FW_INIT_RESULT));

	con_str("GPU", "reading gpu");
	con_hex("rbbm", rd(GPU, REG_RBBM_STATUS));
	lo = rd(GPU, REG_CP_ALWAYS_ON_COUNTER);
	hi = rd(GPU, REG_CP_ALWAYS_ON_COUNTER + 4);
	wait_ms(10);
	c0 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER);
	c1 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER + 4);
	con_hex2("counter", "a", lo, "b", c0);
	con_hex2("delta", "lo", c0 - lo, "hi", hi ^ c1);

	con_str("GPU", "probe done, halted");
}
