// ─────────────────────────────────────────────────────────────────────────────
// MeshProto.cpp  –  Minimal hand-rolled protobuf codec for Meshtastic
//
// Wire-format quick reference:
//   Tag   = (field_number << 3) | wire_type
//   Wire types: 0 = varint,  2 = length-delimited
//   Varint: 7 bits/byte, MSB=1 means another byte follows (little-endian bit order)
// ─────────────────────────────────────────────────────────────────────────────
#include "MeshProto.h"
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Write a varint into buf.  Returns bytes written.
static size_t writeVarint(uint8_t *buf, uint64_t val) {
    size_t n = 0;
    do {
        uint8_t b = val & 0x7Fu;
        val >>= 7;
        if (val) b |= 0x80u;
        buf[n++] = b;
    } while (val);
    return n;
}

// Read a varint from buf[0..len-1] into val.  Returns bytes consumed, 0 on error.
static size_t readVarint(const uint8_t *buf, size_t len, uint64_t &val) {
    val = 0;
    for (size_t i = 0; i < len && i < 10; i++) {
        val |= (uint64_t)(buf[i] & 0x7Fu) << (7 * i);
        if (!(buf[i] & 0x80u)) return i + 1;
    }
    return 0;  // truncated
}

// Write a varint field: tag + value.
static size_t writeVarField(uint8_t *buf, uint32_t field_no, uint64_t val) {
    size_t n = 0;
    n += writeVarint(buf + n, ((uint64_t)field_no << 3) | 0u);  // wire type 0
    n += writeVarint(buf + n, val);
    return n;
}

