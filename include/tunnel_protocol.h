#pragma once
#include <stdint.h>

// ── Frame types ───────────────────────────────────────────────────────────────
#define FRAME_CONNECT  0x01
#define FRAME_CONFIG   0x02
#define FRAME_REQUEST  0x03
#define FRAME_RESPONSE 0x04
#define FRAME_DATA     0x05
#define FRAME_RESET    0x06
#define FRAME_PING     0x07
#define FRAME_PONG     0x08
#define FRAME_GOAWAY   0x09

// ── Flags ─────────────────────────────────────────────────────────────────────
#define FLAG_HAS_BODY          0x01   // REQUEST: body DATA frames follow
#define FLAG_END_STREAM        0x01   // DATA: last data frame for this stream
#define FLAG_RESPONSE_HAS_BODY 0x01   // RESPONSE: body DATA frames follow

// ── Reset error codes ─────────────────────────────────────────────────────────
#define RESET_NORMAL               0x0000
#define RESET_BACKEND_UNREACHABLE  0x0001
#define RESET_BACKEND_TIMEOUT      0x0002
#define RESET_INTERNAL_ERROR       0x0003

// ── Frame header: 10 bytes, big-endian ───────────────────────────────────────
//   type(1) | stream_id(4) | flags(1) | length(4)
#define FRAME_HEADER_SIZE 10

// ── Protocol version sent in CONNECT payload ──────────────────────────────────
#define PROTOCOL_VERSION 0x02
