// ─────────────────────────────────────────────────────────────────────────────
// MeshtasticUART.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "MeshtasticUART.h"
#include "proto/MeshProto.h"
#include "config.h"
#include <Arduino.h>

static constexpr uint8_t FRAME_START1 = 0x94;
static constexpr uint8_t FRAME_START2 = 0xC3;

MeshtasticUART &MeshtasticUART::instance() {
    static MeshtasticUART inst;
    return inst;
}

bool MeshtasticUART::init() {
    // Serial2 on ESP32-S3: user assigns RX/TX pins in config.h
    _serial = &Serial2;
    _serial->begin(MESH_UART_BAUD, SERIAL_8N1, MESH_UART_RX_PIN, MESH_UART_TX_PIN);
    _ready = true;
    Serial.printf("[UART] Meshtastic serial on RX=%d TX=%d @ %u baud\n",
                  MESH_UART_RX_PIN, MESH_UART_TX_PIN, MESH_UART_BAUD);
    return true;
}

bool MeshtasticUART::isAvailable() const {
    return _ready && _serial != nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// TX: frame and send
// ─────────────────────────────────────────────────────────────────────────────
bool MeshtasticUART::sendPacket(const uint8_t *buf, uint16_t len) {
    if (!isAvailable() || len == 0 || len > 512) return false;

    uint8_t header[4] = {
        FRAME_START1,
        FRAME_START2,
        (uint8_t)(len >> 8),
        (uint8_t)(len & 0xFFu),
    };
    _serial->write(header, sizeof(header));
    _serial->write(buf, len);
    _serial->flush();

    Serial.printf("[UART] TX %u bytes\n", len);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RX state machine – one byte at a time
// ─────────────────────────────────────────────────────────────────────────────
bool MeshtasticUART::feedByte(uint8_t b) {
    switch (_state) {
        case State::WAIT_START1:
            if (b == FRAME_START1) _state = State::WAIT_START2;
            break;

        case State::WAIT_START2:
            _state = (b == FRAME_START2) ? State::READ_LEN_MSB
                                         : State::WAIT_START1;
            break;

        case State::READ_LEN_MSB:
            _expectedLen = (uint16_t)b << 8;
            _state = State::READ_LEN_LSB;
            break;

        case State::READ_LEN_LSB:
            _expectedLen |= b;
            _rxPos = 0;
            if (_expectedLen == 0 || _expectedLen > sizeof(_rxBuf)) {
                Serial.printf("[UART] Bad frame length %u – discarding\n", _expectedLen);
                _state = State::WAIT_START1;
            } else {
                _state = State::READ_DATA;
            }
            break;

        case State::READ_DATA:
            _rxBuf[_rxPos++] = b;
            if (_rxPos >= _expectedLen) {
                _state = State::WAIT_START1;
                return true;  // frame complete
            }
            break;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// FreeRTOS task
// ─────────────────────────────────────────────────────────────────────────────
void MeshtasticUART::uartRxTask(void *arg) {
    auto &self   = MeshtasticUART::instance();
    auto &bridge = MessageBridge::instance();

    for (;;) {
        if (!self.isAvailable()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        while (self._serial->available()) {
            uint8_t b = (uint8_t)self._serial->read();
            if (self.feedByte(b)) {
                // Complete frame in _rxBuf[0.._rxPos-1]
                MeshPacket mp{};
                if (MeshProto::decodeFromRadio(self._rxBuf, self._rxPos, mp)) {
                    Serial.printf("[UART] RX from 0x%08X: %.*s\n",
                                  mp.from_id,
                                  mp.decoded.payload_len,
                                  (char *)mp.decoded.payload);
                    bridge.onMeshPacket(mp);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
