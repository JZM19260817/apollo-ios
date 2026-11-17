/*
 * GPU RSC init for apollo (SM8250, Adreno 650), then the GMU probe again.
 *
 * The GMU register window read as zero because the RSC (Resource State
 * Coordinator, the block inside the GPU that runs the power handshakes) had never
 * been initialised. The vendor driver's a6xx_load_rsc_ucode() does it first, and it
 * takes a different path for the a650 family: RSCC lives at its own base (0x3de0000
 * in apollo's device tree, "rscc" in the kgsl-3d0 node) and the sequencer takes a
 * five-word ucode of its own.
 *
 * Offsets are the vendor's word offsets (a6xx_reg.h), so they are shifted by 2 here.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"
#include "gmu_fw_array.h"

/* The shim's log writer: used here as a crash checkpoint in the cache partition. */
int ufs_write_log(const void *buf, unsigned len);

/* Little-endian word out of the embedded firmware image. */
static uint32_t
fw32(unsigned off)
{
	return (uint32_t)gmu_fw[off] | ((uint32_t)gmu_fw[off + 1] << 8) |
	    ((uint32_t)gmu_fw[off + 2] << 16) |
	    ((uint32_t)gmu_fw[off + 3] << 24);
}

/* Write the current step to cache first, so a hang still leaves a trace. */
static char ck_buf[512];
static unsigned ck_len;

static void ckpt(const char *s)
{
	unsigned i = 0;

	/* Append, so a crash shows every step that was reached, not just the last. */
	while (s[i] && ck_len + 2 < sizeof(ck_buf)) {
		ck_buf[ck_len++] = s[i++];
	}
	ck_buf[ck_len++] = '\n';
	(void)ufs_write_log(ck_buf, ck_len);
	con_str("CK", s);
}

#define GPUCC		0x3D90000UL
#define GCC		0x100000UL
#define GPU		0x3D00000UL
#define GMU		0x3D6A000UL
#define RSCC		0x3DE0000UL
#define PDC_CFG		0xB290000UL
#define PDC_SEQ		0xB490000UL

/* PDC, word offsets from a6xx_reg.h */
#define PDC_GPU_ENABLE_PDC		0x1140u
#define PDC_GPU_SEQ_START_ADDR		0x1148u
#define PDC_GPU_TCS1_CONTROL		0x1572u
#define PDC_GPU_TCS1_CMD_ENABLE_BANK	0x1573u
#define PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL	0x1574u
#define PDC_GPU_TCS1_CMD0_MSGID		0x1575u
#define PDC_GPU_TCS1_CMD0_ADDR		0x1576u
#define PDC_GPU_TCS1_CMD0_DATA		0x1577u
#define PDC_GPU_TCS3_CONTROL		0x15D6u
#define PDC_GPU_TCS3_CMD_ENABLE_BANK	0x15D7u
#define PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL	0x15D8u
#define PDC_GPU_TCS3_CMD0_MSGID		0x15D9u
#define PDC_GPU_TCS3_CMD0_ADDR		0x15DAu
#define PDC_GPU_TCS3_CMD0_DATA		0x15DBu
#define PDC_CMD_OFFSET			4u

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

/* GMU */
#define GMU_CM3_CFG		0x140b4u
#define CM3_ITCM		0x3000u
#define CM3_DTCM		0x7000u
#define CM3_SYSRESET		0x14000u
#define CM3_BOOT_CONFIG		0x14004u
#define CM3_FW_INIT		0x14070u


/* RSCC, word offsets from a6xx_reg.h */
#define RSCC_RSC_STATUS0_DRV0	0x004u
#define RSCC_PDC_SEQ_START_ADDR	0x008u
#define RSCC_PDC_MATCH_VALUE_LO	0x009u
#define RSCC_PDC_MATCH_VALUE_HI	0x00Au
#define RSCC_PDC_SLAVE_ID_DRV0	0x00Bu
#define RSCC_HIDDEN_TCS_CMD0_ADDR 0x00Du
#define RSCC_HIDDEN_TCS_CMD0_DATA 0x00Eu
#define RSCC_OVERRIDE_START_ADDR 0x100u
#define RSCC_SEQ_MEM_0_DRV0	0x180u

#define CBCR_EN			(1u << 0)
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

/* The vendor writes word offsets; RSCC is a separate block at 0x3de0000. */
static void
rscw(uint32_t word_off, uint32_t value)
{
	wr(RSCC, word_off << 2, value);
}

