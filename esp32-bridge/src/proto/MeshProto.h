#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MeshProto.h  –  Hand-rolled minimal protobuf codec for Meshtastic messages.
//
// Only the fields the bridge actually uses are encoded/decoded.  No nanopb
// dependency – keeping the build simple for a first prototype.
//
// Relevant Meshtastic protobuf definitions (for reference):
//   https://github.com/meshtastic/protobufs/blob/master/meshtastic/mesh.proto
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>

// ── Meshtastic PortNum enum (partial) ────────────────────────────────────────
enum class PortNum : uint32_t {
    UNKNOWN         = 0,
    TEXT_MESSAGE    = 1,  // UTF-8 plaintext chat
    REMOTE_HARDWARE = 2,
    POSITION        = 3,
    NODEINFO        = 4,
    ROUTING         = 5,
    ADMIN           = 6,
};

// ── In-memory representations ─────────────────────────────────────────────────
struct MeshData {
    PortNum  portnum       = PortNum::UNKNOWN;
    uint8_t  payload[240] = {};
    uint16_t payload_len  = 0;
};

struct MeshPacket {
    uint32_t from_id    = 0;
    uint32_t to_id      = 0xFFFFFFFFu;  // broadcast by default
    uint32_t packet_id  = 0;
    uint32_t channel    = 0;
    uint8_t  hop_limit  = 3;
    MeshData decoded;
};

// ── Encode / Decode API ───────────────────────────────────────────────────────
namespace MeshProto {

// Encode a  ToRadio { packet: mp }  into buf.
// Returns number of bytes written, or 0 on error (buf too small).
size_t encodeToRadio(const MeshPacket &mp, uint8_t *buf, size_t buf_size);

// Decode a FromRadio protobuf from buf.
// Returns true and fills 'out' if a MeshPacket with TEXT_MESSAGE payload was found.
bool decodeFromRadio(const uint8_t *buf, size_t len, MeshPacket &out);

} // namespace MeshProto
