#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MessageBridge.h  –  Core routing + translation layer
//
// Receives packets from either network, deduplicates them, translates the
// format, and places the result on the appropriate output queue.
//
// Data flows:
//   BLE  →  onBLEPacket()  →  bitchatToMesh()  →  meshOutQueue  →  Meshtastic
//   Mesh →  onMeshPacket() →  meshToBitchat()  →  bleOutQueue   →  BLE
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "proto/MeshProto.h"
#include "config.h"

// ── bitchat packet header constants ──────────────────────────────────────────
//
// Wire format of a raw bitchat BLE packet:
//   Byte 0     : type
//   Byte 1     : TTL  (decremented each hop; discard at 0)
//   Bytes 2-17 : message UUID (16 bytes, used for deduplication)
//   Bytes 18+  : type-specific payload
//
// Text payload (type 0x01):
//   Bytes  0-5  : sender_id  (6 bytes – usually BLE MAC-derived)
//   Bytes  6-9  : timestamp  (uint32 LE, Unix seconds)
//   Byte  10    : channel_len
//   Bytes 11..  : channel name (UTF-8, channel_len bytes)
//   Bytes ..    : message text (UTF-8, remainder)

#define BITCHAT_HEADER_LEN  18u   // type(1) + ttl(1) + id(16)
#define BITCHAT_TYPE_TEXT   0x01u
#define BITCHAT_TYPE_INFO   0x02u
#define BITCHAT_TYPE_ACK    0x03u

struct BitchatPacket {
    uint8_t  type;
    uint8_t  ttl;
    uint8_t  id[16];           // message UUID (dedup key)
    uint8_t  payload[512];
    uint16_t payload_len;
};

// ── Inter-task message carrier ────────────────────────────────────────────────
// Heap-allocated; the receiving task is responsible for calling free(data) and
// delete on the BridgeMsg pointer.
struct BridgeMsg {
    uint8_t  *data;
    uint16_t  len;
};

// ── MessageBridge singleton ───────────────────────────────────────────────────
class MessageBridge {
public:
    static MessageBridge &instance();

    // Must be called once before any tasks start.
    void init();

    // ── Inbound callbacks (called from BLE / Meshtastic tasks) ───────────────
    void onBLEPacket(const BitchatPacket &pkt);
    void onMeshPacket(const MeshPacket &pkt);

    // ── Output queues (consumed by BLE-tx and Mesh-tx tasks) ─────────────────
    QueueHandle_t bleOutQueue;   // BridgeMsg* → send over BLE
    QueueHandle_t meshOutQueue;  // BridgeMsg* → send over Meshtastic

private:
    MessageBridge() = default;

    // ── Deduplication ring buffer ─────────────────────────────────────────────
    bool isDuplicate(const uint8_t *id16);
    void markSeen(const uint8_t *id16);

    uint8_t           _dedup[DEDUP_CACHE_SIZE][16];
    uint8_t           _dedup_head = 0;
    bool              _dedup_full = false;
    SemaphoreHandle_t _dedup_mtx  = nullptr;

    // ── Translation helpers ───────────────────────────────────────────────────
    bool bitchatToMesh(const BitchatPacket &bc, MeshPacket &out);
    bool meshToBitchat(const MeshPacket &mp, BitchatPacket &out);

    // Encode a BitchatPacket into a heap-allocated BridgeMsg.
    static BridgeMsg *encodeBLEMsg(const BitchatPacket &pkt);

    // Encode a MeshPacket (ToRadio) into a heap-allocated BridgeMsg.
    static BridgeMsg *encodeMeshMsg(const MeshPacket &mp);

    // Monotonically increasing counter for synthetic outgoing packet IDs.
    uint32_t _seq = 0;
};
