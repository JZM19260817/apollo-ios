#ifndef RPMH_H
#define RPMH_H

/* Vote the PM8150L L11C rail on through RPMh (0 = success). */
int rpmh_ldo11c_on(unsigned mv);

#endif
