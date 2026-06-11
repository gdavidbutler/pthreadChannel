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
 * The wire-format-defining sizes (dgramMax, tagSize, frgHmacSize,
 * dtgHmacSize, dtgHashSize) MUST match between a sender's egress context
 * and the corresponding receiver's ingress context, and likewise within
 * a single peer that runs both directions.  tableSize is per-direction.
 *
 * The framer thread (chanBlbChnRsecEgr / chanBlbChnRsecIgr) owns the
 * private state's lifecycle: it allocates on entry using the chanBlb
 * realloc/free pair (`v->realloc`/`v->free`) and releases on exit.
 * The application supplies the ctx with config fields populated and
 * `opaque` zero (e.g. via calloc); no explicit init/fini calls.
 *
 * Three independent keyed tiers, each enabled by its own *Size > 0 and
 * each free to use a distinct key/callback:
 *
 *   - Datagram GATE  (dtgHashSign/dtgHashVrfy, dtgHashSize bytes/datagram):
 *     a cheap keyed MAC verified ONCE per datagram, before any other
 *     per-datagram or per-fragment work.  A flood that lacks the gate key
 *     is rejected at the cost of one (cheap, e.g. halfsiphash) MAC verify.
 *     Failure drops the whole datagram.  `adr` points to the
 *     [addrlen][addr] prefix from the wire (egress: chosen destination;
 *     ingress: kernel-supplied source, attacker-controlled, advisory only).
 *
 *   - Datagram AUTH  (dtgHmacSign/dtgHmacVrfy, dtgHmacSize bytes/datagram):
 *     a keyed MAC verified ONCE per datagram over EVERY fragment's bytes.
 *     Unlike the gate, the verify callback is also handed the packed
 *     fragment structure (`frgOff` = each fragment's tag offset within
 *     `src`, `frgCnt` = fragment count) so the application can enforce
 *     intra-datagram sender uniformity (every fragment's tag claims the
 *     same sender that keys the MAC) WITHOUT re-parsing the VLQ framing.
 *     With uniformity enforced, this tier safely REPLACES per-fragment
 *     AUTH: one MAC per datagram instead of one per fragment, still
 *     Byzantine-safe.  Failure drops the whole datagram.
 *
 *   - Fragment AUTH  (frgHmacSign/frgHmacVrfy, frgHmacSize bytes/fragment):
 *     a keyed MAC per fragment, keyed by per-fragment header material.
 *     End-to-end authentication of each shard with drop-one-on-failure
 *     granularity (a bad fragment is skipped, its datagram siblings are
 *     still processed).  Needed only when fragments of differing trust
 *     can share a datagram; redundant alongside datagram AUTH + uniformity.
 *
 * All tiers are optional and any mix is legal (e.g. all three active is
 * the defense-in-depth case).
 */

struct chanBlbChnRsecEgrCtrs {
  unsigned int msg;       /* messages fully sent */
  unsigned int frg;       /* fragments sent */
  unsigned int full;      /* messages blocked (table was full) */
};

struct chanBlbChnRsecIgrCtrs {
  unsigned int frg;       /* fragments received */
  unsigned int hash;      /* datagram GATE (dtgHash) failures */
  unsigned int dhmac;     /* datagram AUTH (dtgHmac) failures */
  unsigned int hmac;      /* fragment AUTH (frgHmac) failures */
  unsigned int dup;       /* duplicate fragments */
  unsigned int late;      /* late fragments after delivery */
  unsigned int msg;       /* messages delivered */
  unsigned int dcd;       /* messages via RS decode */
  unsigned int evict;     /* messages evicted (table was full) */
};

