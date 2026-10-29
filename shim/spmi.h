#ifndef SPMI_H
#define SPMI_H

#include <stdint.h>

/* Read-only SPMI access through the PMIC arbiter. */
uint32_t spmi_version(void);
int spmi_find_apid(uint32_t ppid, uint32_t *out);
int spmi_read(uint8_t sid, uint8_t periph, uint8_t reg, uint8_t *buf, unsigned len);

/* One-shot diagnostic: version, APID lookup, revision registers. */
void spmi_probe(void);

#endif /* SPMI_H */
