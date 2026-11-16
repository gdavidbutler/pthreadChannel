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
 * Messages over datagrams, each message k data shards and m Reed-Solomon
 * parity shards, any k of which deliver it.
 *
 * Egress and ingress are independent agents with independent contexts.
 * Each context is owned by a single thread; there is no cross-thread
 * sharing of callbacks, callback contexts, or counters.
 *
 * The wire-format sizes (dgramMax, tagSize, dtgHashSize) MUST match
 * between a sender's egress context and the receiver's ingress context.
 * tableSize is per direction.
 *
 * The framer thread (chanBlbChnRsecEgr / chanBlbChnRsecIgr) owns the
 * private state's lifecycle: it allocates on entry using the chanBlb
 * realloc/free pair (`v->realloc`/`v->free`) and releases on exit.
 * The application supplies the ctx with config fields populated and
 * `opaque` zero (e.g. via calloc); no explicit init/fini calls.  A
 * configuration the framer cannot run (a size above 0 without its
 * callback, no table, a dgramMax no shard fits, tagOwner past the tag)
 * ends the framer thread at once, as an output or input failure.
 *
 * Wire format, one datagram:
 *   [frag1][frag2]...[fragN][dtgHash(dtgHashSize)]
 * One fragment:
 *   [tag][k-1][m][si][pad][shard_len_vlq(1-2)][shard_data]
 *   k-1  data shards less one, so k in 1..256 fits a byte
 *   m    parity shards, 0..255, k + m <= 256
 *   si   the shard's index, 0-based
 *   pad  bytes appended to the last data shard, k * ceil(L / k) - L,
 *        in [0, k - 1]
 *   shard_len_vlq  byte0 & 0x80 == 0: byte0 (0..127); else
 *        (((byte0 & 0x7f) << 7) | (byte1 & 0x7f)) + 1 (1..16384), the
 *        1-byte form always taken below 128
 * k and the shard's length are the message length's alone, and ingress
 * refuses any other: k is 1 for an empty message and else
 * ceil(L / chanBlbChnRsecShard), the shard ceil(L / k), so what an entry
 * and a decode cost is the length of a message, never a fragment's
 * choice.
 * dtgHash covers [frag1]..[fragN].
 *
 * Fragments of one TAG never share a datagram: one loss would take two
 * shards of one train.  Fragments of different tags to one address do,
 * as many as fit.
 *
 * An address is [addrlen(1)][addr(addrlen)], the transport's own frame:
 * the datagram transport (chanBlbTrnFdDatagram) takes it as the sendto()
 * destination and strips it, and writes the recvfrom() source on
 * receive.  It is never on the wire and under no MAC.
 *
 * The GATE (dtgHashSign/dtgHashVrfy, dtgHashSize bytes a datagram, 0
 * none): one keyed MAC a datagram over every fragment, verified before
 * any other work, a failure dropping the datagram.  Its strength is the
 * caller's: a cheap one (e.g. halfsiphash) turns a flood without the key
 * away at one verify; a strong one (e.g. an HMAC) says a holder of the
 * key framed these bytes.  Either way a copy of them passes: whether a
 * message is new is for the end that knows what it has taken.
 * On ingress adr is the kernel's source address, which whoever sent the
 * datagram chose: a verify that selects its key by it verifies under
 * that address's key, and one that selects its key by a tag byte names
 * that byte in tagOwner, and a fragment claiming any other sender than
 * the datagram's first is refused.
 *
 * WARNING: Datagrams exceeding the path MTU are IP-fragmented; loss of
 * any IP fragment drops the entire datagram, defeating erasure coding at
 * this layer.  For best results do not exceed the expected minimum MTU
 * minus IP and UDP headers (e.g. IPv4 Internet: 576 - 60 - 8 = 508).
 */

struct chanBlbChnRsecEgrCtrs {
  unsigned int msg;       /* messages whose every shard was sent */
  unsigned int frg;       /* fragments sent */
  unsigned int full;      /* messages held for a full table */
  unsigned int again;     /* messages held for one to the same address
                           * under the same tag still in the table */
};

struct chanBlbChnRsecIgrCtrs {
  unsigned int frg;       /* fragments parsed */
  unsigned int hash;      /* datagram GATE failures, a fragment claiming
                           * another sender among them */
  unsigned int shape;     /* fragments of a geometry no egress derives,
                           * or past msgMax; or malformed or cut, the
                           * rest of their datagram with them */
  unsigned int dup;       /* duplicate fragments */
  unsigned int late;      /* fragments after their message was delivered:
                           * its stragglers and their copies */
  unsigned int msg;       /* messages delivered */
  unsigned int dcd;       /* of those, by Reed-Solomon decode */
  unsigned int evict;     /* incomplete messages evicted, or with no
                           * memory to decode: a loss each */
  unsigned int evictFull; /* of those, by a new tag on a full table */
};

