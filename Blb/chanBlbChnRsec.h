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

/* Generated with Claude Code (https://claude.ai/code) */

#ifndef __CHANBLBCHNRSEC_H__
#define __CHANBLBCHNRSEC_H__

/* This multiplexer / demultiplexer depends on datagram semantics (write boundaries are preserved) */

/*
 * Egress and ingress are independent agents with independent contexts.
 * Each context is owned by a single thread; there is no cross-thread
 * sharing of callbacks, callback contexts, or counters.
 *
 * The wire-format-defining sizes (dgramMax, tagSize, hmacSize, hashSize)
 * MUST match between a sender's egress context and the corresponding
 * receiver's ingress context, and likewise within a single peer that
 * runs both directions.  tableSize is per-direction.
 *
 * The framer thread (chanBlbChnRsecEgr / chanBlbChnRsecIgr) owns the
 * private state's lifecycle: it allocates on entry using the chanBlb
 * realloc/free pair (`v->realloc`/`v->free`) and releases on exit.
 * The application supplies the ctx with config fields populated and
 * `opaque` zero (e.g. via calloc); no explicit init/fini calls.
 *
 * Two-layer authentication:
 *   - Datagram layer (hashSign/hashVrfy, called once per datagram):
 *     a keyed gate that lets the ingress reject adversarial floods
 *     before any per-fragment HMAC work runs.  `adr` points to the
 *     [addrlen][addr] prefix from the wire; on egress this is the
 *     destination address (chosen by the local application), on
 *     ingress this is the kernel-supplied source address (worthless
 *     against a spoofing adversary; advisory only).  The application
 *     decides what to do with `adr` — use it as a hint, ignore it,
 *     or trial-verify across a peer-key set.
 *   - Fragment layer (hmacSign/hmacVrfy, called once per fragment):
 *     end-to-end authentication of each shard, keyed by per-fragment
 *     header material.
 */

struct chanBlbChnRsecEgrCtrs {
  unsigned int msg;       /* messages fully sent */
  unsigned int frg;       /* fragments sent */
  unsigned int full;      /* messages blocked (table was full) */
};

struct chanBlbChnRsecIgrCtrs {
  unsigned int frg;       /* fragments received */
  unsigned int hash;      /* datagram MAC failures */
  unsigned int hmac;      /* HMAC failures */
  unsigned int dup;       /* duplicate fragments */
  unsigned int late;      /* late fragments after delivery */
  unsigned int msg;       /* messages delivered */
  unsigned int dcd;       /* messages via RS decode */
  unsigned int evict;     /* messages evicted (table was full) */
};

struct chanBlbChnRsecEgrCtx {
  void *hashCtx;
  /* adr points to [addrlen(1)][addr(addrlen)] of the destination */
  void (*hashSign)(void *hashCtx, const unsigned char *adr, unsigned char *dst, const unsigned char *src, unsigned int len);
  void *hmacCtx;
  /* hdr points to fragment header ([addrlen][addr][tag][k-1][m][si]) for key selection */
  void (*hmacSign)(void *hmacCtx, const unsigned char *hdr, unsigned char *dst, const unsigned char *src, unsigned int len);
  void *cryptCtx;
  /* hdr as above; len excludes padding on last data shard (OTP safety) */
  void (*encrypt)(void *cryptCtx, const unsigned char *hdr, unsigned char *data, unsigned int len);
  /*
   * Max datagram payload size (transport MTU minus headers).
   * Max shard size (dgramMax - tagSize - hmacSize - hashSize - 7)
   * must not exceed 16384; chanBlbChnRsecShard() returns 0 if violated.
   *
   * Wire format packs multiple fragments per datagram:
   *   [frag1][frag2]...[fragN][hash(hashSize)]
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
  /*
   * Correlation tag size in bytes.  The tag identifies which message
   * a fragment belongs to.  Tags MUST be unique across all messages
   * that may be in flight concurrently at the ingress.
   */
  unsigned int tagSize;
  unsigned int tableSize; /* max in-flight messages being paced */
  unsigned int hmacSize;  /* fragment HMAC size in bytes (0 = no fragment HMAC) */
  unsigned int hashSize;  /* datagram MAC size in bytes (0 = no datagram MAC) */
  void *opaque;           /* private: managed by chanBlbChnRsecEgr; init to 0 */
};

