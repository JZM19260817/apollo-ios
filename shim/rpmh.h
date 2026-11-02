#ifndef RPMH_H
#define RPMH_H

/* Vote a rail on through RPMh; 0 = accepted. */
int rpmh_ldo11c_on(unsigned mv);

/* Command-db lookup: resolve a regulator name ("ldoc11") to its RPMh address. */
int rpmh_find(const char *name, uint32_t *address);

/* Send a write vote to an address resolved by rpmh_find(). */
int rpmh_send(uint32_t address, unsigned mv);

#endif
