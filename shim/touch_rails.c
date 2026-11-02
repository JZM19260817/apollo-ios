/*
 * Rail sweep only: power every candidate supply, read the controller's
 * BOOT_RDY register after each, print one line per rail and stop.
 *
 * Deliberately minimal - no mode sweep, no bit-bang, no touch watch - so the
 * whole result fits on one screen and can be photographed. The phone is left
 * halted instead of rebooting, because the cache log stopped updating and the
 * screen is the reliable channel.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define SE4		0x990000UL
#define GCC		0x100000UL

#define GCC_RCG_S4	0x174d0u
#define GCC_VOTE	0x52008u
#define VOTE_M33	((1u << 6) | (1u << 7) | (1u << 8) | (1u << 14))

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
#define SE_GSI_EVENT_EN		0xe18u
#define SE_IRQ_EN		0xe1cu

#define SPI_TX_RX	7u
#define M_CMD_DONE	(1u << 0)

#define TLMM		0x0F000000UL
#define TILE_NORTH	0x00900000UL
#define TILE_SOUTH	0x00500000UL
#define QUP4_FUNC	1u

#define PIN_CLK		8
#define PIN_MOSI	9
#define PIN_MISO	10
#define PIN_CS		11

#define CFG(pin, tile)	(TLMM + (tile) + 0x1000UL * (pin))
#define C_CLK		CFG(PIN_CLK, TILE_NORTH)
#define C_MOSI		CFG(PIN_MOSI, TILE_NORTH)
#define C_MISO		CFG(PIN_MISO, TILE_NORTH)
#define C_CS		CFG(PIN_CS, TILE_NORTH)

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
	uint64_t end = ticks() + (uint64_t)us * 1920ull / 100u;

	while (ticks() < end)
		;
}

static void gpio_mux(uintptr_t cfg, unsigned mux, unsigned pull, unsigned drive,
    int out, int high)
{
	uint32_t v;

	if (out) {
		v = rd(cfg + 4);
		wr(cfg + 4, (v & ~2u) | (high ? 2u : 0u));
	}
	v = rd(cfg);
	v &= ~0x3ffu;
	v |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	wr(cfg, v);
}

static int se_done(unsigned us)
{
	uint64_t end = ticks() + (uint64_t)us * 1920ull / 100u;

	for (;;) {
		uint32_t st = rd(SE4 + SE_GENI_M_IRQ_STATUS);

		if (st & M_CMD_DONE) {
			wr(SE4 + SE_GENI_M_IRQ_CLEAR, st);
			return 0;
		}
		if (ticks() > end) {
			return -1;
		}
	}
}

static int geni_setup(void)
{
	uint32_t cfg = rd(GCC + GCC_RCG_S4 + 4u);

	cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
	wr(GCC + GCC_RCG_S4 + 4u, cfg);
	wr(GCC + GCC_RCG_S4, rd(GCC + GCC_RCG_S4) | 1u);
	delay_us(1000);
	wr(GCC + GCC_VOTE, rd(GCC + GCC_VOTE) | VOTE_M33);
	delay_us(1000);

	wr(SE4 + SE_GENI_FORCE_DEFAULT, 1u);
	wr(SE4 + SE_GENI_OUTPUT_CTRL, 0x7fu);
	wr(SE4 + SE_GSI_EVENT_EN, 0);
	wr(SE4 + SE_IRQ_EN, 0);
	wr(SE4 + SE_GENI_DMA_MODE_EN, rd(SE4 + SE_GENI_DMA_MODE_EN) & ~1u);
	wr(SE4 + SE_GENI_TX_PACK_CFG0, 0xffu);
	wr(SE4 + SE_GENI_RX_PACK_CFG0, 0xffu);
	wr(SE4 + SE_SPI_WORD_LEN, 4u);
	wr(SE4 + SE_SPI_TRANS_CFG, rd(SE4 + SE_SPI_TRANS_CFG) & ~(1u << 1));
	wr(SE4 + SE_SPI_CPHA, 0u);
	wr(SE4 + SE_SPI_CPOL, 0u);
	wr(SE4 + SE_GENI_CLK_SEL, 0u);
	wr(SE4 + GENI_SER_M_CLK_CFG, (2u << 4) | 1u);
	delay_us(500);
	return (int)((rd(SE4 + 0x68u) >> 8) & 0xffu);
}

static uint8_t se_xfer(uint8_t out)
{
	wr(SE4 + SE_SPI_TX_TRANS_LEN, 1u);
	wr(SE4 + SE_SPI_RX_TRANS_LEN, 1u);
	wr(SE4 + SE_GENI_M_CMD0, SPI_TX_RX << 27);
	wr(SE4 + SE_GENI_TX_FIFO, out);
	if (se_done(2000)) {
		return 0xffu;
	}
	return (uint8_t)(rd(SE4 + SE_GENI_RX_FIFO) & 0xffu);
}

static void nvt_cs(int sel)
{
	gpio_mux(C_CS, 0, 0, 6, 1, !sel);
	delay_us(2);
}

static void nvt_read(uint32_t addr, uint8_t *buf, unsigned n)
{
	nvt_cs(1);
	(void)se_xfer(0xffu);
	(void)se_xfer((uint8_t)((addr >> 15) & 0xffu));
	(void)se_xfer((uint8_t)((addr >> 7) & 0xffu));
	nvt_cs(0);
	nvt_cs(1);
	(void)se_xfer((uint8_t)(addr & 0x7fu));
	(void)se_xfer(0u);
	for (unsigned i = 0; i < n; i++) {
		buf[i] = se_xfer(0u);
	}
	nvt_cs(0);
}

/*
 * RPMh addresses of the PM8150L LDOs are evenly spaced. The kernel resolved
 * ldoc11 to 0x41300, so ldocN = 0x40900 + (N - 1) * 0x100. Computing them
 * avoids the command-db lookup, which hangs in the bare-metal shim.
 */
#define LDO_C_BASE	0x40900u

static void show_rail(unsigned n, unsigned mv)
{
	uint32_t addr = LDO_C_BASE + (n - 1u) * 0x100u;
	uint8_t b[4];

	/* Print before the vote so a crash still identifies the rail. */
	con_hex2("rail", "n", n, "volts", mv);
	int sent = rpmh_send(addr, mv);

	delay_us(40000);
	nvt_read(0x3F10D, b, 4);
	con_hex("rail rdy", ((uint32_t)b[0] << 8) | b[1]);
	con_hex("rail send", (uint32_t)sent);
}

void test_touch(void)
{
	int proto;

	con_str("RAIL", "sweep: SE4 + PMIC supplies");
	for (unsigned pin = PIN_CLK; pin <= PIN_MISO; pin++) {
		gpio_mux(CFG(pin, TILE_NORTH), QUP4_FUNC, 0, 6, 0, 0);
	}
	gpio_mux(C_CS, 0, 0, 6, 1, 1);
	proto = geni_setup();
	con_hex("proto", (uint32_t)proto);

	show_rail(6u, 2900u);
	show_rail(7u, 3000u);
	show_rail(9u, 2900u);
	show_rail(10u, 3300u);
	show_rail(11u, 3000u);


	con_str("RAIL", "sweep done, halted");
	for (;;) {
		__asm__ volatile("wfe");
	}
}
