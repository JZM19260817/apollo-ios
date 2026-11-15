/*
 * GPU power-up probe for apollo (SM8250, Adreno 650).
 *
 * The first question is whether the GPU block answers at all: bring up the
 * GPUCC clocks and the CX/GX power domains, then read RBBM_STATUS and watch the
 * CP's always-on counter. The counter runs off the 19.2 MHz crystal, so about
 * 192000 ticks in 10 ms means the GPU is alive and its bus is up.
 *
 * Register offsets come from the upstream clock drivers for this SoC
 * (gpucc-sm8250.c, gcc-sm8250.c); the sequence follows the a6xx bring-up that
 * works on the sibling port and the msm DRM a6xx_gmu path.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define GPUCC		0x3D90000UL
#define GCC		0x100000UL
#define GPU		0x3D00000UL

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

/* GPU registers */
#define REG_RBBM_STATUS		0x0210u
#define REG_CP_ALWAYS_ON_COUNTER 0x0980u

#define CBCR_EN			(1u << 0)
#define CBCR_OFF		(1u << 31)
#define GDSC_COLLAPSE		(1u << 0)
#define GDSC_PWR_ON		(1u << 31)
#define SRC_GPLL0		5

/* Millisecond delay off the generic counter (the shim keeps its own static). */
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

static volatile uint32_t *reg(uintptr_t base, uint32_t off)
{
	return (volatile uint32_t *)(base + off);
}

static uint32_t rd(uintptr_t base, uint32_t off)
{
	return *reg(base, off);
}

