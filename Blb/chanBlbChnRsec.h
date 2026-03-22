/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2025 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of pthreadChannel
 *
 * pthreadChannel is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pthreadChannel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __CHANBLBCHNRSEC_H__
#define __CHANBLBCHNRSEC_H__

/* This multiplexer / demultiplexer depends on datagram semantics (write boundaries are preserved) */

struct chanBlbChnRsecCtx {
  void *hmacCtx;
  /* hdr points to fragment header ([addrlen][addr][tag][k-1][m][si]) for key selection */
  void (*hmacSign)(void *hmacCtx, const unsigned char *hdr, unsigned char *dst, const unsigned char *src, unsigned int len);
  int (*hmacVrfy)(void *hmacCtx, const unsigned char *hdr, const unsigned char *mac, const unsigned char *src, unsigned int len);
  void *cryptCtx;
  /* hdr as above; len excludes padding on last data shard (OTP safety) */
  void (*encrypt)(void *cryptCtx, const unsigned char *hdr, unsigned char *data, unsigned int len);
  void (*decrypt)(void *cryptCtx, const unsigned char *hdr, unsigned char *data, unsigned int len);
  /*
   * Max datagram payload size (transport MTU minus headers).
   * Max shard size (dgramMax - tagSize - hmacSize - 8) must not
   * exceed 16384; chanBlbChnRsecShard() returns 0 if violated.
   *
   * Wire format packs multiple fragments per datagram:
   *   [frag1][frag2]...[fragN][smallhash(1)]
   * Each fragment:
   *   [tag][k-1][m][si][pad_vlq(1-2)][shard_len_vlq(1-2)][shard_data][hmac]
   *
   * WARNING: Datagrams exceeding the path MTU are IP-fragmented;
   * loss of any IP fragment drops the entire datagram, defeating
   * erasure coding at this layer.  For best results do not exceed
   * the expected minimum MTU minus IP and UDP headers
   * (e.g. IPv4 Internet: 576 - 60 - 8 = 508).
   */
  unsigned int dgramMax;
  unsigned int tagSize;   /* correlation tag size in bytes */
  unsigned int tableSize; /* max in-flight sequences (ingress and egress) */
  unsigned int hmacSize;  /* HMAC size in bytes (0 = no HMAC) */
  unsigned int egrMsg;    /* egress: messages sent */
  unsigned int egrFrg;    /* egress: fragments sent */
  unsigned int igrFrg;    /* ingress: fragments received */
  unsigned int igrHash;   /* ingress: small hash failures */
  unsigned int igrHmac;   /* ingress: HMAC failures */
  unsigned int igrDup;    /* ingress: duplicate fragments */
  unsigned int igrLate;   /* ingress: late fragments */
  unsigned int igrMsg;    /* ingress: messages delivered */
  unsigned int igrDcd;    /* ingress: messages via RS decode */
  unsigned int igrLost;   /* ingress: incomplete messages evicted */
};

/* Return shard size in bytes (0 if dgramMax too small or too large) */
unsigned int
chanBlbChnRsecShard(
  const struct chanBlbChnRsecCtx *ctx
);

/* Return max payload bytes for a given m parity count (0 if invalid config) */
unsigned int
chanBlbChnRsecMax(
  const struct chanBlbChnRsecCtx *ctx
 ,unsigned char m
);

/* Egress blob: [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_ms(1)][payload(N)] */
/* delay_ms: inter-shard pacing delay in milliseconds (0 = send all at once) */
void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
);

/* Ingress blob: [addrlen(1)][addr(addrlen)][tag(tagSize)][rm(1)][um(1)][payload(N)] */
/* rm: received m. um: used m. Lost: rm=0 um=received, zero-length payload. */
void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
);

#endif /* __CHANBLBCHNRSEC_H__ */
