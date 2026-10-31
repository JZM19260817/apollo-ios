/*
 * Novatek NT36675 probe over the real GENI SPI controller (QUPv3 SE4).
 *
 * Earlier versions bit-banged the SPI pins as GPIOs because XNU has no clock
 * tree; that never reached the chip. This one programs the SE properly:
 *
 *   GCC      base 0x100000, SE4 clock source at 0x174d0 (XO parent 0, div 1),
 *            branches vote-gated in 0x52008 bits 6 (m_ahb), 7 (s_ahb),
 *            8 (core) and 14 (s4)   [mainline gcc-sm8250.c]
 *   SE4      base 0x990000, protocol SPI (GENI_SE_SPI = 1)
 *   TLMM     pins 8/9/10/11 muxed to function "qup4" = 78, reset 38, irq 39
 *
 * Register offsets and the command opcodes come from the mainline
 * qcom-geni-se.c / spi-geni-qcom.c: SPI_TX_RX = 7, SPI_CS_ASSERT = 8,
 * SPI_CS_DEASSERT = 9, M_CMD_DONE = M_IRQ_STATUS bit 0.
 */
#include <stdint.h>
#include "console.h"
#include "nvt.h"

#define GCC		0x100000UL
#define SE4		0x990000UL

#define GCC_RCG_S4	0x174d0u
#define GCC_VOTE	0x52008u
#define VOTE_M_AHB	(1u << 6)
#define VOTE_S_AHB	(1u << 7)
#define VOTE_CORE	(1u << 8)
#define VOTE_SE4	(1u << 14)

/* GENI SE registers used here. */
#define SE_GENI_FORCE_DEFAULT	0x20u
#define SE_GENI_OUTPUT_CTRL	0x24u
#define GENI_SER_M_CLK_CFG	0x48u
#define SE_GENI_CLK_SEL		0x7cu
#define SE_SPI_CPHA		0x224u
#define SE_SPI_CPOL		0x230u
#define SE_SPI_TRANS_CFG	0x25cu
#define SE_GENI_DMA_MODE_EN	0x258u
#define SE_GENI_TX_PACK_CFG0	0x260u
#define SE_SPI_WORD_LEN		0x268u
#define SE_SPI_TX_TRANS_LEN	0x26cu
#define SE_SPI_RX_TRANS_LEN	0x270u
#define SE_GENI_RX_PACK_CFG0	0x284u
#define SE_GENI_M_CMD0		0x600u
#define SE_GENI_M_IRQ_STATUS	0x610u
#define SE_GENI_M_IRQ_CLEAR	0x618u
#define SE_GENI_TX_FIFO		0x700u
#define SE_GENI_RX_FIFO		0x780u
#define SE_GENI_RX_WATERMARK	0x810u
#define SE_GENI_RX_RFR_WM	0x814u
#define SE_GSI_EVENT_EN		0xe18u
#define SE_IRQ_EN		0xe1cu

#define SPI_TX_RX		7u
#define SPI_CS_ASSERT		8u
#define SPI_CS_DEASSERT		9u
#define M_CMD_DONE		(1u << 0)

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL
#define TILE_SOUTH	0x00500000UL
#define QUP4_FUNC	78u		/* mux value of the "qup4" function */

#define PIN_CLK		8
#define PIN_MOSI	9
#define PIN_MISO	10
#define PIN_CS		11
#define PIN_RST		38
#define PIN_IRQ		39

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CLK		CFG(PIN_CLK, TILE_NORTH)
#define C_MOSI		CFG(PIN_MOSI, TILE_NORTH)
#define C_MISO		CFG(PIN_MISO, TILE_NORTH)
#define C_CS		CFG(PIN_CS, TILE_NORTH)
#define C_RST		CFG(PIN_RST, TILE_SOUTH)
#define C_IRQ		CFG(PIN_IRQ, TILE_SOUTH)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 1920ull / 100u;	/* 19.2 MHz */

	while (ticks() < end)
		;
}

static void gpio_mux(uintptr_t cfg, unsigned mux, unsigned pull, unsigned drive,
    int out, int high)
{
	uint32_t v = rd(cfg + 4);

	if (out)
		wr(cfg + 4, (v & ~2u) | (high ? 2u : 0u));
	v = rd(cfg);
	v &= ~0x3ffu;
	v |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	wr(cfg, v);
}

/* ---- GENI SPI ---------------------------------------------------------- */

static int se_wait_done(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 1920ull / 100u;

	for (;;) {
		uint32_t st = rd(SE4 + SE_GENI_M_IRQ_STATUS);

		if (st & M_CMD_DONE) {
			wr(SE4 + SE_GENI_M_IRQ_CLEAR, st);
			return 0;
		}
		if (ticks() > end)
			return -1;
	}
}

static int se_cmd(uint32_t opcode)
{
	wr(SE4 + SE_GENI_M_CMD0, opcode << 27);
	return se_wait_done(2000);
}

