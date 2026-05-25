/*
   SipHash reference C implementation

   Copyright (c) 2012-2021 Jean-Philippe Aumasson
   <jeanphilippe.aumasson@gmail.com>
   Copyright (c) 2012-2014 Daniel J. Bernstein <djb@cr.yp.to>

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along
   with
   this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.

   Source: https://github.com/veorq/SipHash.git
 */

#ifndef HALFSIPHASH_H
#define HALFSIPHASH_H

#define HALFSIPHASH_KEY_SZ 8
#define HALFSIPHASH_HSH_SZ 4

/*
 * *in:    pointer to input data (read-only)
 * inlen:  input data length in bytes
 * *k:     pointer to the key data (read-only), must be HALFSIPHASH_KEY_SZ bytes
 * *out:   pointer to output data (write-only), must be HALFSIPHASH_HSH_SZ bytes
 */
int halfsiphash(const unsigned char *in, unsigned int inlen, const unsigned char *k, unsigned char *out);

#endif /* HALFSIPHASH_H */