struct chanBlbChnRsecIgrCtx {
  void *hashCtx;
  /*
   * adr points to [addrlen(1)][addr(addrlen)] of the kernel-supplied
   * source address.  This address is attacker-controlled in any
   * threat model that includes adversarial peers — the callback
   * MUST NOT trust it as a key selector.  Use it as a routing hint,
   * ignore it, or trial-verify across a peer-key set.
   */
  int (*hashVrfy)(void *hashCtx, const unsigned char *adr, const unsigned char *mac, const unsigned char *src, unsigned int len);
  void *hmacCtx;
  /* hdr points to fragment header ([addrlen][addr][tag][k-1][m][si]) for key selection */
  int (*hmacVrfy)(void *hmacCtx, const unsigned char *hdr, const unsigned char *mac, const unsigned char *src, unsigned int len);
  void *cryptCtx;
  /* hdr as above; len excludes padding on last data shard (OTP safety) */
  void (*decrypt)(void *cryptCtx, const unsigned char *hdr, unsigned char *data, unsigned int len);
  unsigned int dgramMax;  /* must match the sender's egress dgramMax */
  unsigned int tagSize;   /* must match the sender's egress tagSize */
  /*
   * Max in-flight reassemblies.  If a fragment matches an existing
   * tag entry but with different RS parameters (k, m, shard size),
   * the existing entry is evicted (parameter mismatch) and its
   * collected shards are lost.  Callers that resend or re-encode a
   * message with different RS parameters MUST use a distinct tag
   * (e.g. by including a sequence byte) to avoid evicting the
   * original.
   */
  unsigned int tableSize;
  unsigned int hmacSize;  /* must match the sender's egress hmacSize */
  unsigned int hashSize;  /* must match the sender's egress hashSize */
  void *opaque;           /* private: managed by chanBlbChnRsecIgr; init to 0 */
};

/*
 * Coherent counter snapshot.  Copies the current counter values into
 * the caller-supplied destination under the private mutex.  Safe to
 * call from any thread.
 *
 * If the framer thread has not yet initialized the private state
 * (i.e. shortly after chanBlb() returns) or has already torn it down
 * (after the framer thread exits), `out` is filled with zeros.
 * Callers should snapshot before initiating chanShut.
 */
void chanBlbChnRsecEgrSnap(struct chanBlbChnRsecEgrCtx *, struct chanBlbChnRsecEgrCtrs *out);
void chanBlbChnRsecIgrSnap(struct chanBlbChnRsecIgrCtx *, struct chanBlbChnRsecIgrCtrs *out);

/* Return shard size in bytes (0 if dgramMax too small or too large) */
unsigned int
chanBlbChnRsecShard(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int hmacSize
 ,unsigned int hashSize
);

/* Return max payload bytes for a given m parity count (0 if invalid config) */
unsigned int
chanBlbChnRsecMax(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int hmacSize
 ,unsigned int hashSize
 ,unsigned char m
);

/* Egress blob: [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_ms(1)][payload(N)] */
/* delay_ms: inter-shard pacing delay in milliseconds (0 = send all at once) */
/* v->frmCtx must point to a struct chanBlbChnRsecEgrCtx with opaque zeroed */
void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
);

/* Ingress blob: [addrlen(1)][addr(addrlen)][tag(tagSize)][rm(1)][um(1)][payload(N)] */
/* rm: received m. um: used m. Lost: rm=0 um=received, zero-length payload. */
/* v->frmCtx must point to a struct chanBlbChnRsecIgrCtx with opaque zeroed */
void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
);

#endif /* __CHANBLBCHNRSEC_H__ */
