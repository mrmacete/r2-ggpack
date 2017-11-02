/* MIT - Copyright 2017 - mrmacete */

#ifndef R_GGPACK_PASS2_H
#define R_GGPACK_PASS2_H

/*
 * This file collects all known obfuscation tables for pass2 xor.
 * To add a new one, use make_pass2_table.py script. Then just
 * include here the generated header, and add the new line to the
 * r_ggpack_pass2 table.
 */

#include "r_ggpack_pass2_t0.h"

#define R_GGPACK_PASS2_BRUTE 1
#define R_GGPACK_PASS2_WIDTH 4096
static const ut8 r_ggpack_pass2[R_GGPACK_PASS2_BRUTE][R_GGPACK_PASS2_WIDTH] = {
	R_GGPACK_XOR_PASS2_T0,
};

#endif