static int geni_spi_init(void)
{
	/* Clocks: SE4 runs from XO (19.2 MHz); the branches are voted. */
	uint32_t cfg = rd(GCC + GCC_RCG_S4 + 4u);

	cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
	wr(GCC + GCC_RCG_S4 + 4u, cfg);
	wr(GCC + GCC_RCG_S4, rd(GCC + GCC_RCG_S4) | 1u);
	for (unsigned i = 0; i < 1000; i++) {
		if (!(rd(GCC + GCC_RCG_S4) & 1u))
			break;
		delay_us(1);
	}
	wr(GCC + GCC_VOTE, rd(GCC + GCC_VOTE) |
	    VOTE_M_AHB | VOTE_S_AHB | VOTE_CORE | VOTE_SE4);
	delay_us(1000);

	/* Protocol must already be SPI (the bootloader selects it). */
	uint32_t proto = rd(SE4 + 0x40u) & 0x7u;	/* SE_GENI_STATUS */

	wr(SE4 + SE_GENI_FORCE_DEFAULT, 1u);
	wr(SE4 + SE_GENI_OUTPUT_CTRL, 0x7fu);
	wr(SE4 + SE_GSI_EVENT_EN, 0);
	wr(SE4 + SE_IRQ_EN, 0);			/* polled, no interrupts */
	wr(SE4 + SE_GENI_DMA_MODE_EN, rd(SE4 + SE_GENI_DMA_MODE_EN) & ~1u);
	wr(SE4 + SE_GENI_RX_WATERMARK, 8u);
	wr(SE4 + SE_GENI_RX_RFR_WM, 9u);
	/* 8-bit words, MSB first, one word per FIFO word. */
	wr(SE4 + SE_GENI_TX_PACK_CFG0, 0xffu);
	wr(SE4 + SE_GENI_TX_PACK_CFG0 + 4u, 0u);
	wr(SE4 + SE_GENI_RX_PACK_CFG0, 0xffu);
	wr(SE4 + SE_GENI_RX_PACK_CFG0 + 4u, 0u);
	wr(SE4 + SE_SPI_WORD_LEN, 8u - 4u);
	wr(SE4 + SE_SPI_TRANS_CFG, rd(SE4 + SE_SPI_TRANS_CFG) & ~(1u << 1));
	wr(SE4 + SE_SPI_CPHA, 0u);
	wr(SE4 + SE_SPI_CPOL, 0u);
	/* XO source, divider 2 => 9.6 MHz SPI clock. */
	wr(SE4 + SE_GENI_CLK_SEL, 0u);
	wr(SE4 + GENI_SER_M_CLK_CFG, (2u << 4) | 1u);

	con_hex2("SPI init", "proto=", proto, "clkcfg=", rd(SE4 + GENI_SER_M_CLK_CFG));
	return proto;
}

void nvt_hw_cs(int assert)
{
	se_cmd(assert ? SPI_CS_ASSERT : SPI_CS_DEASSERT);
}

uint8_t nvt_hw_xfer(uint8_t out)
{
	wr(SE4 + SE_SPI_TX_TRANS_LEN, 1u);	/* one word */
	wr(SE4 + SE_SPI_RX_TRANS_LEN, 1u);
	wr(SE4 + SE_GENI_M_CMD0, SPI_TX_RX << 27);
	wr(SE4 + SE_GENI_TX_FIFO, out);
	if (se_wait_done(2000))
		return 0xff;
	return (uint8_t)(rd(SE4 + SE_GENI_RX_FIFO) & 0xffu);
}

/* ---- probe ------------------------------------------------------------- */

static void dump(const char *tag, const uint8_t *b, unsigned n)
{
	char line[3 * 16 + 1];
	static const char hx[] = "0123456789ABCDEF";
	unsigned c = 0;

	if (n > 16)
		n = 16;
	for (unsigned i = 0; i < n; i++) {
		line[c++] = hx[b[i] >> 4];
		line[c++] = hx[b[i] & 0xf];
		line[c++] = ' ';
	}
	line[c] = 0;
	con_str(tag, line);
}

void test_touch(void)
{
	struct nvt_fw_info info;
	uint8_t b[16];

	con_str("TP", "novatek NT36675 over GENI SPI (SE4)");

	/* Pins 8..11 to the QUP function, reset and irq stay GPIOs. */
	for (unsigned pin = PIN_CLK; pin <= PIN_CS; pin++)
		gpio_mux(CFG(pin, TILE_NORTH), QUP4_FUNC, 0, 6, 0, 0);
	gpio_mux(C_RST, 0, 0, 8, 1, 1);
	gpio_mux(C_IRQ, 0, 3, 3, 0, 0);

	geni_spi_init();

	/* Vendor probe order: reset high, eng-reset command, reset high, 10 ms. */
	delay_us(5000);
	nvt_write_addr(0x7FFF80, 0x5A);
	delay_us(15000);

	con_hex2("TP lines", "rst=", (uint32_t)(rd(C_RST + 4) & 1u),
	    "irq=", (uint32_t)(rd(C_IRQ + 4) & 1u));

	nvt_read_addr(0x3F000, b, 8);
	dump("TP 3F000", b, 8);

	if (nvt_get_fw_info(&info) == 0) {
		con_hex2("TP fw", "ver=", info.fw_ver, "type=", info.fw_type);
		con_hex2("TP geom", "x=", info.abs_x_max, "y=", info.abs_y_max);
		con_hex2("TP pid", "pid=", info.pid, "xnum=", info.x_num);
		con_str("TP verdict", "chip answered over GENI SPI");
	} else {
		con_str("TP verdict", "no firmware info over GENI SPI");
	}
}
