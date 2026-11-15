/*
 * Battery probe: which SPMI slave actually answers on the charger registers?
 *
 * The driver reads the SMB5 charger and the QG from SID 0 (plain PM8150) and the
 * phone reports itself charging forever; apollo's device tree puts both on
 * qcom,pm8150b@2. Rather than guess again, read the same registers from every
 * SID and print what comes back.
 */
#include <stdint.h>
#include "console.h"
#include "spmi.h"

static const uint16_t probe_addrs[] = {0x1310, 0x110b, 0x1006, 0x1007,
	0x100d, 0x4804, 0x4808};
static const char probe_names[7][8] = {"usb", "path", "chgr", "err", "temp",
	"qg type", "qg pres"};

void test_touch(void)
{
	static const uint8_t sids[2] = {0, 2};	/* PM8150, PM8150B only */
	uint8_t v;

	con_clear(0x00aa0000u);
	con_str("BAT", "SPMI charger probe");
	con_hex("spmi ver", spmi_version());

	/*
	 * Only the two SIDs apollo's device tree declares are touched. Reading a
	 * slave that is not there wedged the PMIC arbiter last time and put the
	 * phone in a reboot loop; a marker goes out before every read so a hang
	 * also says which one it was.
	 */
	for (unsigned s = 0; s < 2; s++) {
		uint8_t sid = sids[s];
		char label[6];

		label[0] = 's';
		label[1] = 'i';
		label[2] = 'd';
		label[3] = ' ';
		label[4] = (char)('0' + sid);
		label[5] = 0;
		con_str("--", label);

		for (unsigned i = 0; i < 7; i++) {
			uint16_t a = probe_addrs[i];
			uint8_t per = (uint8_t)(a >> 8);
			uint8_t reg = (uint8_t)(a & 0xff);
			int rc;

			con_str("reading", probe_names[i]);
			rc = spmi_read(sid, per, reg, &v, 1);
			if (rc == 0) {
				con_hex2(probe_names[i], "v", v, "a", a);
			} else {
				con_hex(probe_names[i], (uint32_t)rc | 0x1000u);
			}
		}
	}

	con_str("BAT", "probe done, halted");
}
