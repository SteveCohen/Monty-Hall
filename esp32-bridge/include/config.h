#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// config.h  –  User-editable configuration for the bitchat↔Meshtastic bridge
// ─────────────────────────────────────────────────────────────────────────────

// ── WiFi credentials ─────────────────────────────────────────────────────────
#define WIFI_SSID       "your_ssid_here"
#define WIFI_PASS       "your_password_here"
#define WIFI_TIMEOUT_MS  15000

// ── Node identity ─────────────────────────────────────────────────────────────
// Human-readable BLE device name (max 29 bytes for BLE advertisement payload).
#define NODE_NAME       "esp32-bridge"

// Unique 32-bit Meshtastic node ID.  Must be unique across the mesh.
// Derive from ESP32 efuse MAC or assign manually.
#define NODE_ID         0xDEAD0001u

// ── Meshtastic – UDP (primary transport) ──────────────────────────────────────
// Standard Meshtastic LAN UDP mesh port and multicast group (unchanged in 2.x).
// Enable on your Meshtastic node with:
//   meshtastic --set network.enabled_protocols 1
#define MESH_UDP_PORT    4403
#define MESH_MCAST_ADDR  "224.0.0.69"

// ── Meshtastic – UART (fallback transport) ────────────────────────────────────
// Connect a Meshtastic device via UART to these ESP32-S3 GPIO pins.
// The wire protocol is the standard Meshtastic StreamInterface framing:
//   0x94 0xC3 <uint16_be length> <protobuf bytes>
#define MESH_UART_RX_PIN  16
#define MESH_UART_TX_PIN  17
#define MESH_UART_BAUD    115200

// ── bitchat BLE service / characteristic UUIDs ───────────────────────────────
// !! IMPORTANT: Replace these placeholder UUIDs with the real ones from !!
// !! https://github.com/permissionlesstech/bitchat before flashing.     !!
//
// TX characteristic: remote bitchat device sends data (we NOTIFY / subscribe)
// RX characteristic: remote bitchat device receives data (we WRITE to it)
#define BITCHAT_SVC_UUID  "F0E3C5A7-1234-4321-8765-9ABC5DEF0123"
#define BITCHAT_TX_UUID   "F0E3C5A8-1234-4321-8765-9ABC5DEF0123"
#define BITCHAT_RX_UUID   "F0E3C5A9-1234-4321-8765-9ABC5DEF0123"

// ── Bridge tuning ─────────────────────────────────────────────────────────────
// Maximum simultaneous BLE client connections (NimBLE supports up to 9).
#define MAX_BLE_PEERS     5

// TTL stamped on bitchat packets originating from the bridge.
#define MSG_TTL_INJECT    3

// Number of recent message IDs held in the deduplication ring buffer.
#define DEDUP_CACHE_SIZE  128

// BLE scan window length in seconds before restarting.
#define BLE_SCAN_PERIOD_S  5

// Meshtastic channel index for forwarded messages (0 = primary).
#define MESH_CHANNEL       0

// FreeRTOS inter-task queue depth (number of pending BridgeMsg pointers).
#define BRIDGE_QUEUE_LEN   16
