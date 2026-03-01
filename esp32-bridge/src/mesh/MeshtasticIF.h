#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MeshtasticIF.h  –  Abstract interface for Meshtastic transports
//
// Both MeshtasticUDP and MeshtasticUART implement this interface, letting
// the mesh-TX task select whichever transport is currently available.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>

class MeshtasticIF {
public:
    virtual ~MeshtasticIF() = default;

    // Initialise the transport.  Returns true on success.
    virtual bool init() = 0;

    // Send a pre-encoded protobuf buffer (ToRadio).
    // Returns true if the bytes were handed off to the hardware.
    virtual bool sendPacket(const uint8_t *buf, uint16_t len) = 0;

    // True if the transport is initialised and the underlying link is up.
    virtual bool isAvailable() const = 0;
};