static void wr(uintptr_t base, uint32_t off, uint32_t v)
{
	*reg(base, off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void rmw(uintptr_t base, uint32_t off, uint32_t clear, uint32_t set)
{
	uint32_t v = rd(base, off);

	wr(base, off, (v & ~clear) | set);
}

static int poll(uintptr_t base, uint32_t off, uint32_t mask, uint32_t want,
    unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if ((rd(base, off) & mask) == want) {
			return 1;
		}
		wait_ms(1);
	}
	return 0;
}

/* Vendor order: engine reset while still in reset, then release, then 10 ms. */
static int branch_on(uint32_t off, const char *name)
{
	rmw(GPUCC, off, 0, CBCR_EN);
	int ok = poll(GPUCC, off, CBCR_OFF, 0, 200);

	con_hex(name, rd(GPUCC, off));
	return ok;
}

static int gcc_branch_on(uint32_t off, const char *name)
{
	rmw(GCC, off, 0, 1u);
	int ok = poll(GCC, off, CBCR_OFF, 0, 200);

	con_hex(name, rd(GCC, off));
	return ok;
}

static int gdsc_on(uint32_t gdscr, uint32_t bcr, uint32_t clamp,
    uint32_t status, const char *name, unsigned ms)
{
	if (bcr != 0) {
		rmw(GPUCC, bcr, 0, 1);
		wait_ms(1);
		rmw(GPUCC, bcr, 1, 0);
	}
	if (clamp != 0) {
		rmw(GPUCC, clamp, 1, 0);
	}
	rmw(GPUCC, gdscr, GDSC_COLLAPSE, 0);
	wait_ms(1);
	int ok = poll(GPUCC, status, GDSC_PWR_ON, GDSC_PWR_ON, ms);

	con_hex(name, rd(GPUCC, status));
	return ok;
}

void test_touch(void)
{
	uint32_t lo1, hi1, lo2, hi2;

	con_clear(0x00aa0000u);
	con_str("GPU", "SM8250 power-up probe");

	/* 1. GPLL0 into the GPU CC, then the GPU's bus clocks. */
	con_hex("gpll0 vote", rd(GCC, GCC_GPU_GPLL0_VOTE));
	rmw(GCC, GCC_GPU_GPLL0_VOTE, 0, 1u << 15);
	con_hex("gpll0 after", rd(GCC, GCC_GPU_GPLL0_VOTE));

	gcc_branch_on(GCC_GPU_MEMNOC_GFX, "memnoc_gfx");
	gcc_branch_on(GCC_GPU_SNOC_DVM_GFX, "snoc_dvm");

	/*
	 * 2. The GDSCs have parent rails in apollo's device tree:
	 *    gpu_cx_gdsc -> pm8150_s3_level, gpu_gx_gdsc -> pm8150a_s1_level,
	 *    both at level 0x10. Without those votes the domains stay collapsed
	 *    no matter what is written to the GDSCR.
	 */
	{
		static const char s3_names[4][20] = {"pm8150_s3_level",
		    "s3_level", "pm8150_s3", "pm8150l_s3_level"};
		static const char s1_names[3][20] = {"pm8150a_s1_level",
		    "s1_level", "pm8150a_s1"};
		uint32_t addr = 0;
		int rc = -1;

		for (unsigned i = 0; i < 4 && rc; i++) {
			rc = rpmh_find(s3_names[i], &addr);
		}
		con_hex("s3 addr", rc ? 0u : addr);
		if (!rc) {
			con_hex("s3 vote", (uint32_t)rpmh_send(addr, 0x10));
		}

		addr = 0;
		rc = -1;
		for (unsigned i = 0; i < 3 && rc; i++) {
			rc = rpmh_find(s1_names[i], &addr);
		}
		con_hex("s1 addr", rc ? 0u : addr);
		if (!rc) {
			con_hex("s1 vote", (uint32_t)rpmh_send(addr, 0x10));
		}
	}

	/* 3. CX domain: the GPU CC's own branches live in it. */
	if (!gdsc_on(GPUCC_CX_GDSCR, 0, 0, GPUCC_CX_GDS_HW_CTRL, "cx gdsc", 500)) {
		con_str("GPU", "CX did not come up");
		goto out;
	}
	branch_on(GPUCC_CX_CXO_CBCR, "gpucc_cxo");
	branch_on(GPUCC_CX_AHB_CBCR, "gpucc_ahb");

	/* 3. GMU clock from GPLL0 / 3 and its branch. */
	wr(GPUCC, GPUCC_GMU_CLK_RCGR + 4, (SRC_GPLL0 << 8) | (2 * 3 - 1));
	rmw(GPUCC, GPUCC_GMU_CLK_RCGR, 0, 1u << 0);
	poll(GPUCC, GPUCC_GMU_CLK_RCGR, 1u << 0, 0, 500);
	con_hex("gmu rcg", rd(GPUCC, GPUCC_GMU_CLK_RCGR));

	rmw(GPUCC, GPUCC_CX_GMU_CBCR, 0xFF0u, 0xFF0u);
	branch_on(GPUCC_CX_GMU_CBCR, "gpucc_cx_gmu");
	branch_on(GPUCC_HLOS1_VOTE_SMMU, "vote_smmu");

	/* 4. GX domain, one write at a time with a marker before each, so a hang
	 * says exactly which access wedged the interconnect. In the last run the
	 * GX GDSCR already read 0xb2220000 (PWR_ON) on the previous build, so do
	 * not insist on the poll: write the three registers, read the status once
	 * and move on. */
	con_str("GPU", "gx 1 block reset");
	rmw(GPUCC, GPUCC_GX_BCR, 0, 1);
	wait_ms(1);
	rmw(GPUCC, GPUCC_GX_BCR, 1, 0);
	con_str("GPU", "gx 2 clamp off");
	rmw(GPUCC, GPUCC_GX_CLAMP_IO, 1, 0);
	con_str("GPU", "gx 3 collapse off");
	rmw(GPUCC, GPUCC_GX_GDSCR, GDSC_COLLAPSE, 0);
	wait_ms(5);
	con_str("GPU", "gx 4 status");
	con_hex("gx gdsc", rd(GPUCC, GPUCC_GX_GDSCR));

	/* 5. Is it alive? RBBM_STATUS plus the always-on counter, 10 ms apart. */
	con_str("GPU", "first gpu read");
	con_hex("rbbm status", rd(GPU, REG_RBBM_STATUS));
	lo1 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER);
	hi1 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER + 4);
	wait_ms(10);
	lo2 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER);
	hi2 = rd(GPU, REG_CP_ALWAYS_ON_COUNTER + 4);
	con_hex2("counter", "a", lo1, "b", lo2);
	con_hex2("delta", "lo", lo2 - lo1, "hi", (hi2 << 16) | (lo1 >> 16));

	con_hex("rbbm again", rd(GPU, REG_RBBM_STATUS));
	con_str("GPU", "probe done, halted");
	return;
out:
	con_str("GPU", "stopped early, halted");
}
