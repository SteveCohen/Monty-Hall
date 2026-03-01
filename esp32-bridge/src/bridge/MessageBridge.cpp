// ─────────────────────────────────────────────────────────────────────────────
// MessageBridge.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "MessageBridge.h"
#include <Arduino.h>

MessageBridge &MessageBridge::instance() {
    static MessageBridge inst;
    return inst;
}

void MessageBridge::init() {
    _dedup_mtx = xSemaphoreCreateMutex();
    memset(_dedup, 0, sizeof(_dedup));

    bleOutQueue  = xQueueCreate(BRIDGE_QUEUE_LEN, sizeof(BridgeMsg *));
    meshOutQueue = xQueueCreate(BRIDGE_QUEUE_LEN, sizeof(BridgeMsg *));

    Serial.println("[Bridge] Initialized (queues + dedup cache)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Deduplication
// ─────────────────────────────────────────────────────────────────────────────

bool MessageBridge::isDuplicate(const uint8_t *id16) {
    xSemaphoreTake(_dedup_mtx, portMAX_DELAY);
    size_t count = _dedup_full ? DEDUP_CACHE_SIZE : _dedup_head;
    bool found = false;
    for (size_t i = 0; i < count && !found; i++)
        found = (memcmp(_dedup[i], id16, 16) == 0);
    xSemaphoreGive(_dedup_mtx);
    return found;
}

void MessageBridge::markSeen(const uint8_t *id16) {
    xSemaphoreTake(_dedup_mtx, portMAX_DELAY);
    memcpy(_dedup[_dedup_head], id16, 16);
    _dedup_head = (_dedup_head + 1) % DEDUP_CACHE_SIZE;
    if (_dedup_head == 0) _dedup_full = true;
    xSemaphoreGive(_dedup_mtx);
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE → Meshtastic
// ─────────────────────────────────────────────────────────────────────────────

void MessageBridge::onBLEPacket(const BitchatPacket &pkt) {
    if (pkt.type != BITCHAT_TYPE_TEXT) return;
    if (isDuplicate(pkt.id)) return;
    markSeen(pkt.id);

    MeshPacket mp;
    if (!bitchatToMesh(pkt, mp)) {
        Serial.println("[Bridge] BLE→Mesh translation failed");
        return;
    }

    BridgeMsg *msg = encodeMeshMsg(mp);
    if (!msg) return;

    if (xQueueSend(meshOutQueue, &msg, 0) != pdTRUE) {
        Serial.println("[Bridge] meshOutQueue full – dropping");
        free(msg->data);
        delete msg;
    }
}

bool MessageBridge::bitchatToMesh(const BitchatPacket &bc, MeshPacket &out) {
    // Minimum text payload: 6 (sender) + 4 (ts) + 1 (ch_len) = 11 bytes
    if (bc.payload_len < 11) return false;

    uint8_t ch_len    = bc.payload[10];
    uint16_t txt_off  = 11u + ch_len;
    if (txt_off >= bc.payload_len) return false;

    uint16_t txt_len  = bc.payload_len - txt_off;
    if (txt_len > sizeof(out.decoded.payload)) return false;

    out.from_id   = NODE_ID;
    out.to_id     = 0xFFFFFFFFu;
    out.packet_id = ++_seq;
    out.channel   = MESH_CHANNEL;
    out.hop_limit = 3;

    out.decoded.portnum = PortNum::TEXT_MESSAGE;
    memcpy(out.decoded.payload, bc.payload + txt_off, txt_len);
    out.decoded.payload_len = txt_len;

    Serial.printf("[Bridge] BLE→Mesh  seq=%u  %.*s\n",
                  out.packet_id, txt_len, (char *)out.decoded.payload);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Meshtastic → BLE
// ─────────────────────────────────────────────────────────────────────────────

void MessageBridge::onMeshPacket(const MeshPacket &pkt) {
    if (pkt.decoded.portnum != PortNum::TEXT_MESSAGE) return;
    if (pkt.decoded.payload_len == 0)                 return;

    // Build a synthetic 16-byte dedup ID:  packet_id (4) + from_id (4) + 0xBEEF marker
    uint8_t dedup_id[16] = {};
    memcpy(dedup_id,      &pkt.packet_id, 4);
    memcpy(dedup_id + 4,  &pkt.from_id,  4);
    dedup_id[8] = 0xBE; dedup_id[9] = 0xEF;

    if (isDuplicate(dedup_id)) return;
    markSeen(dedup_id);

    BitchatPacket bc;
    if (!meshToBitchat(pkt, bc)) {
        Serial.println("[Bridge] Mesh→BLE translation failed");
        return;
    }

    BridgeMsg *msg = encodeBLEMsg(bc);
    if (!msg) return;

    if (xQueueSend(bleOutQueue, &msg, 0) != pdTRUE) {
        Serial.println("[Bridge] bleOutQueue full – dropping");
        free(msg->data);
        delete msg;
    }
}

bool MessageBridge::meshToBitchat(const MeshPacket &mp, BitchatPacket &out) {
    if (mp.decoded.payload_len == 0 || mp.decoded.payload_len > 240) return false;

    out.type = BITCHAT_TYPE_TEXT;
    out.ttl  = MSG_TTL_INJECT;

    // Synthetic bitchat UUID from mesh packet_id + from_id
    memset(out.id, 0, 16);
    memcpy(out.id,     &mp.packet_id, 4);
    memcpy(out.id + 4, &mp.from_id,  4);
    out.id[8] = 0xBE; out.id[9] = 0xEF;

    // Build bitchat text payload:
    //   [0-5]  sender_id  (pad from_id into 6 bytes)
    //   [6-9]  timestamp  (uint32 LE)
    //   [10]   channel_len = 0  (use "default" channel)
    //   [11..] message text
    uint32_t ts = (uint32_t)(millis() / 1000u);
    uint8_t *p  = out.payload;

    memset(p, 0, 6);
    memcpy(p, &mp.from_id, 4);  // 4-byte node ID in the first 4 of 6
    p += 6;

    memcpy(p, &ts, 4); p += 4;
    *p++ = 0;  // channel_len = 0 → empty channel name → "default"

    memcpy(p, mp.decoded.payload, mp.decoded.payload_len);
    p += mp.decoded.payload_len;

    out.payload_len = (uint16_t)(p - out.payload);

    Serial.printf("[Bridge] Mesh→BLE  from=0x%08X  %.*s\n",
                  mp.from_id,
                  mp.decoded.payload_len,
                  (char *)mp.decoded.payload);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BridgeMsg factory helpers
// ─────────────────────────────────────────────────────────────────────────────

BridgeMsg *MessageBridge::encodeBLEMsg(const BitchatPacket &pkt) {
    uint16_t total = (uint16_t)(BITCHAT_HEADER_LEN + pkt.payload_len);
    uint8_t *buf   = (uint8_t *)malloc(total);
    if (!buf) return nullptr;

    buf[0] = pkt.type;
    buf[1] = pkt.ttl;
    memcpy(buf + 2,                 pkt.id,      16);
    memcpy(buf + BITCHAT_HEADER_LEN, pkt.payload, pkt.payload_len);

    return new BridgeMsg{buf, total};
}

BridgeMsg *MessageBridge::encodeMeshMsg(const MeshPacket &mp) {
    uint8_t *buf = (uint8_t *)malloc(512);
    if (!buf) return nullptr;

    size_t n = MeshProto::encodeToRadio(mp, buf, 512);
    if (n == 0) { free(buf); return nullptr; }

    return new BridgeMsg{buf, (uint16_t)n};
}