/* a6xx_load_rsc_ucode(), a650 path. */
static void
load_rsc_ucode(void)
{
	/* Disable SDE clock gating. */
	rscw(RSCC_RSC_STATUS0_DRV0, 1u << 24);

	/* RSC PDC handshake for sleep and wakeup. */
	rscw(RSCC_PDC_SLAVE_ID_DRV0, 1);
	rscw(RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	rscw(RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	rscw(RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0);
	rscw(RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0);
	rscw(RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000u);
	rscw(RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0);
	rscw(RSCC_OVERRIDE_START_ADDR, 0);
	rscw(RSCC_PDC_SEQ_START_ADDR, 0x4520);
	rscw(RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	rscw(RSCC_PDC_MATCH_VALUE_HI, 0x4514);

	/* RSC sequencer ucode for sleep and wakeup, a650 variant. */
	rscw(RSCC_SEQ_MEM_0_DRV0 + 0, 0xEAAAE5A0u);
	rscw(RSCC_SEQ_MEM_0_DRV0 + 1, 0xE1A1EBABu);
	rscw(RSCC_SEQ_MEM_0_DRV0 + 2, 0xA2E0A581u);
	rscw(RSCC_SEQ_MEM_0_DRV0 + 3, 0xECAC82E2u);
	rscw(RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020EDADu);
}

/*
 * cmd_db lookup without the VRM-only restriction that rpmh_find() has: the GPU
 * needs the ARC resources cx.lvl, mx.lvl and xo.lvl.
 *
 *   header: u32 version, u8 magic[4], rsc_hdr[8] (16 bytes each), u32 checksum,
 *           u32 reserved, data[]
 *   rsc_hdr: u16 slv_id, u16 header_offset, u16 data_offset, u16 cnt, ...
 *   entry:  u8 id[8], u32 priority[2], u32 addr, u16 len, u16 offset
 */
static int
db_find(const char *id, uint32_t *addr)
{
	uint32_t pa = *(volatile uint32_t *)0x0c3f000cu;
	uint32_t q0 = 0, q1 = 0;
	unsigned i, j, k;

	if (!pa || pa < 0x80000000u) {
		con_str("dbf", "no pointer");
		return -1;
	}
	if (*(volatile uint32_t *)(pa + 4) != 0x0c0330dbu) {
		con_str("dbf", "bad magic");
		return -2;
	}
	/* Pack the query the way the entries are packed: 8 bytes, low first. */
	for (i = 0; i < 4; i++) {
		q0 |= (uint32_t)(uint8_t)id[i] << (8u * i);
	}
	for (i = 4; i < 8; i++) {
		uint8_t ch = id[i];

		q1 |= (uint32_t)ch << (8u * (i - 4));
	}
	for (i = 0; i < 8; i++) {
		uint32_t h = pa + 8u + 16u * i;
		uint32_t w = *(volatile uint32_t *)h;
		uint32_t w2 = *(volatile uint32_t *)(h + 4);
		unsigned slv = w & 0xffffu;
		unsigned hoff = (w >> 16) & 0xffffu;
		unsigned cnt = (w2 >> 16) & 0xffffu;
		uint32_t base;

		if (!slv) {
			break;
		}
		base = pa + 144u + hoff;
		for (j = 0; j < cnt; j++) {
			uint32_t e = base + 24u * j;
			uint32_t b0 = *(volatile uint32_t *)e;
			uint32_t b1 = *(volatile uint32_t *)(e + 4);

			(void)k;
			if (b0 == q0 && b1 == q1) {
				*addr = *(volatile uint32_t *)(e + 16);
				return 0;
			}
		}
	}
	return -4;
}
/* Print the command DB as it actually is, so the lookup can stop guessing. */
static void
db_dump(void)
{
	uint32_t pa;
	uint32_t w, w2;
	unsigned i, j, k;

	pa = *(volatile uint32_t *)0x0c3f000cu;
	con_hex2("db", "pa", pa, "size", *(volatile uint32_t *)0x0c3f0010u);
	if (!pa || pa < 0x80000000u) {
		con_str("db", "no pointer");
		return;
	}
	w = *(volatile uint32_t *)(pa + 0);
	con_hex2("db hdr", "ver", w, "magic", *(volatile uint32_t *)(pa + 4));
	for (i = 0; i < 8; i++) {
		uint32_t h = pa + 8u + 16u * i;
		unsigned slv, hoff, cnt, doff;

		w = *(volatile uint32_t *)h;
		slv = w & 0xffffu;
		hoff = (w >> 16) & 0xffffu;
		w2 = *(volatile uint32_t *)(h + 4);
		doff = w2 & 0xffffu;
		cnt = (w2 >> 16) & 0xffffu;
		con_hex2("rsc", "slv", slv, "cnt", cnt);
		con_hex2("rsc", "hoff", hoff, "doff", doff);
		if (!slv) {
			break;
		}
		for (j = 0; j < cnt && j < 3; j++) {
			uint32_t e = pa + 144u + hoff + 24u * j;
			char id[9];
			uint32_t addr;

			for (k = 0; k < 8; k++) {
				uint32_t ew = *(volatile uint32_t *)(e & ~3u);

				id[k] = (char)(uint8_t)(ew >> (8u * (e & 3u)));
				e++;
			}
			id[8] = 0;
			e = pa + 144u + hoff + 24u * j;
			addr = *(volatile uint32_t *)(e + 16);
			con_str("  id", id);
			con_hex("  addr", addr);
			/* The raw eight bytes and a live comparison, so the match
			 * rule can be seen rather than assumed. */
			{
				uint32_t e2 = pa + 144u + hoff + 24u * j;
				uint32_t b0 = *(volatile uint32_t *)e2;
				uint32_t b1 = *(volatile uint32_t *)(e2 + 4);
				int eq = 1;
				uint8_t q[8] = {'c', 'x', '.', 'l', 'v', 'l', 0, 0};

				for (k = 0; k < 8; k++) {
					uint32_t w3 = k < 4 ? b0 : b1;
					uint8_t b = (uint8_t)(w3 >> (8u * (k & 3u)));

					if (b != q[k]) {
						eq = 0;
					}
				}
				con_hex2("  raw", "a", b0, "b", b1);
				con_hex("  is cx.lvl", (uint32_t)eq);
			}
		}
	}
}
/* a6xx_load_pdc_ucode(): the sequencer that sequences the GPU's power rails. */
static void
load_pdc_ucode(uint32_t cx, uint32_t mx, uint32_t xo, int have)
{
	/* Sequencer ucode for the power up and down sequences. */
	wr(PDC_SEQ, 0x00u, 0xFEBEA1E1u);
	wr(PDC_SEQ, 0x04u, 0xA5A4A3A2u);
	wr(PDC_SEQ, 0x08u, 0x8382A6E0u);
	wr(PDC_SEQ, 0x0Cu, 0xBCE3E284u);
	wr(PDC_SEQ, 0x10u, 0x002081FCu);

	/* TCS commands for the low power modes. */
	wr(PDC_CFG, PDC_GPU_TCS1_CMD_ENABLE_BANK << 2, 7);
	wr(PDC_CFG, PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL << 2, 0);
	wr(PDC_CFG, PDC_GPU_TCS1_CONTROL << 2, 0);
	wr(PDC_CFG, PDC_GPU_TCS1_CMD0_MSGID << 2, 0x10108u);
	wr(PDC_CFG, PDC_GPU_TCS1_CMD0_ADDR << 2, have ? mx : 0);
	wr(PDC_CFG, PDC_GPU_TCS1_CMD0_DATA << 2, 1);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET) << 2, 0x10108u);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET) << 2,
	    have ? cx : 0);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET) << 2, 0);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET * 2) << 2,
	    0x10108u);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET * 2) << 2,
	    have ? xo : 0);
	wr(PDC_CFG, (PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET * 2) << 2, 0);

	wr(PDC_CFG, PDC_GPU_TCS3_CMD_ENABLE_BANK << 2, 7);
	wr(PDC_CFG, PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL << 2, 0);
	wr(PDC_CFG, PDC_GPU_TCS3_CONTROL << 2, 0);
	wr(PDC_CFG, PDC_GPU_TCS3_CMD0_MSGID << 2, 0x10108u);
	wr(PDC_CFG, PDC_GPU_TCS3_CMD0_ADDR << 2, have ? mx : 0);
	wr(PDC_CFG, PDC_GPU_TCS3_CMD0_DATA << 2, 2);
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET) << 2, 0x10108u);
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET) << 2,
	    have ? cx : 0);
	/* a650 keeps the CX rail at level 2 here. */
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET) << 2, 2);
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET * 2) << 2,
	    0x10108u);
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET * 2) << 2,
	    have ? xo : 0);
	wr(PDC_CFG, (PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET * 2) << 2, 3);

	/* Enable the PDC and point it at the sequence. */
	wr(PDC_CFG, PDC_GPU_SEQ_START_ADDR << 2, 0);
	wr(PDC_CFG, PDC_GPU_ENABLE_PDC << 2, 0x80000001u);
}
static void
power_up(void)
{
	rmw(GCC, GCC_GPU_GPLL0_VOTE, 0, 1u << 15);
	rmw(GCC, GCC_GPU_MEMNOC_GFX, 0, 1u);
	rmw(GCC, GCC_GPU_SNOC_DVM_GFX, 0, 1u);

	rmw(GPUCC, GPUCC_CX_GDSCR, GDSC_COLLAPSE, 0);
	poll(GPUCC, GPUCC_CX_GDS_HW_CTRL, GDSC_PWR_ON, GDSC_PWR_ON, 500);
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
}