struct chanBlbChnRsecEgrCtx {
  void *dtgHashCtx;
  /* adr points to [addrlen(1)][addr(addrlen)] of the destination */
  void (*dtgHashSign)(void *dtgHashCtx, const unsigned char *adr, unsigned char *dst, const unsigned char *src, unsigned int len);
  void *dtgHmacCtx;
  /*
   * Datagram AUTH sign.  adr as above.  src/len cover every fragment's
   * bytes (the same region the gate covers, minus the dtgHmac/dtgHash
   * trailers).  frgOff[i] is the byte offset of fragment i's tag within
   * src (frgOff[0] == 0); frgCnt is the fragment count.  The sender may
   * read src + frgOff[i] to locate fragment i's tag (sender lives there).
   */
  void (*dtgHmacSign)(void *dtgHmacCtx, const unsigned char *adr, unsigned char *dst, const unsigned char *src, unsigned int len, const unsigned int *frgOff, unsigned int frgCnt);
  void *frgHmacCtx;
  /* hdr points to fragment header ([addrlen][addr][tag][k-1][m][si]) for key selection */
  void (*frgHmacSign)(void *frgHmacCtx, const unsigned char *hdr, unsigned char *dst, const unsigned char *src, unsigned int len);
  /*
   * Max datagram payload size (transport MTU minus headers).
   * Max shard size
   *   (dgramMax - tagSize - frgHmacSize - dtgHmacSize - dtgHashSize - 6)
   * must not exceed 16384; chanBlbChnRsecShard() returns 0 if violated.
   *
   * Wire format (what is actually transmitted) packs multiple
   * fragments per datagram:
   *   [frag1][frag2]...[fragN][dtgHmac(dtgHmacSize)][dtgHash(dtgHashSize)]
   * Each fragment:
   *   [tag][k-1][m][si][pad(1)][shard_len_vlq(1-2)][shard_data][frgHmac(frgHmacSize)]
   * Field encodings:
   *   k-1: data shard count minus 1 (so k in 1..256 fits one byte)
   *   m:   parity shard count, unbiased (0..255)
   *   si:  shard index, 0-based
   *   pad: padding bytes appended to the last data shard, one byte.
   *     padding = k*ceil(payloadLen/k) - payloadLen is always in
   *     [0, k-1] and k <= 256, so it never exceeds 255.
   *   shard_len_vlq: 1-2 byte variable-length quantity --
   *     byte0 & 0x80 == 0: 1-byte form, value = byte0 (0..127)
   *     byte0 & 0x80 != 0: 2-byte form,
   *       value = (((byte0 & 0x7f) << 7) | (byte1 & 0x7f)) + 1 (1..16384)
   *     The 2-byte form is BIASED BY -1 (it encodes value-1); that bias
   *     is what lets shard_len reach 16384.  The encoder always picks the
   *     1-byte form for value < 128, so the encoding is canonical.
   * The [addrlen][addr] prefix the framer reads (egress) or writes
   * (ingress) is a transport-local frame, NOT on the wire: the datagram
   * transport (chanBlbTrnFdDatagram) consumes it as the sendto()
   * destination and strips it before transmission, and re-derives it
   * from recvfrom() on receive.  It is therefore NOT covered by any MAC.
   * The trailing dtgHmac (datagram AUTH) covers every byte from the first
   * fragment through the last (absent when dtgHmacSize == 0).  The trailing
   * dtgHash (datagram GATE) covers every fragment byte AND the dtgHmac
   * (absent when dtgHashSize == 0) -- the gate is outermost, verified
   * first, and protects the auth MAC bytes too.  Each fragment's frgHmac
   * covers the bytes from tag through shard_data (the frgHmac excluded).
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
  unsigned int frgHmacSize; /* fragment AUTH size in bytes (0 = none) */
  unsigned int dtgHmacSize; /* datagram AUTH size in bytes (0 = none) */
  unsigned int dtgHashSize; /* datagram GATE size in bytes (0 = none) */
  void *opaque;           /* private: managed by chanBlbChnRsecEgr; init to 0 */
};

struct chanBlbChnRsecIgrCtx {
  void *dtgHashCtx;
  /*
   * adr points to [addrlen(1)][addr(addrlen)] of the kernel-supplied
   * source address.  This address is attacker-controlled in any
   * threat model that includes adversarial peers — the callback
   * MUST NOT trust it as a key selector.  Use it as a routing hint,
   * ignore it, or trial-verify across a peer-key set.
   */
  int (*dtgHashVrfy)(void *dtgHashCtx, const unsigned char *adr, const unsigned char *mac, const unsigned char *src, unsigned int len);
  void *dtgHmacCtx;
  /*
   * Datagram AUTH verify.  adr as above (untrusted).  src/len cover every
   * fragment's bytes; mac points to the dtgHmac on the wire.  frgOff[i] is
   * the byte offset of fragment i's tag within src (frgOff[0] == 0);
   * frgCnt is the fragment count.  The callback verifies the MAC over
   * [src,len] AND should enforce sender uniformity: read each fragment's
   * tag at src + frgOff[i] and reject (return 0) if any claims a sender
   * other than the one keying the MAC.  Returns non-zero on accept.
   */
  int (*dtgHmacVrfy)(void *dtgHmacCtx, const unsigned char *adr, const unsigned char *mac, const unsigned char *src, unsigned int len, const unsigned int *frgOff, unsigned int frgCnt);
  void *frgHmacCtx;
  /* hdr points to fragment header ([addrlen][addr][tag][k-1][m][si]) for key selection */
  int (*frgHmacVrfy)(void *frgHmacCtx, const unsigned char *hdr, const unsigned char *mac, const unsigned char *src, unsigned int len);
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
  unsigned int frgHmacSize; /* must match the sender's egress frgHmacSize */
  unsigned int dtgHmacSize; /* must match the sender's egress dtgHmacSize */
  unsigned int dtgHashSize; /* must match the sender's egress dtgHashSize */
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
 ,unsigned int frgHmacSize
 ,unsigned int dtgHmacSize
 ,unsigned int dtgHashSize
);

/* Return max payload bytes for a given m parity count (0 if invalid config) */
unsigned int
chanBlbChnRsecMax(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int frgHmacSize
 ,unsigned int dtgHmacSize
 ,unsigned int dtgHashSize
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
