/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;

/*
 * Everything drawn is mirrored into RAM so the shim can dump a real log to
 * flash (the phone has no wired serial port). Cleared only by a reboot.
 */
#define LOG_CAP	(96 * 1024)
static char log_buf[LOG_CAP];
static unsigned log_len;

static void log_putc(char c)
{
	if (log_len + 1 < LOG_CAP) {
		log_buf[log_len++] = c;
		log_buf[log_len] = 0;
	}
}

static void log_puts(const char *s)
{
	while (*s)
		log_putc(*s++);
}

static void log_hex(uint64_t v, int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		log_putc(hx[(v >> (4 * (digits - 1 - i))) & 0xf]);
}

const char *con_log(unsigned *len)
{
	if (len)
		*len = log_len;
	return log_buf;
}

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

static int con_col;

void con_raw(const char *s, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		char ch = s[i];
		if (ch == '\n') {
			row++;
			con_col = 0;
			continue;
		}
		if (ch < 0x20 || ch > 0x7e)
			ch = '.';
		put_char(con_col++, ch, WHITE);
		if (con_col >= 45) {
			con_col = 0;
			row++;
		}
	}
}


/* Smallest text: 8x16 cells, 135 columns x 150 rows, for dense log dumps. */
static int row1, col1;

void con_raw1(const char *s, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		char ch = s[i];

		if (ch == '\n') {
			row1++;
			col1 = 0;
			continue;
		}
		if (ch < 0x20 || ch > 0x7e)
			ch = '.';
		if (row1 * 16 + 16 <= FB_H && col1 * 8 + 8 <= FB_W) {
			const unsigned char *g = font8x16[ch - 0x20];
			for (int gy = 0; gy < 16; gy++)
				for (int gx = 0; gx < 8; gx++) {
					uint32_t c = (g[gy] & (0x80 >> gx)) ? 0xffd0d0d0u : 0xff101010u;
					con_fb[(row1 * 16 + gy) * (FB_STRIDE / 4) + col1 * 8 + gx] = c;
				}
		}
		if (++col1 >= 135) {
			col1 = 0;
			row1++;
		}
	}
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
	log_puts(label);
	log_putc(' ');
	log_puts(s);
	log_putc('\n');
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
	log_puts(label);
	log_putc(' ');
	log_puts(a);
	log_hex(va, 8);
	if (b) {
		log_putc(' ');
		log_puts(b);
		log_hex(vb, 8);
	}
	log_putc('\n');
}
