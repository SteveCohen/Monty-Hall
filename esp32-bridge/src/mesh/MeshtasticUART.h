#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MeshtasticUART.h  –  Fallback Meshtastic transport over UART (Serial2)
//
// Wire protocol: Meshtastic StreamInterface framing
//   [0x94][0xC3][MSB_LEN][LSB_LEN][<protobuf bytes>]
//   Where LEN is the number of protobuf bytes (big-endian uint16).
//
// Incoming frames are decoded from a byte-at-a-time state machine to tolerate
// partial reads from the hardware FIFO.
//
// Both TX and RX are handled by this class:
//   • sendPacket() is called from the mesh-TX task (synchronous, safe from any
//     task because HardwareSerial write() is not ISR-based on ESP32-S3 Arduino).
//   • uartRxTask() drives the receive state machine.
// ─────────────────────────────────────────────────────────────────────────────

#include <HardwareSerial.h>
#include "MeshtasticIF.h"
#include "bridge/MessageBridge.h"

class MeshtasticUART : public MeshtasticIF {
public:
    static MeshtasticUART &instance();

    bool init()                                    override;
    bool sendPacket(const uint8_t *buf, uint16_t len) override;
    bool isAvailable() const                       override;

    // FreeRTOS task: runs the RX state machine.
    static void uartRxTask(void *arg);

private:
    MeshtasticUART() = default;

    HardwareSerial *_serial = nullptr;
    bool            _ready  = false;

    // ── RX state machine ──────────────────────────────────────────────────────
    enum class State : uint8_t {
        WAIT_START1,
        WAIT_START2,
        READ_LEN_MSB,
        READ_LEN_LSB,
        READ_DATA,
    };

    State    _state        = State::WAIT_START1;
    uint16_t _expectedLen  = 0;
    uint8_t  _rxBuf[512]   = {};
    uint16_t _rxPos        = 0;

    // Process one byte through the state machine.
    // Returns true when a complete frame has been assembled in _rxBuf[0.._rxPos-1].
    bool feedByte(uint8_t b);
};
