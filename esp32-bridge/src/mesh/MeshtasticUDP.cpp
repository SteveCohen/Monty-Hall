// ─────────────────────────────────────────────────────────────────────────────
// MeshtasticUDP.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "MeshtasticUDP.h"
#include "proto/MeshProto.h"
#include "config.h"
#include <WiFi.h>
#include <Arduino.h>

MeshtasticUDP &MeshtasticUDP::instance() {
    static MeshtasticUDP inst;
    return inst;
}

bool MeshtasticUDP::init() {
    // Tear down any stale socket from a previous WiFi session.
    _udp.stop();
    _ready = false;

    IPAddress mcast;
    if (!mcast.fromString(MESH_MCAST_ADDR)) {
        Serial.println("[UDP] Bad multicast address in config");
        return false;
    }

    if (_udp.beginMulticast(mcast, MESH_UDP_PORT)) {
        _ready = true;
        Serial.printf("[UDP] Joined %s:%d\n", MESH_MCAST_ADDR, MESH_UDP_PORT);
    } else {
        Serial.println("[UDP] beginMulticast() failed");
    }
    return _ready;
}

bool MeshtasticUDP::isAvailable() const {
    return _ready && (WiFi.status() == WL_CONNECTED);
}

bool MeshtasticUDP::sendPacket(const uint8_t *buf, uint16_t len) {
    if (!isAvailable()) return false;

    IPAddress dest;
    dest.fromString(MESH_MCAST_ADDR);
    _udp.beginPacket(dest, MESH_UDP_PORT);
    _udp.write(buf, len);
    bool ok = (_udp.endPacket() == 1);
    if (!ok) Serial.println("[UDP] endPacket() failed");
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Receive
// ─────────────────────────────────────────────────────────────────────────────
void MeshtasticUDP::pollReceive(MessageBridge &bridge) {
    int pkt_size = _udp.parsePacket();
    if (pkt_size <= 0 || pkt_size > 512) return;

    uint8_t buf[512];
    int n = _udp.read(buf, sizeof(buf));
    if (n <= 0) return;

    // Ignore packets we sent ourselves (source == our IP).
    // WiFiUDP doesn't echo multicast back by default on ESP32, but guard anyway.
    if (_udp.remoteIP() == WiFi.localIP()) return;

    MeshPacket mp{};
    if (!MeshProto::decodeFromRadio(buf, (size_t)n, mp)) return;

    Serial.printf("[UDP] RX from 0x%08X: %.*s\n",
                  mp.from_id,
                  mp.decoded.payload_len,
                  (char *)mp.decoded.payload);

    bridge.onMeshPacket(mp);
}

// ─────────────────────────────────────────────────────────────────────────────
// FreeRTOS task
// ─────────────────────────────────────────────────────────────────────────────
void MeshtasticUDP::udpRxTask(void *arg) {
    auto &self   = MeshtasticUDP::instance();
    auto &bridge = MessageBridge::instance();

    for (;;) {
        if (self.isAvailable())
            self.pollReceive(bridge);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