void test_touch(void)
{
	con_clear(0x00aa0000u);
	con_str("GPU", "RSC init then GMU probe");

	/* Power first: an access to an unpowered block hangs the interconnect. */
	ckpt("power");
	power_up();
	con_hex("gx gdsc", rd(GPUCC, GPUCC_GX_GDSCR));

	ckpt("rscc-read");
	{
		uint32_t v = rd(RSCC, RSCC_RSC_STATUS0_DRV0 << 2);

		con_hex("rscc status", v);
	}

	ckpt("rsc-ucode");
	load_rsc_ucode();
	wait_ms(5);
	ckpt("rscc-back");
	{
		uint32_t s = rd(RSCC, RSCC_RSC_STATUS0_DRV0 << 2);
		uint32_t q = rd(RSCC, RSCC_SEQ_MEM_0_DRV0 << 2);

		con_hex2("rscc", "after", s, "seq", q);
	}

	ckpt("db-dump");
	db_dump();
	ckpt("pdc-load");
	{
		uint32_t cx = 0, mx = 0, xo = 0;
		int ok = !rpmh_find("cx.lvl", &cx) && !rpmh_find("mx.lvl", &mx) &&
		    !rpmh_find("xo.lvl", &xo);

		con_hex2("rpmh", "cx", cx, "mx", mx);
		con_hex("rpmh xo", xo);
		load_pdc_ucode(cx, mx, xo, ok);
	}

	ckpt("gmu-read");
	/* GMU registers are word-indexed: offset = 4 * index (see HakiOS's GMU_WORD). */
	con_hex2("gmu", "cfg", rd(GMU, GMU_CM3_CFG), "init",
	    rd(GMU, CM3_FW_INIT));

	/*
	 * Map the window: which offsets answer at all? Every read is preceded by
	 * its own checkpoint, so the cache says which offset hung the bus.
	 */
	{
		/* 0x0000..0x0c00 read; 0x1000 hangs. Probe above the hole. */
		static const uint32_t offs[8] = {0x2000u, 0x3000u, 0x4000u,
		    0x7000u, 0xa000u, 0x10000u, 0x12000u, 0x14000u};
		static const char names[8][8] = {"w2000", "w3000", "w4000",
		    "w7000", "wa000", "w10000", "w12000", "w14000"};
		unsigned i;

		for (i = 0; i < 8; i++) {
			ckpt(names[i]);
			con_hex2("win", "off", offs[i], "val",
			    rd(GMU, offs[i]));
		}
	}

	con_str("GPU", "window mapped, halted");
	ckpt("done");
	return;

	/*
	 * Hold the processor in reset before its memories are written: the ITCM
	 * write hung with the core free-running, and a halted CM3 is what the
	 * vendor's own loading sequence implies.
	 */
	ckpt("cm3-reset");
	wr(GMU, CM3_SYSRESET, 1u);
	wait_ms(2);

	/* Load a650_gmu.bin: table at 0x8, payload after it. */
	ckpt("gmu-fw");
	{
		unsigned off = 8, data, n = 0, i, k;
		uint32_t sizes[8], dests[8];

		while (n < 8 && off + 16 <= GMU_FW_SIZE) {
			uint32_t type = fw32(off), size = fw32(off + 4),
			    dest = fw32(off + 8);

			if (type != 1u || size == 0 || size > 0x20000u) {
				break;
			}
			sizes[n] = size;
			dests[n] = dest;
			n++;
			off += 16;
		}
		con_hex2("fw", "segs", n, "data", off);
		data = off;
		for (i = 0; i < n; i++) {
			con_hex2("seg", "size", sizes[i], "dest", dests[i]);
			if (dests[i] < 0x4000u) {
				for (k = 0; k < sizes[i]; k += 4) {
					wr(GMU, CM3_ITCM + dests[i] + k,
					    fw32(data + k));
				}
			} else if (dests[i] >= 0x10004000u &&
			    dests[i] < 0x10008000u) {
				for (k = 0; k < sizes[i]; k += 4) {
					wr(GMU, CM3_DTCM +
					    (dests[i] - 0x10004000u) + k,
					    fw32(data + k));
				}
			}
			data += (sizes[i] + 3u) & ~3u;
		}
		con_hex("fw end", data);
	}

	/* Start the CM3 and watch the init result. */
	ckpt("cm3-start");
	wr(GMU, CM3_BOOT_CONFIG, 0x02u);
	wr(GMU, CM3_SYSRESET, 1u);
	wait_ms(1);
	wr(GMU, CM3_SYSRESET, 0u);
	wait_ms(20);
	con_hex("cm3 init", rd(GMU, CM3_FW_INIT));
	wait_ms(200);
	con_hex("cm3 later", rd(GMU, CM3_FW_INIT));
	con_hex("rbbm", rd(GPU, REG_RBBM_STATUS));
	con_hex("counter", rd(GPU, REG_CP_ALWAYS_ON_COUNTER));
	wait_ms(10);
	con_hex("counter2", rd(GPU, REG_CP_ALWAYS_ON_COUNTER));

	ckpt("done");
}
