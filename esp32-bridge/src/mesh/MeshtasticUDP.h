#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MeshtasticUDP.h  –  Primary Meshtastic transport over WiFi multicast UDP
//
// Meshtastic LAN UDP meshing (enabled in 2.6+ / network.enabled_protocols=1):
//   • Multicast group : 224.0.0.69
//   • Port            : 4403
//   • Payload         : raw protobuf  (FromRadio when received,
//                                     ToRadio when sent)
//
// The UDP RX loop is run from a dedicated FreeRTOS task (udpRxTask).
// Sending is called directly from the mesh-TX task.
// ─────────────────────────────────────────────────────────────────────────────

#include <WiFiUdp.h>
#include "MeshtasticIF.h"
#include "bridge/MessageBridge.h"

class MeshtasticUDP : public MeshtasticIF {
public:
    static MeshtasticUDP &instance();

    bool init()                                    override;
    bool sendPacket(const uint8_t *buf, uint16_t len) override;
    bool isAvailable() const                       override;

    // Call periodically from the UDP-RX task to drain incoming datagrams.
    void pollReceive(MessageBridge &bridge);

    // FreeRTOS task: calls pollReceive() in a tight loop.
    static void udpRxTask(void *arg);

private:
    MeshtasticUDP() = default;
    WiFiUDP _udp;
    bool    _ready = false;
};
