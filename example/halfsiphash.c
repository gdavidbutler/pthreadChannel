/*
   SipHash reference C implementation

   Copyright (c) 2016 Jean-Philippe Aumasson <jeanphilippe.aumasson@gmail.com>

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along
   with
   this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.

   Source: https://github.com/veorq/SipHash.git
 */
#include "halfsiphash.h"

typedef unsigned int halfsiphash_bt; /* unsigned 32 bits */

/* default: SipHash-2-4 */
#ifndef cROUNDS
#define cROUNDS 2
#endif
#ifndef dROUNDS
#define dROUNDS 4
#endif

#define ROTL(x, b) (halfsiphash_bt)(((x) << (b)) | ((x) >> (32 - (b))))

#define U32TO8_LE(p, v)                  \
  (p)[0] = (unsigned char)((v));         \
  (p)[1] = (unsigned char)((v) >>  8);   \
  (p)[2] = (unsigned char)((v) >> 16);   \
  (p)[3] = (unsigned char)((v) >> 24);

#define U8TO32_LE(p)                                  \
  (((halfsiphash_bt)((p)[0]))                         \
 | ((halfsiphash_bt)((p)[1]) <<  8)                   \
 | ((halfsiphash_bt)((p)[2]) << 16)                   \
 | ((halfsiphash_bt)((p)[3]) << 24))

#define SIPROUND        \
  do {                  \
    v0 += v1;           \
    v1 = ROTL(v1,  5);  \
    v1 ^= v0;           \
    v0 = ROTL(v0, 16);  \
    v2 += v3;           \
    v3 = ROTL(v3,  8);  \
    v3 ^= v2;           \
    v0 += v3;           \
    v3 = ROTL(v3,  7);  \
    v3 ^= v0;           \
    v2 += v1;           \
    v1 = ROTL(v1, 13);  \
    v1 ^= v2;           \
    v2 = ROTL(v2, 16);  \
  } while (0)

int
halfsiphash(
  const unsigned char *in
 ,unsigned int inlen
 ,const unsigned char *k
 ,unsigned char *out
){
  const unsigned char *end;
  halfsiphash_bt v0 = 0;
  halfsiphash_bt v1 = 0;
  halfsiphash_bt v2 = 0x6c796765U;
  halfsiphash_bt v3 = 0x74656462U;
  halfsiphash_bt k0;
  halfsiphash_bt k1;
  halfsiphash_bt m;
  halfsiphash_bt b;
  unsigned int left;
  int i;

  k0 = U8TO32_LE(k);
  k1 = U8TO32_LE(k + 4);
  end = in + inlen - (inlen % sizeof (halfsiphash_bt));
  left = inlen & 3;
  b = ((halfsiphash_bt)inlen) << 24;

  v3 ^= k1;
  v2 ^= k0;
  v1 ^= k1;
  v0 ^= k0;

  for (; in != end; in += 4) {
    m = U8TO32_LE(in);
    v3 ^= m;
    for (i = 0; i < cROUNDS; ++i)
      SIPROUND;
    v0 ^= m;
  }

  switch (left) {
  case 3:
    b |= ((halfsiphash_bt)in[2]) << 16;
    /* FALLTHRU */
  case 2:
    b |= ((halfsiphash_bt)in[1]) << 8;
    /* FALLTHRU */
  case 1:
    b |= ((halfsiphash_bt)in[0]);
    break;
  case 0:
  default:
    break;
  }

  v3 ^= b;
  for (i = 0; i < cROUNDS; ++i)
    SIPROUND;
  v0 ^= b;

  v2 ^= 0xff;

  for (i = 0; i < dROUNDS; ++i)
    SIPROUND;

  b = v1 ^ v3;
  U32TO8_LE(out, b);

  return 0;
}
