#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BitchatBLE.h  –  NimBLE dual-role handler (Peripheral + Central)
//
// Peripheral role (GATT server):
//   Advertises with BITCHAT_SVC_UUID so bitchat devices can find the bridge.
//   Hosts TX characteristic (NOTIFY)  – bridge notifies connected peers.
//   Hosts RX characteristic (WRITE)   – connected peers write chat data here.
//
// Central role (scanner + client):
//   Continuously scans for devices advertising BITCHAT_SVC_UUID.
//   Connects to up to MAX_BLE_PEERS simultaneously.
//   Subscribes to each peer's TX characteristic for incoming chat messages.
//   Writes to each peer's RX characteristic to forward outgoing messages.
//
// NimBLE-Arduino v1.4.x API is assumed (h2zero/NimBLE-Arduino @ ^1.4.3).
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "bridge/MessageBridge.h"
#include "config.h"

class BitchatBLE
    : public NimBLEServerCallbacks
    , public NimBLEAdvertisedDeviceCallbacks
{
public:
    static BitchatBLE &instance();

    // Call once from setup() after MessageBridge::init().
    void init();

    // (Re)start the BLE scanner.
    void startScan();

    // Broadcast raw bitchat bytes to all connected peers.
    // Called by the BLE-TX FreeRTOS task.
    void broadcast(const uint8_t *data, uint16_t len);

    // FreeRTOS task entry point: drains bleOutQueue and calls broadcast().
    static void bleTask(void *arg);

    // Queue of peer addresses to connect to (posted from scan callback).
    QueueHandle_t connectQueue;

private:
    BitchatBLE() = default;

    // ── NimBLEServerCallbacks (peripheral role) ───────────────────────────────
    void onConnect   (NimBLEServer *pServer, NimBLEConnInfo &info) override;
    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &info, int reason) override;

    // ── NimBLEAdvertisedDeviceCallbacks (central role / scanner) ─────────────
    void onResult(NimBLEAdvertisedDevice *device) override;

    // Connect to a peer BLE address, subscribe to its TX characteristic.
    void connectToPeer(const NimBLEAddress &addr);

    // Prune entries from _peers whose connection has dropped.
    void prunePeers();

    // ── GATT server objects ───────────────────────────────────────────────────
    NimBLEServer         *_server  = nullptr;
    NimBLECharacteristic *_txChar  = nullptr;  // we NOTIFY centrals on this
    NimBLECharacteristic *_rxChar  = nullptr;  // centrals WRITE to this

    // ── Connected peers (central role) ────────────────────────────────────────
    // Key = BLE address string, Value = connected NimBLEClient*
    std::map<std::string, NimBLEClient *> _peers;
    // Addresses currently queued for connection (prevents double-connect).
    std::set<std::string> _pendingAddrs;
    SemaphoreHandle_t _peersMtx = nullptr;

    // Rate-limit prunePeers() to avoid thrashing every loop iteration.
    uint32_t _lastPruneMs = 0;

    // ── RX characteristic callback (inner class) ──────────────────────────────
    // Invoked when a remote central writes chat data to our RX characteristic.
    class RxWriteCallback : public NimBLECharacteristicCallbacks {
        void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override;
    } _rxCb;

    // ── Peer TX notification callback (static, passed to subscribe()) ─────────
    static void onPeerNotify(NimBLERemoteCharacteristic *c,
                             uint8_t *data, size_t len, bool isNotify);
};