struct chanBlbChnRsecEgrCtx {
  void *dtgHashCtx;
  /* adr: the destination; src/len: every fragment's bytes */
  void (*dtgHashSign)(void *dtgHashCtx, const unsigned char *adr, unsigned char *dst, const unsigned char *src, unsigned int len);
  /* the wire payload a datagram may carry (the transport's MTU less its
   * headers) */
  unsigned int dgramMax;
  /* the correlation tag's bytes.  A tag names a message's bytes: a
   * re-send goes under its tag, a different message under a new one.
   * Different bytes under a tag an ingress still holds a train of
   * reassemble with that train, a message neither sender framed */
  unsigned int tagSize;
  /* messages in flight: a message holds its slot until its last shard
   * is sent, and a message to the same address under the same tag as
   * one still in the table is held until that one's slot frees -- a
   * second train under its tag would feed the one reassembly at the
   * ingress, two messages delivered as one */
  unsigned int tableSize;
  unsigned int dtgHashSize;
  void *opaque;           /* private: managed by chanBlbChnRsecEgr; init to 0 */
};

struct chanBlbChnRsecIgrCtx {
  void *dtgHashCtx;
  /* adr: the kernel's source; mac: the dtgHash on the wire; src/len:
   * every fragment's bytes; non-zero accepts */
  int (*dtgHashVrfy)(void *dtgHashCtx, const unsigned char *adr, const unsigned char *mac, const unsigned char *src, unsigned int len);
  unsigned int dgramMax;  /* must match the sender's egress dgramMax */
  unsigned int tagSize;   /* must match the sender's egress tagSize */
  /* 0, or the longest message a sender may frame: a fragment implying
   * more is refused before any entry */
  unsigned int msgMax;
  /*
   * Reassemblies in flight.  A delivered entry stays to eat its train's
   * stragglers until k + m distinct fragments of the train have come, a
   * copy counting toward nothing; the next fragment under its tag is a
   * new train, so a re-send under the same tag is delivered again once
   * the first train's fragments have all come -- until then its
   * fragments are the first train's stragglers.  A
   * new tag on a full table evicts first an entry whose train's
   * fragments have all come, which holds nothing, then the entry least
   * recently fed, a delivered one first, which costs nothing, then one
   * in flight; a fragment whose k, m, shard size or padding differ from
   * its tag's entry evicts that entry.  An evicted entry not yet
   * delivered is a loss.  No clock enters.
   */
  unsigned int tableSize;
  /*
   * 0, or 1 + the offset of the tag byte that names the fragment's
   * sender, the byte the gate selects its key by: every fragment of a
   * datagram must name the first's, and a new tag on a full table
   * evicts the entry least recently fed of the sender holding the most
   * entries, its own where it ties for the most, a delivered one first
   * as above.  A sender that opens reassemblies faster than it
   * completes them takes the table from nobody but itself.  An egress
   * packs any tags to one address together, so one feeding such an
   * ingress carries one sender's tags.
   */
  unsigned int tagOwner;
  unsigned int dtgHashSize; /* must match the sender's egress dtgHashSize */
  void *opaque;           /* private: managed by chanBlbChnRsecIgr; init to 0 */
};

/*
 * Coherent counter snapshot, from any thread at any time: the counters
 * as the framer last published them, before its last wait.  Zeros
 * before the framer thread starts and after it ends, however it ends:
 * snapshot before shutting to read its last counts.
 */
void chanBlbChnRsecEgrSnap(struct chanBlbChnRsecEgrCtx *, struct chanBlbChnRsecEgrCtrs *out);
void chanBlbChnRsecIgrSnap(struct chanBlbChnRsecIgrCtx *, struct chanBlbChnRsecIgrCtrs *out);

/* Shard bytes a datagram carries (0 if dgramMax is too small or the
 * shard would pass 16384) */
unsigned int
chanBlbChnRsecShard(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int dtgHashSize
);

/* Payload bytes a message can carry at m parity shards (0 as above) */
unsigned int
chanBlbChnRsecMax(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int dtgHashSize
 ,unsigned char m
);

/*
 * Egress blob: [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_us(4)][payload(N)]
 * delay_us, big endian, paces the message's shards: shard i is due
 * (i + 1) x delay_us after the message is taken (0 sends them at once).
 * After a datagram the next waits the SUM of the delays of the shards
 * it carried: a delay is a slice of wire time, and packing lets shards
 * ride early, never for free.  m is cut to 256 - k where k + m would
 * pass 256.  A blob malformed, past chanBlbChnRsecMax at m = 0, or
 * with no memory to take it is dropped.  Once the channel is shut, the
 * blobs it still holds are taken and every message taken is sent at
 * its pace before the framer ends, as a stream's close sends what was
 * written.
 * v->frmCtx must point to a struct chanBlbChnRsecEgrCtx with opaque zeroed
 */
void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
);

/*
 * Ingress blob:
 *   a message  [addrlen(1)][addr(addrlen)][tag(tagSize)][rm(1)][um(1)][payload(N)]:
 *              rm the sender's m, um the parity shards the decode used
 *   a loss     [addrlen][addr][tag][0][received]: rm 0 with um above 0,
 *              no payload, the fragments the evicted entry held
 * (rm 0 um 0 is a message: m 0, every data shard arrived.)  addr is the
 * source of the entry's first fragment.  A datagram longer than dgramMax
 * or shorter than one fragment is not of this wire: dropped, uncounted.
 * v->frmCtx must point to a struct chanBlbChnRsecIgrCtx with opaque zeroed
 */
void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
);

#endif /* __CHANBLBCHNRSEC_H__ */
