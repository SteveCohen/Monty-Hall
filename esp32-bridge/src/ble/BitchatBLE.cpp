// ─────────────────────────────────────────────────────────────────────────────
// BitchatBLE.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "BitchatBLE.h"
#include "config.h"
#include <Arduino.h>

static constexpr uint8_t  BLE_CONNECT_TIMEOUT_S = 5;   // seconds
static constexpr uint32_t PRUNE_INTERVAL_MS     = 2000; // rate-limit prunePeers()

BitchatBLE &BitchatBLE::instance() {
    static BitchatBLE inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// RX characteristic callback
// A connected central wrote data to our RX characteristic -> forward to bridge.
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::RxWriteCallback::onWrite(NimBLECharacteristic *c,
                                          NimBLEConnInfo &info) {
    std::string val = c->getValue();
    if (val.size() < BITCHAT_HEADER_LEN) return;

    BitchatPacket pkt{};
    pkt.type = (uint8_t)val[0];
    pkt.ttl  = (uint8_t)val[1];
    memcpy(pkt.id, val.data() + 2, 16);
    pkt.payload_len = (uint16_t)(val.size() - BITCHAT_HEADER_LEN);
    if (pkt.payload_len > sizeof(pkt.payload)) return;
    memcpy(pkt.payload, val.data() + BITCHAT_HEADER_LEN, pkt.payload_len);

    MessageBridge::instance().onBLEPacket(pkt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Peer TX notification callback (static)
// A peer device notified us of a new outgoing message on its TX characteristic.
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::onPeerNotify(NimBLERemoteCharacteristic * /*c*/,
                               uint8_t *data, size_t len, bool /*isNotify*/) {
    if (len < BITCHAT_HEADER_LEN) return;

    BitchatPacket pkt{};
    pkt.type = data[0];
    pkt.ttl  = data[1];
    memcpy(pkt.id, data + 2, 16);
    pkt.payload_len = (uint16_t)(len - BITCHAT_HEADER_LEN);
    if (pkt.payload_len > sizeof(pkt.payload)) return;
    memcpy(pkt.payload, data + BITCHAT_HEADER_LEN, pkt.payload_len);

    MessageBridge::instance().onBLEPacket(pkt);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server callbacks (peripheral role)
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::onConnect(NimBLEServer *pServer, NimBLEConnInfo &info) {
    Serial.printf("[BLE] Central connected: %s\n",
                  info.getAddress().toString().c_str());
    // Tighten connection interval for lower latency chat.
    pServer->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    // Keep advertising so additional devices can still connect.
    NimBLEDevice::getAdvertising()->start();
}

void BitchatBLE::onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &info,
                               int reason) {
    Serial.printf("[BLE] Central disconnected: %s (reason %d)\n",
                  info.getAddress().toString().c_str(), reason);
    NimBLEDevice::getAdvertising()->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Scan result callback (central role)
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::onResult(NimBLEAdvertisedDevice *device) {
    if (!device->isAdvertisingService(NimBLEUUID(BITCHAT_SVC_UUID))) return;

    std::string addrStr = device->getAddress().toString();

    xSemaphoreTake(_peersMtx, portMAX_DELAY);
    bool skip = (_peers.count(addrStr) > 0)
             || (_pendingAddrs.count(addrStr) > 0)
             || (_peers.size() + _pendingAddrs.size() >= MAX_BLE_PEERS);
    if (!skip) _pendingAddrs.insert(addrStr);
    xSemaphoreGive(_peersMtx);
    if (skip) return;

    NimBLEAddress addr = device->getAddress();
    Serial.printf("[BLE] Found bitchat device: %s\n", addrStr.c_str());

    // Post address to connection queue; the BLE task does the actual connect.
    if (xQueueSend(connectQueue, &addr, 0) != pdTRUE) {
        Serial.println("[BLE] connectQueue full");
        // Remove from pending so future scans can retry.
        xSemaphoreTake(_peersMtx, portMAX_DELAY);
        _pendingAddrs.erase(addrStr);
        xSemaphoreGive(_peersMtx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect to a peer (called from bleTask, NOT from the scan callback)
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::connectToPeer(const NimBLEAddress &addr) {
    std::string addrStr = addr.toString();

    auto cleanup_pending = [&]() {
        xSemaphoreTake(_peersMtx, portMAX_DELAY);
        _pendingAddrs.erase(addrStr);
        xSemaphoreGive(_peersMtx);
    };

    NimBLEClient *pClient = NimBLEDevice::createClient(addr);
    if (!pClient) {
        Serial.println("[BLE] createClient failed");
        cleanup_pending();
        return;
    }
    pClient->setConnectionParams(12, 24, 0, 400);
    pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_S);

    if (!pClient->connect()) {
        Serial.printf("[BLE] connect() to %s failed\n", addrStr.c_str());
        NimBLEDevice::deleteClient(pClient);
        cleanup_pending();
        return;
    }

    NimBLERemoteService *pSvc = pClient->getService(BITCHAT_SVC_UUID);
    if (!pSvc) {
        Serial.println("[BLE] bitchat service not found on peer");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        cleanup_pending();
        return;
    }

    // Subscribe to peer's TX characteristic (peer -> us).
    NimBLERemoteCharacteristic *pTx = pSvc->getCharacteristic(BITCHAT_TX_UUID);
    if (!pTx || !pTx->canNotify() || !pTx->subscribe(true, onPeerNotify, true)) {
        Serial.println("[BLE] TX characteristic missing or subscribe failed - disconnecting");
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        cleanup_pending();
        return;
    }

    // Success: move from pending to connected.
    xSemaphoreTake(_peersMtx, portMAX_DELAY);
    _pendingAddrs.erase(addrStr);
    _peers[addrStr] = pClient;
    size_t total = _peers.size();
    xSemaphoreGive(_peersMtx);

    Serial.printf("[BLE] Connected to %s (%zu peer(s))\n", addrStr.c_str(), total);
}

// ─────────────────────────────────────────────────────────────────────────────
// Prune dropped connections (rate-limited)
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::prunePeers() {
    uint32_t now = millis();
    if (now - _lastPruneMs < PRUNE_INTERVAL_MS) return;
    _lastPruneMs = now;

    xSemaphoreTake(_peersMtx, portMAX_DELAY);
    for (auto it = _peers.begin(); it != _peers.end(); ) {
        if (!it->second->isConnected()) {
            Serial.printf("[BLE] Pruning lost peer: %s\n", it->first.c_str());
            NimBLEDevice::deleteClient(it->second);
            it = _peers.erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(_peersMtx);
}

// ─────────────────────────────────────────────────────────────────────────────
// broadcast()  –  send raw bitchat bytes to all reachable peers
//
// Takes a snapshot of the peer list under the mutex, then releases the mutex
// before doing blocking BLE I/O.
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::broadcast(const uint8_t *data, uint16_t len) {
    // Notify connected centrals via our own TX characteristic.
    if (_txChar && _server && _server->getConnectedCount() > 0) {
        _txChar->setValue(data, len);
        _txChar->notify();
    }

    // Snapshot peer list to avoid holding the mutex during BLE write I/O.
    std::vector<NimBLEClient *> snapshot;
    xSemaphoreTake(_peersMtx, portMAX_DELAY);
    snapshot.reserve(_peers.size());
    for (auto &[addrStr, client] : _peers) {
        if (client->isConnected()) snapshot.push_back(client);
    }
    xSemaphoreGive(_peersMtx);

    // Write to each peer's RX characteristic (us as central -> peer as peripheral).
    for (NimBLEClient *client : snapshot) {
        NimBLERemoteService *svc = client->getService(BITCHAT_SVC_UUID);
        if (!svc) continue;
        NimBLERemoteCharacteristic *rx = svc->getCharacteristic(BITCHAT_RX_UUID);
        if (rx && rx->canWriteNoResponse())
            rx->writeValue(data, len, false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FreeRTOS BLE task
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::bleTask(void *arg) {
    auto &self   = BitchatBLE::instance();
    auto &bridge = MessageBridge::instance();

    for (;;) {
        // ── Connect to any newly discovered peers ─────────────────────────────
        NimBLEAddress addr;
        if (xQueueReceive(self.connectQueue, &addr, 0) == pdTRUE) {
            // Stop scan while connecting (NimBLE constraint).
            NimBLEDevice::getScan()->stop();
            self.connectToPeer(addr);
            self.startScan();
        }

        // ── Forward outgoing messages to BLE peers ────────────────────────────
        BridgeMsg *msg = nullptr;
        if (xQueueReceive(bridge.bleOutQueue, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            self.broadcast(msg->data, msg->len);
            free(msg->data);
            delete msg;
        }

        // ── Maintenance: prune dead connections (rate-limited) ────────────────
        self.prunePeers();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────
void BitchatBLE::init() {
    _peersMtx   = xSemaphoreCreateMutex();
    connectQueue = xQueueCreate(MAX_BLE_PEERS, sizeof(NimBLEAddress));

    NimBLEDevice::init(NODE_NAME);
    NimBLEDevice::setMTU(512);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power on ESP32-S3

    // ── Peripheral: GATT server ───────────────────────────────────────────────
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(this);
    _server->advertiseOnDisconnect(false);  // we call start() manually

    NimBLEService *svc = _server->createService(BITCHAT_SVC_UUID);

    _txChar = svc->createCharacteristic(BITCHAT_TX_UUID,
                                        NIMBLE_PROPERTY::NOTIFY);
    _rxChar = svc->createCharacteristic(BITCHAT_RX_UUID,
                                        NIMBLE_PROPERTY::WRITE |
                                        NIMBLE_PROPERTY::WRITE_NR);
    _rxChar->setCallbacks(&_rxCb);

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BITCHAT_SVC_UUID);
    adv->setName(NODE_NAME);
    adv->setScanResponse(true);
    adv->start();
    Serial.printf("[BLE] Advertising as \"%s\"  SVC=%s\n",
                  NODE_NAME, BITCHAT_SVC_UUID);

    // ── Central: scanner ─────────────────────────────────────────────────────
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(this, false);
    scan->setActiveScan(true);
    scan->setInterval(160);  // in 0.625ms units -> 100ms
    scan->setWindow(80);     //                  ->  50ms (50% duty cycle)
    startScan();
}

void BitchatBLE::startScan() {
    // Non-blocking scan; results arrive via onResult().
    // Restarts automatically after BLE_SCAN_PERIOD_S seconds.
    NimBLEDevice::getScan()->start(BLE_SCAN_PERIOD_S,
        [](NimBLEScanResults /*results*/) {
            Serial.println("[BLE] Scan window done, restarting...");
            BitchatBLE::instance().startScan();
        }, false);
    Serial.println("[BLE] Scanning for bitchat peers...");
}
