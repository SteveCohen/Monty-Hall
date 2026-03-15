// ─────────────────────────────────────────────────────────────────────────────
// main.cpp  –  ESP32 bitchat ↔ Meshtastic Bridge
//
// Architecture overview
// ─────────────────────────────────────────────────────────────────────────────
//
//  ┌──────────────┐       BLE (NimBLE)        ┌──────────────────┐
//  │  bitchat     │◄─────────────────────────►│ BitchatBLE       │
//  │  peers       │   GATT notify/write        │ (dual-role:      │
//  └──────────────┘   scan + connect           │  server+client)  │
//                                              └────────┬─────────┘
//                                                       │ BitchatPacket
//                                              ┌────────▼─────────┐
//                                              │ MessageBridge    │
//                                              │ (dedup + xlate)  │
//                                              └────────┬─────────┘
//                                                       │ MeshPacket (protobuf)
//                         ┌─────────────────────────────┼──────────────────────┐
//                         │                             │                      │
//                ┌────────▼────────┐          ┌─────────▼──────────┐          │
//                │ MeshtasticUDP   │          │ MeshtasticUART     │          │
//                │ (primary)       │          │ (fallback)         │          │
//                │ WiFi multicast  │          │ Serial2 UART       │          │
//                │ 224.0.0.69:4403 │          │ 0x94 0xC3 framing  │          │
//                └────────┬────────┘          └─────────┬──────────┘          │
//                         │                             │                      │
//                         ▼                             ▼                      │
//                   Meshtastic node              Meshtastic device             │
//                   (UDP LAN mesh)               (UART connected)             │
//                                                                              │
//  ─── FreeRTOS tasks ────────────────────────────────────────────────────────│
//  Core 0:  ble_task      – drains bleOutQueue, manages BLE connections       │
//  Core 1:  wifi_task     – connects WiFi, calls MeshtasticUDP::init()        │
//           udp_rx_task   – drains incoming UDP datagrams                     │
//           uart_rx_task  – drains incoming UART bytes (state machine)        │
//           mesh_tx_task  – drains meshOutQueue, sends via UDP or UART        │
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "ble/BitchatBLE.h"
#include "mesh/MeshtasticUDP.h"
#include "mesh/MeshtasticUART.h"
#include "bridge/MessageBridge.h"

// ─────────────────────────────────────────────────────────────────────────────
// WiFi management task (Core 1, lowest priority)
// Monitors connection state and initialises the UDP transport on connect.
// ─────────────────────────────────────────────────────────────────────────────
static void wifiTask(void * /*arg*/) {
    bool udpUp = false;

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] Connecting to \"%s\"...\n", WIFI_SSID);

    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!udpUp) {
                Serial.printf("[WiFi] Connected – IP: %s\n",
                              WiFi.localIP().toString().c_str());
                MeshtasticUDP::instance().init();
                udpUp = true;
            }
        } else {
            if (udpUp) {
                Serial.println("[WiFi] Lost connection");
                udpUp = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP RX task (Core 1)
// Polls the WiFiUdp socket and forwards decoded MeshPackets to the bridge.
// ─────────────────────────────────────────────────────────────────────────────
static void udpRxTask(void * /*arg*/) {
    auto &udp    = MeshtasticUDP::instance();
    auto &bridge = MessageBridge::instance();

    for (;;) {
        if (udp.isAvailable())
            udp.pollReceive(bridge);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mesh TX task (Core 1)
// Drains meshOutQueue; tries UDP first, falls back to UART if UDP is down.
// ─────────────────────────────────────────────────────────────────────────────
static void meshTxTask(void * /*arg*/) {
    auto &udp    = MeshtasticUDP::instance();
    auto &uart   = MeshtasticUART::instance();
    auto &bridge = MessageBridge::instance();

    for (;;) {
        BridgeMsg *msg = nullptr;
        if (xQueueReceive(bridge.meshOutQueue, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        bool sent = false;

        if (udp.isAvailable()) {
            sent = udp.sendPacket(msg->data, msg->len);
            if (sent) Serial.printf("[MeshTX] UDP  %u bytes\n", msg->len);
        }

        if (!sent && uart.isAvailable()) {
            sent = uart.sendPacket(msg->data, msg->len);
            if (sent) Serial.printf("[MeshTX] UART %u bytes\n", msg->len);
        }

        if (!sent)
            Serial.println("[MeshTX] Both transports unavailable – dropping");

        free(msg->data);
        delete msg;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println(  "║  ESP32 bitchat ↔ Meshtastic Bridge  ║");
    Serial.println(  "╚══════════════════════════════════════╝");
    Serial.printf("   Node name : %s\n", NODE_NAME);
    Serial.printf("   Node ID   : 0x%08X\n", NODE_ID);
    Serial.printf("   Free heap : %u bytes\n\n", ESP.getFreeHeap());

    // 1. Bridge core (queues + dedup) – must be first
    MessageBridge::instance().init();

    // 2. UART transport – always on, even without WiFi
    MeshtasticUART::instance().init();

    // 3. BLE (NimBLE init + advertise + scan)
    BitchatBLE::instance().init();

    // 4. FreeRTOS tasks
    //    Priorities: 5 = high (BLE + MeshTX), 4 = normal (RX tasks), 2 = low (WiFi mgmt)
    //    Stack sizes: BLE needs 8K for NimBLE connect/GATT; TX/RX tasks 6K for printf+protobuf.
    xTaskCreatePinnedToCore(BitchatBLE::bleTask,         "ble",      8192, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(wifiTask,                    "wifi",     4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(udpRxTask,                   "udp_rx",   6144, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(MeshtasticUART::uartRxTask,  "uart_rx",  4096, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(meshTxTask,                  "mesh_tx",  6144, nullptr, 5, nullptr, 1);

    Serial.println("[Boot] All tasks started – bridge is running");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop()  –  unused; all work is done in FreeRTOS tasks
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // Print a periodic heartbeat so the serial monitor shows the board is alive.
    static uint32_t last_beat = 0;
    if (millis() - last_beat >= 30000) {
        last_beat = millis();
        Serial.printf("[Heartbeat] uptime=%lus  heap=%u  WiFi=%s\n",
                      millis() / 1000,
                      ESP.getFreeHeap(),
                      (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str()
                                                      : "disconnected");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