// Write a length-delimited field: tag + length + data.
static size_t writeLenField(uint8_t *buf, uint32_t field_no,
                             const uint8_t *data, size_t data_len) {
    size_t n = 0;
    n += writeVarint(buf + n, ((uint64_t)field_no << 3) | 2u);  // wire type 2
    n += writeVarint(buf + n, data_len);
    memcpy(buf + n, data, data_len);
    n += data_len;
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoding
// ─────────────────────────────────────────────────────────────────────────────

// Encode  Data { portnum, payload }  → buf.  Returns bytes written.
static size_t encodeData(const MeshData &d, uint8_t *buf, size_t buf_size) {
    size_t n = 0;
    n += writeVarField(buf + n, 1, (uint64_t)d.portnum);           // field 1: portnum
    if (d.payload_len > 0)
        n += writeLenField(buf + n, 2, d.payload, d.payload_len);  // field 2: payload
    return n;
}

// Encode  MeshPacket { from, to, channel, decoded, id, hop_limit }  → buf.
static size_t encodeMeshPacket(const MeshPacket &mp, uint8_t *buf, size_t buf_size) {
    size_t n = 0;
    n += writeVarField(buf + n, 1, mp.from_id);    // field 1: from
    n += writeVarField(buf + n, 2, mp.to_id);      // field 2: to
    n += writeVarField(buf + n, 3, mp.channel);    // field 3: channel

    // field 4: decoded  (Data sub-message, length-delimited)
    uint8_t data_buf[280];
    size_t  data_len = encodeData(mp.decoded, data_buf, sizeof(data_buf));
    n += writeLenField(buf + n, 4, data_buf, data_len);

    n += writeVarField(buf + n, 6, mp.packet_id);  // field 6: id
    n += writeVarField(buf + n, 9, mp.hop_limit);  // field 9: hop_limit
    return n;
}

namespace MeshProto {

size_t encodeToRadio(const MeshPacket &mp, uint8_t *buf, size_t buf_size) {
    uint8_t pkt_buf[512];
    size_t  pkt_len = encodeMeshPacket(mp, pkt_buf, sizeof(pkt_buf));
    if (pkt_len == 0) return 0;

    size_t needed = 1 + 5 + pkt_len;  // tag (1) + varint len (max 5) + data
    if (needed > buf_size) return 0;

    // ToRadio { packet = field 1, length-delimited }
    return writeLenField(buf, 1, pkt_buf, pkt_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Decoding  (two-level: FromRadio → MeshPacket → Data)
// ─────────────────────────────────────────────────────────────────────────────

// Generic: skip over any field whose wire type we already know.
// Returns bytes to skip past the value (NOT including the already-read tag).
static size_t skipField(const uint8_t *buf, size_t len, uint32_t wire_type) {
    if (wire_type == 0) {          // varint – consume bytes until MSB=0
        for (size_t i = 0; i < len; i++)
            if (!(buf[i] & 0x80u)) return i + 1;
        return 0;
    }
    if (wire_type == 2) {          // length-delimited – read length then skip
        uint64_t flen = 0;
        size_t adv = readVarint(buf, len, flen);
        if (!adv) return 0;
        return adv + (size_t)flen;
    }
    return 0;  // wire types 1,3,4,5 are not used by Meshtastic protobufs we care about
}

// Decode a Data sub-message from buf[0..len-1] into d.
static void decodeData(const uint8_t *buf, size_t len, MeshData &d) {
    size_t i = 0;
    while (i < len) {
        uint64_t tag_raw = 0;
        size_t adv = readVarint(buf + i, len - i, tag_raw);
        if (!adv) break;
        i += adv;

        uint32_t fn = (uint32_t)(tag_raw >> 3);
        uint32_t wt = (uint32_t)(tag_raw & 7u);

        if (wt == 0) {
            uint64_t v = 0;
            adv = readVarint(buf + i, len - i, v);
            if (!adv) break;
            i += adv;
            if (fn == 1) d.portnum = (PortNum)v;
        } else if (wt == 2) {
            uint64_t flen = 0;
            adv = readVarint(buf + i, len - i, flen);
            if (!adv) break;
            i += adv;
            if (fn == 2) {  // payload
                uint16_t copy = (flen > sizeof(d.payload)) ? sizeof(d.payload) : (uint16_t)flen;
                memcpy(d.payload, buf + i, copy);
                d.payload_len = copy;
            }
            i += (size_t)flen;
        } else {
            size_t skip = skipField(buf + i, len - i, wt);
            if (!skip) break;
            i += skip;
        }
    }
}

// Decode a MeshPacket sub-message from buf[0..len-1] into mp.
static void decodeMeshPacket(const uint8_t *buf, size_t len, MeshPacket &mp) {
    size_t i = 0;
    while (i < len) {
        uint64_t tag_raw = 0;
        size_t adv = readVarint(buf + i, len - i, tag_raw);
        if (!adv) break;
        i += adv;

        uint32_t fn = (uint32_t)(tag_raw >> 3);
        uint32_t wt = (uint32_t)(tag_raw & 7u);

        if (wt == 0) {
            uint64_t v = 0;
            adv = readVarint(buf + i, len - i, v);
            if (!adv) break;
            i += adv;
            switch (fn) {
                case 1: mp.from_id   = (uint32_t)v; break;
                case 2: mp.to_id     = (uint32_t)v; break;
                case 3: mp.channel   = (uint32_t)v; break;
                case 6: mp.packet_id = (uint32_t)v; break;
                case 9: mp.hop_limit = (uint8_t)v;  break;
                default: break;
            }
        } else if (wt == 2) {
            uint64_t flen = 0;
            adv = readVarint(buf + i, len - i, flen);
            if (!adv) break;
            if (fn == 4) decodeData(buf + i + adv, (size_t)flen, mp.decoded);
            i += adv + (size_t)flen;
        } else {
            size_t skip = skipField(buf + i, len - i, wt);
            if (!skip) break;
            i += skip;
        }
    }
}

bool decodeFromRadio(const uint8_t *buf, size_t len, MeshPacket &out) {
    memset(&out, 0, sizeof(out));
    out.to_id = 0xFFFFFFFFu;

    size_t i = 0;
    while (i < len) {
        uint64_t tag_raw = 0;
        size_t adv = readVarint(buf + i, len - i, tag_raw);
        if (!adv) break;
        i += adv;

        uint32_t fn = (uint32_t)(tag_raw >> 3);
        uint32_t wt = (uint32_t)(tag_raw & 7u);

        if (wt == 0) {
            uint64_t v = 0;
            adv = readVarint(buf + i, len - i, v);
            if (!adv) break;
            i += adv;
            // field 1 = num (sequence counter) – we ignore it
        } else if (wt == 2) {
            uint64_t flen = 0;
            adv = readVarint(buf + i, len - i, flen);
            if (!adv) break;
            if (fn == 2) {  // field 2 = MeshPacket
                decodeMeshPacket(buf + i + adv, (size_t)flen, out);
                return out.decoded.portnum == PortNum::TEXT_MESSAGE
                    && out.decoded.payload_len > 0;
            }
            i += adv + (size_t)flen;
        } else {
            size_t skip = skipField(buf + i, len - i, wt);
            if (!skip) break;
            i += skip;
        }
    }
    return false;
}

}  // namespace MeshProto
