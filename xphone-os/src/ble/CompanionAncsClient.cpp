// xphone-os M2 — ported from x4-os src/companion/CompanionAncsClient.cpp.
// See CompanionAncsClient.h for the delta list. ANCS UUIDs, attribute
// request shape and Data Source reassembly are kept byte-identical to x4-os;
// the CCCD subscribe order is flipped to Data Source first, Notification
// Source second (Apple ANCS spec recommendation), the notify-RX path is
// re-plumbed through a fixed FreeRTOS queue so parsing happens on the main
// loop, and completed notifications land in the fixed-buffer
// NotificationStore.

#include "CompanionAncsClient.h"

#include <Arduino.h>
#include <BLEDevice.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../NotificationFilter.h"
#include "../NotificationStore.h"
#include "BleShim.h"
#include "CompanionBleService.h"

#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>
#endif

namespace {
constexpr uint16_t NO_CONN_HANDLE = 0xffff;
constexpr std::size_t MAX_ANCS_VALUE_BYTES = 512;

// Data Source fragment marshalled from the NimBLE host task to the main loop.
// Large 512 B slots; these are serialized by the single-in-flight backfill
// gate so a shallow queue is plenty.
struct AncsPacket {
  uint16_t length = 0;
  uint32_t sessionId = 0;
  uint8_t data[MAX_ANCS_VALUE_BYTES];
};

// Notification Source events are always <= 8 bytes (eventId, flags, category,
// count, uid LE32). They get their OWN queue with tiny slots and a DEEP depth
// so a reconnect replay burst (iOS replays every notification still in
// Notification Center at once) buffers on the host task without dropping —
// the transport-layer bug that was starving the backfill.
struct AncsNsPacket {
  uint8_t data[8];
  uint8_t len = 0;
  uint32_t sessionId = 0;
};

constexpr std::size_t PACKET_QUEUE_DEPTH = 6;   // Data Source fragments
constexpr std::size_t NS_QUEUE_DEPTH = 48;      // Notification Source events (burst headroom)

// Single-in-flight backfill gate timeout (millis). If a GetNotificationAttributes
// response never completes, release the gate after this so the queue keeps
// draining. 3.5 s comfortably exceeds a normal attribute round-trip at the
// low-duty 180 ms connection interval yet reclaims a wedged slot quickly.
constexpr uint32_t kBackfillFetchTimeoutMs = 3500;

// Minimum gap between requestResync() CCCD rewrites. A full 20-notification
// replay drains in a few seconds at the low-duty interval; restarting it on
// every scene re-entry inside that window would only repeat work in flight.
constexpr uint32_t kResyncThrottleMs = 5000;

// Static queue storage — no heap, sized once. Each packet also carries the ANCS
// session generation so data queued across a disconnect can be rejected.
StaticQueue_t gPacketQueueControl;
uint8_t gPacketQueueStorage[PACKET_QUEUE_DEPTH * sizeof(AncsPacket)];
StaticQueue_t gNsQueueControl;
uint8_t gNsQueueStorage[NS_QUEUE_DEPTH * sizeof(AncsNsPacket)];

#if defined(CONFIG_NIMBLE_ENABLED)
// ANCS service 7905F431-B5CE-4E99-A40F-4B1E122D00D0 and its three
// characteristics, little-endian byte order (same as x4-os).
constexpr ble_uuid128_t ANCS_SERVICE_UUID =
    BLE_UUID128_INIT(0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
constexpr ble_uuid128_t ANCS_NOTIFICATION_SOURCE_UUID =
    BLE_UUID128_INIT(0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
constexpr ble_uuid128_t ANCS_CONTROL_POINT_UUID =
    BLE_UUID128_INIT(0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
constexpr ble_uuid128_t ANCS_DATA_SOURCE_UUID =
    BLE_UUID128_INIT(0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);

enum AncsCommandId : uint8_t {
  GetNotificationAttributes = 0,
  GetAppAttributes = 1,
  PerformNotificationAction = 2,
};

enum AncsActionId : uint8_t {
  ActionPositive = 0,
  ActionNegative = 1,
};

// GetAppAttributes attribute ids. Unlike notification attributes, app
// attributes carry NO max-length parameter — the command is just the id list.
enum AncsAppAttributeId : uint8_t {
  AppAttributeDisplayName = 0,
};

enum AncsEventId : uint8_t {
  NotificationAdded = 0,
  NotificationModified = 1,
  NotificationRemoved = 2,
};

enum AncsEventFlag : uint8_t {
  EventFlagSilent = 1 << 0,
  EventFlagImportant = 1 << 1,
  EventFlagPreExisting = 1 << 2,
  EventFlagPositiveAction = 1 << 3,
  EventFlagNegativeAction = 1 << 4,
};

enum AncsCategoryId : uint8_t {
  CategoryOther = 0,
  CategoryIncomingCall = 1,
  CategoryMissedCall = 2,
  CategoryVoicemail = 3,
  CategorySocial = 4,
  CategorySchedule = 5,
  CategoryEmail = 6,
  CategoryNews = 7,
  CategoryHealthAndFitness = 8,
  CategoryBusinessAndFinance = 9,
  CategoryLocation = 10,
  CategoryEntertainment = 11,
};

enum AncsNotificationAttributeId : uint8_t {
  AttributeAppIdentifier = 0,
  AttributeTitle = 1,
  AttributeSubtitle = 2,
  AttributeMessage = 3,
  AttributeMessageSize = 4,
  AttributeDate = 5,
  AttributePositiveActionLabel = 6,
  AttributeNegativeActionLabel = 7,
};

// M2.1d: fixed staging buffers sized to the NotificationStore fields they
// feed — no std::string on the notification path. Subtitle/date are fallback
// text for an empty message, so subtitle gets the message size and date a
// small ISO-timestamp-sized buffer.
struct ParsedAttributes {
  uint32_t uid = 0;
  char app[NotificationStore::APP_ID_CHARS] = {0};
  char title[NotificationStore::TITLE_CHARS] = {0};
  char subtitle[NotificationStore::MESSAGE_CHARS] = {0};
  char message[NotificationStore::MESSAGE_CHARS] = {0};
  char date[32] = {0};
  bool hasApp = false;
  bool hasTitle = false;
  bool hasSubtitle = false;
  bool hasMessage = false;
  bool hasDate = false;
};

// Byte-wise little-endian reads: BLE payload buffers are not guaranteed to
// be aligned and the ESP32-C3 (RISC-V) faults on unaligned wide loads.
uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

// Clip raw attribute bytes into a fixed NUL-terminated buffer. When the clip
// cuts a multi-byte UTF-8 sequence, the incomplete tail is stripped
// (continuation bytes 0b10xxxxxx plus the cut lead byte) so no mojibake
// garbage renders on glass.
void clipUtf8(char* dst, std::size_t dstSize, const uint8_t* data, std::size_t length) {
  if (dstSize == 0) return;
  std::size_t count = std::min(length, dstSize - 1);
  const bool clipped = count < length;
  std::memcpy(dst, data, count);
  if (clipped && count > 0) {
    // Walk back over trailing continuation bytes.
    std::size_t i = count;
    while (i > 0 && (static_cast<uint8_t>(dst[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0) {
      const uint8_t lead = static_cast<uint8_t>(dst[i - 1]);
      std::size_t seqLen = 1;
      if ((lead & 0xE0) == 0xC0) seqLen = 2;
      else if ((lead & 0xF0) == 0xE0) seqLen = 3;
      else if ((lead & 0xF8) == 0xF0) seqLen = 4;
      if (seqLen > 1 && (count - (i - 1)) < seqLen) count = i - 1;  // cut sequence
    } else {
      count = 0;  // buffer is nothing but continuation bytes
    }
  }
  dst[count] = '\0';
}

// Decode one UTF-8 codepoint at *p, advancing *p past it; returns 0 at NUL.
// Malformed lead/continuation bytes advance one byte and return that byte so
// the walk always terminates (no infinite loop on garbage).
uint32_t nextUtf8(const char** p) {
  const uint8_t* s = reinterpret_cast<const uint8_t*>(*p);
  const uint8_t c = s[0];
  if (c == 0) return 0;
  uint32_t cp;
  int n;
  if (c < 0x80) { cp = c; n = 1; }
  else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
  else { *p += 1; return c; }  // stray continuation / invalid lead byte
  for (int i = 1; i < n; ++i) {
    if ((s[i] & 0xC0) != 0x80) { *p += 1; return c; }  // truncated sequence
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *p += n;
  return cp;
}

// Fold one codepoint into up to 3 ASCII bytes for the bespoke ~95-glyph font.
// Returns the count written (0 = drop). ASCII passes through; common
// typographic punctuation transliterates ("smart" quotes/dashes/ellipsis);
// common accented Latin letters fold to their base letter; everything else
// (emoji, symbols, other scripts) is dropped so nothing renders as '?' tofu.
int foldCodepoint(uint32_t cp, char out[3]) {
  if (cp == '\n' || cp == '\t' || cp == '\r') { out[0] = ' '; return 1; }
  if (cp < 0x20 || cp == 0x7F) return 0;                 // other control chars
  if (cp < 0x7F) { out[0] = static_cast<char>(cp); return 1; }
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201B: out[0] = '\''; return 1;  // ' ' ‛
    case 0x201C: case 0x201D: case 0x201F: out[0] = '"'; return 1;   // " "
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: out[0] = '-'; return 1;  // – — ― −
    case 0x2026: out[0] = out[1] = out[2] = '.'; return 3;           // …
    case 0x2022: case 0x00B7: out[0] = '-'; return 1;                // • ·
    case 0x00A0: case 0x2007: case 0x2009: case 0x200A: case 0x202F: out[0] = ' '; return 1;  // spaces
    default: break;
  }
  char b = 0;  // common Latin-1 accented letters -> base letter
  switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: b = 'A'; break;
    case 0xC7: b = 'C'; break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: b = 'E'; break;
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: b = 'I'; break;
    case 0xD0: b = 'D'; break;
    case 0xD1: b = 'N'; break;
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: b = 'O'; break;
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: b = 'U'; break;
    case 0xDD: b = 'Y'; break;
    case 0xDF: b = 's'; break;  // ß
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: b = 'a'; break;
    case 0xE7: b = 'c'; break;
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: b = 'e'; break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF: b = 'i'; break;
    case 0xF1: b = 'n'; break;
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: b = 'o'; break;
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: b = 'u'; break;
    case 0xFD: case 0xFF: b = 'y'; break;
    default: break;
  }
  if (b) { out[0] = b; return 1; }
  return 0;  // emoji, symbols, CJK, …: drop
}

// In-place UTF-8 -> ASCII normalization for on-glass notification text.
// Transliterates punctuation, folds common accents, drops what the font can't
// draw (emoji/symbols), and collapses the whitespace those drops leave behind
// (runs of spaces -> one; leading/trailing trimmed). Output byte length is
// always <= input, so the rewrite is safe in place (write never passes read).
void normalizeAsciiInPlace(char* s) {
  if (!s) return;
  const char* rd = s;
  char* wr = s;
  bool pendingSpace = false;  // hold a space until real text follows (trims edges/runs)
  bool wroteAny = false;
  uint32_t cp;
  while ((cp = nextUtf8(&rd)) != 0) {
    char out[3];
    const int n = foldCodepoint(cp, out);
    for (int i = 0; i < n; ++i) {
      if (out[i] == ' ') {
        if (wroteAny) pendingSpace = true;
        continue;
      }
      if (pendingSpace) { *wr++ = ' '; pendingSpace = false; }
      *wr++ = out[i];
      wroteAny = true;
    }
  }
  *wr = '\0';
}

// Fallback app display name: the last dotted segment of the bundle id
// ("net.whatsapp.WhatsApp" -> "WhatsApp"). Used until GetAppAttributes resolves
// the real iOS name and for apps that report an empty display name.
void prettifyBundle(const char* bundleId, char* out, std::size_t outSize) {
  if (outSize == 0) return;
  const char* seg = (bundleId && bundleId[0]) ? bundleId : "app";
  const char* dot = std::strrchr(seg, '.');
  if (dot && dot[1] != '\0') seg = dot + 1;
  std::snprintf(out, outSize, "%s", seg);
}

// ANCS AttributeDate is compact ISO 8601 "YYYYMMDD'T'HHMMSS" (e.g. the
// on-device "date=20260707T204640"). Collapse its digits into a decimal
// YYYYMMDDHHMMSS key that sorts chronologically, so the NotificationStore can
// order the reconnect backfill newest-first by the notification's real date
// regardless of the order iOS replays the preexisting events. Empty/short
// dates yield a smaller key (0 when none), which sinks to the oldest slot.
uint64_t parseAncsDateKey(const char* date) {
  if (!date) return 0;
  uint64_t key = 0;
  int digits = 0;
  for (const char* p = date; *p != '\0' && digits < 14; ++p) {
    if (*p >= '0' && *p <= '9') {
      key = key * 10u + static_cast<uint64_t>(*p - '0');
      ++digits;
    }
  }
  return key;
}

bool sameUuid(const ble_uuid_t* a, const ble_uuid128_t& b) { return ble_uuid_cmp(a, &b.u) == 0; }

const char* eventName(uint8_t eventId) {
  switch (eventId) {
    case NotificationAdded: return "added";
    case NotificationModified: return "modified";
    case NotificationRemoved: return "removed";
    default: return "unknown";
  }
}

const char* categoryName(uint8_t categoryId) {
  switch (categoryId) {
    case CategoryIncomingCall: return "call";
    case CategoryMissedCall: return "missed call";
    case CategoryVoicemail: return "voicemail";
    case CategorySocial: return "social";
    case CategorySchedule: return "schedule";
    case CategoryEmail: return "email";
    case CategoryNews: return "news";
    case CategoryHealthAndFitness: return "health";
    case CategoryBusinessAndFinance: return "finance";
    case CategoryLocation: return "location";
    case CategoryEntertainment: return "entertainment";
    case CategoryOther:
    default: return "other";
  }
}

bool parseAttributes(const uint8_t* buffer, std::size_t size, ParsedAttributes& parsed, bool& needsMore) {
  needsMore = false;
  if (size < 5 || buffer[0] != GetNotificationAttributes) {
    return false;
  }

  parsed.uid = readLe32(buffer + 1);
  std::size_t offset = 5;
  while (offset < size) {
    if (offset + 3 > size) {
      needsMore = true;
      return false;
    }

    const uint8_t attributeId = buffer[offset++];
    const uint16_t attributeLength = readLe16(buffer + offset);
    offset += 2;
    if (offset + attributeLength > size) {
      needsMore = true;
      return false;
    }

    const uint8_t* value = buffer + offset;
    switch (attributeId) {
      case AttributeAppIdentifier:
        clipUtf8(parsed.app, sizeof(parsed.app), value, attributeLength);
        parsed.hasApp = true;
        break;
      case AttributeTitle:
        clipUtf8(parsed.title, sizeof(parsed.title), value, attributeLength);
        parsed.hasTitle = true;
        break;
      case AttributeSubtitle:
        clipUtf8(parsed.subtitle, sizeof(parsed.subtitle), value, attributeLength);
        parsed.hasSubtitle = true;
        break;
      case AttributeMessage:
        clipUtf8(parsed.message, sizeof(parsed.message), value, attributeLength);
        parsed.hasMessage = true;
        break;
      case AttributeDate:
        clipUtf8(parsed.date, sizeof(parsed.date), value, attributeLength);
        parsed.hasDate = true;
        break;
      default: break;
    }
    offset += attributeLength;
  }

  const bool complete = parsed.hasApp && parsed.hasTitle && parsed.hasSubtitle && parsed.hasMessage && parsed.hasDate;
  if (!complete) needsMore = true;
  return complete;
}

int ancsGapEvent(ble_gap_event* event, void*) { return COMPANION_ANCS.handleGapEvent(event); }

int serviceDiscoveryCallback(uint16_t connHandle, const ble_gatt_error* error, const ble_gatt_svc* service,
                             void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (!client) return 0;

  if (error->status == 0 && service) {
    LOG_INF("ANCS", "ANCS service discovered conn=%u start=%u end=%u", connHandle, service->start_handle,
            service->end_handle);
    client->handleServiceDiscovered(service->start_handle, service->end_handle);
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    return 0;
  }

  LOG_ERR("ANCS", "ANCS service discovery failed status=%d", error->status);
  COMPANION_BLE.updateStatus("ANCS service not found");
  return error->status;
}

int characteristicDiscoveryCallback(uint16_t, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (!client) return 0;

  if (error->status == 0 && chr) {
    client->handleCharacteristicDiscovered(&chr->uuid.u, chr->val_handle);
    return 0;
  }

  if (error->status == BLE_HS_EDONE) {
    client->handleCharacteristicDiscoveryComplete();
    return 0;
  }

  LOG_ERR("ANCS", "Characteristic discovery failed status=%d", error->status);
  COMPANION_BLE.updateStatus("ANCS chars failed");
  return error->status;
}

// Apple-recommended CCCD order: Data Source first, then Notification Source,
// so attribute responses can never race ahead of the Data Source pipe.
int dataSourceSubscribeCallback(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (!client) return 0;
  if (error->status == 0) {
    client->handleDataSourceSubscribed();
    return 0;
  }
  LOG_ERR("ANCS", "Data Source subscribe failed status=%d", error->status);
  COMPANION_BLE.updateStatus("ANCS data subscribe failed");
  return error->status;
}

int notificationSourceSubscribeCallback(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (!client) return 0;
  if (error->status == 0) {
    client->handleNotificationSourceSubscribed();
    return 0;
  }
  LOG_ERR("ANCS", "Notification Source subscribe failed status=%d", error->status);
  COMPANION_BLE.updateStatus("ANCS notif subscribe failed");
  return error->status;
}

// requestResync() step 1 completion. On error we STILL fall through to the
// re-subscribe — the one state this must never leave behind is "CCCD off":
// that would silence live notifications entirely.
int notificationSourceUnsubscribeCallback(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (!client) return 0;
  if (error->status != 0) {
    LOG_ERR("ANCS", "Notification Source unsubscribe failed status=%d", error->status);
  }
  client->handleNotificationSourceUnsubscribed();
  return 0;
}

int controlPointWriteCallback(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
  auto* client = static_cast<CompanionAncsClient*>(arg);
  if (client) client->handleControlPointWriteComplete(error->status);
  return error->status == 0 ? 0 : error->status;
}
#endif
}  // namespace

CompanionAncsClient COMPANION_ANCS;

void CompanionAncsClient::ensureMutex() const {
  if (!stateMutex) {
    stateMutex = xSemaphoreCreateMutex();
  }
}

void CompanionAncsClient::ensureQueue() {
  if (!packetQueue) {
    packetQueue =
        xQueueCreateStatic(PACKET_QUEUE_DEPTH, sizeof(AncsPacket), gPacketQueueStorage, &gPacketQueueControl);
  }
  if (!nsQueue) {
    nsQueue = xQueueCreateStatic(NS_QUEUE_DEPTH, sizeof(AncsNsPacket), gNsQueueStorage, &gNsQueueControl);
  }
}

void CompanionAncsClient::setStatus(const char* message) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  snprintf(statusMessage, sizeof(statusMessage), "%s", message ? message : "");
  ++revision;
  xSemaphoreGive(stateMutex);
  // updateStatus keeps its std::string signature (out of scope here); the
  // implicit conversion at this boundary is a status line, not the hot
  // notification-data path.
  COMPANION_BLE.updateStatus(message ? message : "");
}

bool CompanionAncsClient::isPairingRequested() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = pairingRequested;
  xSemaphoreGive(stateMutex);
  return value;
}

void CompanionAncsClient::begin() {
  ensureMutex();
  ensureQueue();
  if (started) return;

#if defined(CONFIG_NIMBLE_ENABLED)
  BLEDevice::setCustomGapHandler(ancsGapEvent);
  started = true;
  pairingRequested = false;
  setStatus("ANCS idle");
#else
  started = true;
  setStatus("ANCS needs NimBLE");
#endif
}

void CompanionAncsClient::rearmAfterRadioResume() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  started = false;
  discovering = false;
  ancsReady = false;
  connHandle = NO_CONN_HANDLE;
  xSemaphoreGive(stateMutex);
  begin();
  requestPairing();
}

void CompanionAncsClient::requestPairing() {
  begin();
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pairingRequested = true;
  discovering = false;
  ancsReady = false;
  const uint16_t handle = connHandle;
  ++revision;
  xSemaphoreGive(stateMutex);

#if defined(CONFIG_NIMBLE_ENABLED)
  if (handle != NO_CONN_HANDLE) {
    if (COMPANION_BLE.isEncrypted()) {
      startDiscovery(handle);
    } else {
      // iOS rejects ANCS discovery on an unencrypted link with insufficient
      // authentication. Wait for pairing — handleAuthenticationComplete()
      // starts discovery once the link encrypts.
      setStatus("Waiting for iPhone pairing");
    }
  } else {
    setStatus("Pair X4 in iPhone Bluetooth");
  }
#else
  (void)handle;
  setStatus("ANCS needs NimBLE");
#endif
}

bool CompanionAncsClient::requestResync() {
#if defined(CONFIG_NIMBLE_ENABLED)
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool ready = ancsReady && connHandle != NO_CONN_HANDLE;
  const uint16_t handle = connHandle;
  const uint16_t valueHandle = notificationSourceHandle;
  xSemaphoreGive(stateMutex);
  if (!ready || valueHandle == 0 || !COMPANION_BLE.isEncrypted()) return false;

  const uint32_t now = millis();
  if (lastResyncMs != 0 && now - lastResyncMs < kResyncThrottleMs) return true;  // one already in flight/fresh
  lastResyncMs = now;

  // Step 1: CCCD off. Step 2 (re-enable) runs from the write-complete
  // callback so the two writes can't reorder on the controller.
  const uint8_t disableNotify[] = {0x00, 0x00};
  const int rc = ble_gattc_write_flat(handle, valueHandle + 1, disableNotify, sizeof(disableNotify),
                                      notificationSourceUnsubscribeCallback, this);
  if (rc != 0) {
    LOG_ERR("ANCS", "Notification Source resync CCCD write rc=%d", rc);
    lastResyncMs = 0;  // failed before it started — the next open may retry at once
    return false;
  }
  LOG_INF("ANCS", "Resync: re-subscribing Notification Source for a fresh replay");
  return true;
#else
  return false;
#endif
}

void CompanionAncsClient::dismissNotification(uint32_t uid, uint8_t categoryId, uint8_t flags,
                                              uint32_t sessionId) {
#if defined(CONFIG_NIMBLE_ENABLED)
  // A negative action on IncomingCall declines the call. Local CLEAR is still
  // allowed (and may create a tombstone), but it must never become that action.
  if (categoryId == CategoryIncomingCall) {
    LOG_DBG("ANCS", "Dismiss skipped uid=%lu: incoming call", static_cast<unsigned long>(uid));
    return;
  }
  if ((flags & EventFlagNegativeAction) == 0) {
    LOG_DBG("ANCS", "Dismiss skipped uid=%lu: NegativeAction unavailable", static_cast<unsigned long>(uid));
    return;
  }
  if ((uid & NotificationStore::RESTORED_UID_BIT) != 0 || sessionId == 0) {
    LOG_DBG("ANCS", "Dismiss skipped uid=%lu: restored/stale UID", static_cast<unsigned long>(uid));
    return;
  }

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool ready = ancsReady && connHandle != NO_CONN_HANDLE && sessionId == currentSessionId;
  xSemaphoreGive(stateMutex);
  if (!ready || !COMPANION_BLE.isEncrypted()) {
    LOG_DBG("ANCS", "Dismiss skipped uid=%lu: ANCS/session not ready", static_cast<unsigned long>(uid));
    return;
  }

  for (std::size_t i = 0; i < dismissCount; ++i) {
    const DismissItem& item = dismissQueue[(dismissHead + i) % DISMISS_CAPACITY];
    if (item.uid == uid && item.sessionId == sessionId) return;
  }
  if (dismissCount >= DISMISS_CAPACITY) {
    // Keep the most recent requests. Every dated clear also has a persisted
    // tombstone, so an evicted action gets another chance on the next replay.
    dismissHead = static_cast<uint8_t>((dismissHead + 1) % DISMISS_CAPACITY);
    --dismissCount;
    LOG_DBG("ANCS", "Dismiss queue full; dropped oldest pending action");
  }
  DismissItem& slot = dismissQueue[(dismissHead + dismissCount) % DISMISS_CAPACITY];
  slot.uid = uid;
  slot.sessionId = sessionId;
  ++dismissCount;
  LOG_DBG("ANCS", "Dismiss queued uid=%lu depth=%u", static_cast<unsigned long>(uid),
          static_cast<unsigned>(dismissCount));
#else
  (void)uid;
  (void)categoryId;
  (void)flags;
  (void)sessionId;
#endif
}

void CompanionAncsClient::processQueue() {
#if defined(CONFIG_NIMBLE_ENABLED)
  maybeKickDiscovery();  // M5: self-heal missed/failed discovery on wake re-bonds
  // Drain the Notification Source burst FIRST so every replayed UID is enqueued
  // into the backfill ring before we issue the first fetch, then drain the
  // (serialized) Data Source fragments. Both are drained fully per tick.
  if (nsQueue) {
    AncsNsPacket ns;  // tiny event packet — trivially on the main loop stack
    while (xQueueReceive(nsQueue, &ns, 0) == pdTRUE) {
      handleNotificationSource(ns.data, ns.len, ns.sessionId);
    }
  }
  if (packetQueue) {
    AncsPacket packet;  // ~520B on the main loop task stack (8KB) — fine here,
                        // NOT on nimble_host (see handleGapEvent).
    while (xQueueReceive(packetQueue, &packet, 0) == pdTRUE) {
      handleDataSource(packet.data, packet.length, packet.sessionId);
    }
  }
  // User actions win the next free Control Point slot. If an attribute fetch
  // is awaiting its Data Source reply pumpDismiss() waits; pumpBackfill() then
  // also sees the action write gate and cannot overlap it.
  pumpDismiss();
  pumpBackfill();            // drain queued ANCS backfill, one fetch at a time
  maybeRequestConnParams();  // M2.1b: post-subscribe low-duty conn params
#endif
}

uint32_t CompanionAncsClient::getRevision() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint32_t value = revision;
  xSemaphoreGive(stateMutex);
  return value;
}

void CompanionAncsClient::getStatusMessage(char* out, std::size_t outSize) const {
  if (!out || outSize == 0) return;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  snprintf(out, outSize, "%s", statusMessage);
  xSemaphoreGive(stateMutex);
}

#if defined(CONFIG_NIMBLE_ENABLED)
void CompanionAncsClient::handleServerConnect(uint16_t handle) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  connHandle = handle;
  ++currentSessionId;
  if (currentSessionId == 0) ++currentSessionId;  // 0 is the restored/stale namespace
  ++revision;
  const bool shouldStart = pairingRequested;
  xSemaphoreGive(stateMutex);

  if (shouldStart) {
    setStatus("iPhone connected; pairing");
  }
}

void CompanionAncsClient::handleServerDisconnect(uint16_t handle) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (handle == connHandle) {
    connHandle = NO_CONN_HANDLE;
    discovering = false;
    ancsReady = false;
    serviceStartHandle = 0;
    serviceEndHandle = 0;
    notificationSourceHandle = 0;
    controlPointHandle = 0;
    dataSourceHandle = 0;
    controlPointWriteInFlight = false;
    controlPointWriteKind = ControlPointNone;
    dismissFlushPending = true;
    connParamUpdatePending = false;  // M2.1b: never update a dead connection
    ++revision;
  }
  const bool shouldReport = pairingRequested;
  xSemaphoreGive(stateMutex);

  if (shouldReport) {
    setStatus("ANCS disconnected");
  }
}

void CompanionAncsClient::handleControlPointWriteComplete(int status) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint8_t kind = controlPointWriteKind;
  controlPointWriteInFlight = false;
  controlPointWriteKind = ControlPointNone;
  xSemaphoreGive(stateMutex);

  if (status == 0) {
    LOG_DBG("ANCS", "Control Point write complete kind=%u", static_cast<unsigned>(kind));
    return;
  }
  LOG_ERR("ANCS", "Control Point write failed kind=%u status=%d", static_cast<unsigned>(kind), status);
  setStatus(kind == ControlPointDismiss ? "ANCS dismiss failed" : "ANCS attr request failed");
}

void CompanionAncsClient::handleAuthenticationComplete(ble_gap_conn_desc* desc) {
  if (!desc) return;
  if (!desc->sec_state.encrypted) {
    if (isPairingRequested()) {
      setStatus("ANCS waiting for encryption");
    }
    return;
  }

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool shouldDiscover = pairingRequested;
  const bool alreadyDiscovering = discovering;
  const bool alreadyReady = ancsReady && connHandle == desc->conn_handle;
  xSemaphoreGive(stateMutex);

  if (!shouldDiscover) return;
  if (alreadyDiscovering || alreadyReady) return;
  startDiscovery(desc->conn_handle);
}

// Main-loop context (via processQueue). See the header comment: kicks
// discovery when the phone is connected and ANCS is wanted but never became
// ready — the encryption-edge trigger is unreliable on wake re-bonds and a
// failed discovery call used to latch `discovering` forever. An unencrypted
// kick just fails with insufficient-auth and retries 4 s later (capped 5).
void CompanionAncsClient::maybeKickDiscovery() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool want = pairingRequested;
  const bool ready = ancsReady;
  const bool disc = discovering;
  const uint16_t handle = connHandle;
  xSemaphoreGive(stateMutex);

  if (!want || handle == NO_CONN_HANDLE || ready || !COMPANION_BLE.isConnected()) {
    notReadySinceMs = 0;  // disarm; reset the cap once ready or disconnected
    if (ready || handle == NO_CONN_HANDLE) discoveryKicks = 0;
    return;
  }
  const uint32_t now = millis();
  if (notReadySinceMs == 0) {
    notReadySinceMs = now;
    return;
  }
  const uint32_t limit = disc ? 12000u : 4000u;  // in-flight discovery gets longer
  if (now - notReadySinceMs < limit) return;
  if (discoveryKicks >= 5) return;  // give up this connection; next connect resets
  ++discoveryKicks;
  notReadySinceMs = now;
  LOG_INF("ANCS", "watchdog: not ready (%s); kicking discovery, attempt %u",
          disc ? "discovery stuck" : "never started", static_cast<unsigned>(discoveryKicks));
  startDiscovery(handle);
}

void CompanionAncsClient::startDiscovery(uint16_t handle) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  connHandle = handle;
  discovering = true;
  ancsReady = false;
  serviceStartHandle = 0;
  serviceEndHandle = 0;
  notificationSourceHandle = 0;
  controlPointHandle = 0;
  dataSourceHandle = 0;
  ++revision;
  xSemaphoreGive(stateMutex);

  setStatus("Discovering ANCS");
  const int rc = ble_gattc_disc_svc_by_uuid(handle, &ANCS_SERVICE_UUID.u, serviceDiscoveryCallback, this);
  if (rc != 0) {
    LOG_ERR("ANCS", "ble_gattc_disc_svc_by_uuid rc=%d", rc);
    setStatus("ANCS discovery start failed");
  }
}

void CompanionAncsClient::handleServiceDiscovered(uint16_t startHandle, uint16_t endHandle) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  serviceStartHandle = startHandle;
  serviceEndHandle = endHandle;
  ++revision;
  xSemaphoreGive(stateMutex);
  discoverCharacteristics();
}

void CompanionAncsClient::handleCharacteristicDiscovered(const ble_uuid_t* uuid, uint16_t valueHandle) {
  if (!uuid) return;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (sameUuid(uuid, ANCS_NOTIFICATION_SOURCE_UUID)) {
    notificationSourceHandle = valueHandle;
  } else if (sameUuid(uuid, ANCS_CONTROL_POINT_UUID)) {
    controlPointHandle = valueHandle;
  } else if (sameUuid(uuid, ANCS_DATA_SOURCE_UUID)) {
    dataSourceHandle = valueHandle;
  }
  ++revision;
  xSemaphoreGive(stateMutex);
}

// Data Source CCCD first, Notification Source second (Apple recommendation).
void CompanionAncsClient::handleCharacteristicDiscoveryComplete() { subscribeDataSource(); }

void CompanionAncsClient::handleDataSourceSubscribed() { subscribeNotificationSource(); }

void CompanionAncsClient::handleNotificationSourceSubscribed() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ancsReady = true;
  // M2.1b lever 2: BOTH CCCDs are now written (Data Source first, this one
  // second), so ANCS setup is complete — arm the main-loop conn-param
  // renegotiation. The 1 s fuse lets any in-flight GATT traffic settle, and
  // keeps ble_gap_update_params off the NimBLE host task (codebase rule).
  connParamUpdatePending = true;
  connParamUpdateDueMs = millis() + 1000;
  ++revision;
  xSemaphoreGive(stateMutex);
  LOG_INF("ANCS", "ANCS subscribed");
  setStatus("ANCS subscribed; send notification");
}

// Main loop only (via processQueue). Renegotiates the connection from the
// fast setup interval (begin() advertises preferred 7.5-22.5 ms so pairing +
// discovery are snappy) down to a low-duty set once ANCS is fully subscribed.
//
// Apple accessory design guideline constraints (iOS rejects non-compliant
// requests): interval a multiple of 15 ms; intervalMax >= intervalMin
// (intervalMax >= intervalMin + 15 ms preferred); slaveLatency <= 30;
// intervalMax * (slaveLatency + 1) <= 2 s; supervisionTimeout 2-6 s; and
// (slaveLatency + 1) * intervalMax * 2 < supervisionTimeout. Chosen set:
//   itvl_min  = 72  x 1.25 ms =   90 ms (multiple of 15 ms)
//   itvl_max  = 144 x 1.25 ms =  180 ms (multiple of 15 ms; 180*(4+1)=900ms <= 2s)
//   latency   = 4   (<= 30)
//   timeout   = 200 x 10 ms   = 2000 ms  >= (1+4)*180ms*2 = 1800 ms, with margin
// Tradeoff: latency 4 @ 180 ms means worst-case ~0.9 s notification latency —
// invisible next to an e-ink refresh, while connected-idle radio wakeups drop
// roughly 10-40x vs the ~15 ms interval iOS grants during setup.
void CompanionAncsClient::maybeRequestConnParams() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool due = connParamUpdatePending && connHandle != NO_CONN_HANDLE &&
                   static_cast<int32_t>(millis() - connParamUpdateDueMs) >= 0;
  const uint16_t handle = connHandle;
  if (due) connParamUpdatePending = false;  // one shot per subscription
  xSemaphoreGive(stateMutex);
  if (!due) return;

  ble_gap_upd_params params = {};
  params.itvl_min = 72;              // 90 ms  (1.25 ms units)
  params.itvl_max = 144;             // 180 ms (1.25 ms units)
  params.latency = 4;                // peripheral may skip 4 events
  params.supervision_timeout = 200;  // 2000 ms (10 ms units)
  params.min_ce_len = 0;             // controller default CE length
  params.max_ce_len = 0;

  const int rc = ble_gap_update_params(handle, &params);
  if (rc != 0) {
    // Local reject (bad state/params) — log and keep going on the current
    // parameters; the link is unaffected.
    LOG_ERR("ANCS", "ble_gap_update_params rc=%d; keeping current conn params", rc);
    return;
  }
  // rc == 0 only means the L2CAP request went out; iOS may still reject it
  // asynchronously (L2CAP reject / no LL change) — that is fine, the link
  // simply stays on the old parameters. The live values are visible on the
  // About scene via CompanionBleService::getConnParams().
  LOG_INF("ANCS", "Requested low-duty conn params: 90-180 ms, latency=4, timeout=2000 ms");
}

void CompanionAncsClient::discoverCharacteristics() {
  uint16_t handle = NO_CONN_HANDLE;
  uint16_t start = 0;
  uint16_t end = 0;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  handle = connHandle;
  start = serviceStartHandle;
  end = serviceEndHandle;
  xSemaphoreGive(stateMutex);

  if (handle == NO_CONN_HANDLE) return;

  const int rc = ble_gattc_disc_all_chrs(handle, start, end, characteristicDiscoveryCallback, this);
  if (rc != 0) {
    LOG_ERR("ANCS", "ble_gattc_disc_all_chrs rc=%d", rc);
    setStatus("ANCS char discovery failed");
  } else {
    setStatus("Discovering ANCS chars");
  }
}

void CompanionAncsClient::subscribeNotificationSource() {
  uint16_t handle = NO_CONN_HANDLE;
  uint16_t valueHandle = 0;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  handle = connHandle;
  valueHandle = notificationSourceHandle;
  xSemaphoreGive(stateMutex);

  if (handle == NO_CONN_HANDLE || valueHandle == 0) {
    setStatus("ANCS notification source missing");
    return;
  }

  const uint8_t enableNotify[] = {0x01, 0x00};
  const int rc = ble_gattc_write_flat(handle, valueHandle + 1, enableNotify, sizeof(enableNotify),
                                      notificationSourceSubscribeCallback, this);
  if (rc != 0) {
    LOG_ERR("ANCS", "Notification Source CCCD write rc=%d", rc);
    setStatus("ANCS notif subscribe start failed");
  } else {
    setStatus("Subscribing ANCS source");
  }
}

// requestResync() step 2 (unsubscribe write completed, NimBLE host task):
// re-enable the CCCD. iOS treats the re-subscription as fresh and replays
// every notification still in Notification Center (EventFlagPreExisting),
// which handleNotificationSource feeds through the normal backfill queue.
void CompanionAncsClient::handleNotificationSourceUnsubscribed() {
  setStatus("ANCS resyncing");
  subscribeNotificationSource();
}

void CompanionAncsClient::subscribeDataSource() {
  uint16_t handle = NO_CONN_HANDLE;
  uint16_t valueHandle = 0;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  handle = connHandle;
  valueHandle = dataSourceHandle;
  xSemaphoreGive(stateMutex);

  if (handle == NO_CONN_HANDLE || valueHandle == 0) {
    setStatus("ANCS data source missing");
    return;
  }

  const uint8_t enableNotify[] = {0x01, 0x00};
  const int rc = ble_gattc_write_flat(handle, valueHandle + 1, enableNotify, sizeof(enableNotify),
                                      dataSourceSubscribeCallback, this);
  if (rc != 0) {
    LOG_ERR("ANCS", "Data Source CCCD write rc=%d", rc);
    setStatus("ANCS data subscribe start failed");
  } else {
    // ancsReady is set in handleNotificationSourceSubscribed() once BOTH
    // CCCDs are written (Data Source here, Notification Source next).
    setStatus("Subscribing ANCS data");
  }
}

bool CompanionAncsClient::writeControlPoint(const uint8_t* data, std::size_t len, uint8_t kind) {
  if (!data || len == 0) return false;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint16_t handle = connHandle;
  const uint16_t controlHandle = controlPointHandle;
  const bool available = ancsReady && handle != NO_CONN_HANDLE && controlHandle != 0 &&
                         !controlPointWriteInFlight;
  if (available) {
    // Set before handing the command to NimBLE: its completion runs on the
    // host task and may arrive before the main loop's next tick.
    controlPointWriteInFlight = true;
    controlPointWriteKind = kind;
  }
  xSemaphoreGive(stateMutex);
  if (!available) return false;

  const int rc = ble_gattc_write_flat(handle, controlHandle, data, len, controlPointWriteCallback, this);
  if (rc == 0) return true;

  // A synchronous start failure has no completion callback, so release the
  // shared procedure gate here and let the owning pump retry later.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  controlPointWriteInFlight = false;
  controlPointWriteKind = ControlPointNone;
  xSemaphoreGive(stateMutex);
  return false;
}

bool CompanionAncsClient::requestNotificationAttributes(uint32_t notificationUid) {

  // GetNotificationAttributes command, fixed 19-byte layout (byte-identical
  // to the old vector build): cmd, uid LE32, app, title+LE16 max, subtitle+
  // LE16 max, message+LE16 max, date.
  uint8_t cmd[16];
  cmd[0] = GetNotificationAttributes;
  cmd[1] = static_cast<uint8_t>(notificationUid & 0xff);
  cmd[2] = static_cast<uint8_t>((notificationUid >> 8) & 0xff);
  cmd[3] = static_cast<uint8_t>((notificationUid >> 16) & 0xff);
  cmd[4] = static_cast<uint8_t>((notificationUid >> 24) & 0xff);
  cmd[5] = AttributeAppIdentifier;
  cmd[6] = AttributeTitle;
  cmd[7] = 80;  // LE16 max length, low byte
  cmd[8] = 0;
  cmd[9] = AttributeSubtitle;
  cmd[10] = 80;
  cmd[11] = 0;
  cmd[12] = AttributeMessage;
  cmd[13] = 240;
  cmd[14] = 0;
  cmd[15] = AttributeDate;  // date takes no max-length parameter in ANCS

  if (!writeControlPoint(cmd, sizeof(cmd), ControlPointNotificationAttributes)) {
    LOG_DBG("ANCS", "Control Point attr request deferred uid=%lu", static_cast<unsigned long>(notificationUid));
    setStatus("ANCS attr request start failed");
    return false;
  }
  setStatus("Requesting notification text");
  return true;
}

// Main-loop context (via pumpBackfill, under the single-in-flight gate). Ask
// iOS for one app's display name: GetAppAttributes command = [cmd=1][bundle id
// bytes][NUL][DisplayName attr id]. App attribute ids carry no max-length
// parameter, so the command is just the id list after the NUL-terminated
// bundle. The reply lands on the shared Data Source (handleAppAttributesResponse).
bool CompanionAncsClient::requestAppAttributes(const char* bundleId) {
  if (!bundleId || !bundleId[0]) return false;

  uint8_t cmd[NotificationStore::APP_ID_CHARS + 3];
  std::size_t n = 0;
  cmd[n++] = GetAppAttributes;
  const std::size_t idLen = std::min(std::strlen(bundleId), sizeof(cmd) - 3);
  std::memcpy(cmd + n, bundleId, idLen);
  n += idLen;
  cmd[n++] = 0x00;                     // NUL terminates the app identifier
  cmd[n++] = AppAttributeDisplayName;  // request the display name only

  if (!writeControlPoint(cmd, n, ControlPointAppAttributes)) {
    LOG_DBG("ANCS", "App-attr request deferred bundle=%s", bundleId);
    return false;
  }
  return true;
}

// Main-loop context (via processQueue -> handleNotificationSource). The
// NotificationStore deliberately KEEPS removed notifications — it is an inbox
// log, not a mirror of the phone's notification shade. A Removed event revokes
// the UID's action provenance and drops any not-yet-started dismiss; it also
// clears half-received Data Source state for the now-dead notification.
void CompanionAncsClient::removeNotification(uint32_t notificationUid) {
  // Keep the row as inbox history, but revoke its action provenance: once iOS
  // emits Removed, this UID is no longer a live action target even though the
  // BLE connection/session itself is unchanged.
  NOTIFICATION_STORE.markUidStale(notificationUid);
  if (fetchInFlight && !fetchInFlightIsApp && fetchInFlightUid == notificationUid) {
    fetchInFlightSessionId = 0;
    fetchInFlightFlags = 0;
  }
  for (std::size_t i = 0; i < backfillCount; ++i) {
    BackfillItem& item = backfillQueue[(backfillHead + i) % BACKFILL_CAPACITY];
    if (item.uid == notificationUid) {
      item.sessionId = 0;
      item.flags = 0;
    }
  }
  for (std::size_t i = 0; i < dismissCount; ++i) {
    if (dismissQueue[(dismissHead + i) % DISMISS_CAPACITY].uid != notificationUid) continue;
    for (std::size_t j = i; j + 1 < dismissCount; ++j) {
      dismissQueue[(dismissHead + j) % DISMISS_CAPACITY] =
          dismissQueue[(dismissHead + j + 1) % DISMISS_CAPACITY];
    }
    --dismissCount;
    LOG_DBG("ANCS", "Dismiss queue removed dead uid=%lu", static_cast<unsigned long>(notificationUid));
    break;
  }
  if (dataSourceBufferUid == notificationUid) {
    dataSourceLen = 0;
    dataSourceBufferUid = 0;
  }
}

// Main-loop context (via processQueue).
void CompanionAncsClient::handleNotificationSource(const uint8_t* data, std::size_t length,
                                                   uint32_t sessionId) {
  if (length < 8) {
    setStatus("Short ANCS event");
    return;
  }

  const uint8_t eventId = data[0];
  const uint8_t eventFlags = data[1];
  const uint8_t categoryId = data[2];
  const uint8_t categoryCount = data[3];
  const uint32_t notificationUid = readLe32(data + 4);
  const bool preExisting = (eventFlags & EventFlagPreExisting) != 0;
  const bool silent = (eventFlags & EventFlagSilent) != 0;
  // Skip Silent ONLY for LIVE events (not user-facing). PREEXISTING events are
  // exactly the sleep backlog we want to recover, and iOS flags every replayed
  // notification Silent (observed on-device: flags 0x15 = PreExisting 0x04 |
  // Silent 0x01 | 0x10) — so those MUST be ingested despite the Silent bit.
  // NotificationRemoved is never skipped.
  const bool skipSilent = eventId != NotificationRemoved && silent && !preExisting;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool currentSession = sessionId != 0 && sessionId == currentSessionId;
  if (currentSession) {
    lastEventFlags = eventFlags;
    lastNotificationUid = notificationUid;
    ++revision;
  }
  xSemaphoreGive(stateMutex);

  if (!currentSession) {
    LOG_DBG("ANCS", "Discarding stale-session Notification Source uid=%lu",
            static_cast<unsigned long>(notificationUid));
    return;
  }

  if (skipSilent) {
    LOG_DBG("ANCS", "Skipping silent event=%s category=%u flags=0x%02x uid=%lu", eventName(eventId), categoryId,
            eventFlags, static_cast<unsigned long>(notificationUid));
    return;
  }

  LOG_INF("ANCS", "Notification Source event=%s category=%s(%u) count=%u flags=0x%02x uid=%lu%s", eventName(eventId),
          categoryName(categoryId), categoryId, categoryCount, eventFlags,
          static_cast<unsigned long>(notificationUid), preExisting ? " (preexisting)" : "");

  if (eventId == NotificationAdded || eventId == NotificationModified) {
    // Do NOT fetch attributes inline. Every fetch (live and preexisting) goes
    // through the single-in-flight backfill queue drained by pumpBackfill() so
    // a reconnect replay burst becomes sequential one-at-a-time pulls instead
    // of the flood that forced the old blanket skip. Live events jump the
    // queue (front) so a notification arriving mid-backfill is served first;
    // preexisting events queue at the back. pumpBackfill() runs at the end of
    // this same processQueue() tick, so a lone live event is still effectively
    // immediate (fires as soon as the drain loop returns).
    enqueueBackfill(notificationUid, categoryId, eventFlags, sessionId, /*front=*/!preExisting);
    if (!preExisting) {
      char status[48];
      snprintf(status, sizeof(status), "ANCS %s %s", eventName(eventId), categoryName(categoryId));
      setStatus(status);
    }
  } else if (eventId == NotificationRemoved) {
    removeNotification(notificationUid);
    setStatus("ANCS notification removed");
  }
}

// Main-loop context (via handleNotificationSource). Fixed ring, no heap.
// Dedups so an Added+Modified preexisting pair (or a UID already in flight)
// costs only one attribute fetch. On overflow the OLDEST queued UID is dropped
// (front) so the most-recent replayed notifications win.
void CompanionAncsClient::enqueueBackfill(uint32_t uid, uint8_t categoryId, uint8_t flags,
                                          uint32_t sessionId, bool front) {
  if (uid == 0) return;
  if (fetchInFlight && !fetchInFlightIsApp && fetchInFlightUid == uid &&
      fetchInFlightSessionId == sessionId) {
    // Modified may arrive while Added is being fetched. Preserve the freshest
    // action capability/category for the eventual store hand-off.
    fetchInFlightCategoryId = categoryId;
    fetchInFlightFlags = flags;
    return;
  }
  for (std::size_t i = 0; i < backfillCount; ++i) {
    BackfillItem& item = backfillQueue[(backfillHead + i) % BACKFILL_CAPACITY];
    if (item.uid == uid && item.sessionId == sessionId) {
      item.categoryId = categoryId;
      item.flags = flags;
      return;
    }
  }

  if (backfillCount >= BACKFILL_CAPACITY) {
    // Drop the oldest (front) to make room — newest replayed notifications win.
    backfillHead = static_cast<uint8_t>((backfillHead + 1) % BACKFILL_CAPACITY);
    --backfillCount;
    ++backfillDropCount;
    LOG_DBG("ANCS", "Backfill queue full; dropped oldest (total dropped=%lu)",
            static_cast<unsigned long>(backfillDropCount));
  }

  if (front) {
    backfillHead = static_cast<uint8_t>((backfillHead + BACKFILL_CAPACITY - 1) % BACKFILL_CAPACITY);
    BackfillItem& item = backfillQueue[backfillHead];
    item.uid = uid;
    item.sessionId = sessionId;
    item.categoryId = categoryId;
    item.flags = flags;
  } else {
    BackfillItem& item = backfillQueue[(backfillHead + backfillCount) % BACKFILL_CAPACITY];
    item.uid = uid;
    item.sessionId = sessionId;
    item.categoryId = categoryId;
    item.flags = flags;
  }
  ++backfillCount;
  LOG_DBG("ANCS", "Backfill queued uid=%lu %s depth=%u dropped=%lu", static_cast<unsigned long>(uid),
          front ? "front" : "back", static_cast<unsigned>(backfillCount),
          static_cast<unsigned long>(backfillDropCount));
}

// Main-loop context. Negative actions are issued only after any notification
// attribute response has completed: although the request write itself may have
// received its callback, iOS/NimBLE can still reject another Control Point
// procedure with BLE_HS_EBUSY while that fetch owns the shared Data Source.
void CompanionAncsClient::pumpDismiss() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool flush = dismissFlushPending;
  dismissFlushPending = false;
  const bool ready = ancsReady && connHandle != NO_CONN_HANDLE;
  const uint32_t sessionId = currentSessionId;
  const bool controlBusy = controlPointWriteInFlight;
  xSemaphoreGive(stateMutex);

  if (flush) {
    if (dismissCount > 0) {
      LOG_DBG("ANCS", "Dismiss queue flushed on disconnect depth=%u", static_cast<unsigned>(dismissCount));
    }
    dismissHead = 0;
    dismissCount = 0;
  }
  if (!ready || !COMPANION_BLE.isEncrypted() || fetchInFlight || controlBusy) return;

  // A disconnect/reconnect can happen between main-loop pumps. Session tags
  // are a second guard behind dismissFlushPending, so no stale UID can leak.
  while (dismissCount > 0 && dismissQueue[dismissHead].sessionId != sessionId) {
    dismissHead = static_cast<uint8_t>((dismissHead + 1) % DISMISS_CAPACITY);
    --dismissCount;
    LOG_DBG("ANCS", "Dismiss queue dropped stale-session action");
  }
  if (dismissCount == 0) return;

  const DismissItem item = dismissQueue[dismissHead];
  const uint8_t cmd[] = {
      PerformNotificationAction,
      static_cast<uint8_t>(item.uid & 0xff),
      static_cast<uint8_t>((item.uid >> 8) & 0xff),
      static_cast<uint8_t>((item.uid >> 16) & 0xff),
      static_cast<uint8_t>((item.uid >> 24) & 0xff),
      ActionNegative,
  };
  if (!writeControlPoint(cmd, sizeof(cmd), ControlPointDismiss)) {
    LOG_DBG("ANCS", "Dismiss write deferred uid=%lu", static_cast<unsigned long>(item.uid));
    return;
  }

  dismissHead = static_cast<uint8_t>((dismissHead + 1) % DISMISS_CAPACITY);
  --dismissCount;
  LOG_INF("ANCS", "Dismiss sent uid=%lu action=negative", static_cast<unsigned long>(item.uid));
}

// Main-loop context (via processQueue). Serializes attribute fetches: issues
// at most ONE GetNotificationAttributes at a time and waits for its Data
// Source response (or a timeout) before issuing the next. This is what turns
// the reconnect replay burst into a safe trickle — the exact flood the old
// EventFlagPreExisting skip was avoiding, now paced instead of dropped.
void CompanionAncsClient::pumpBackfill() {
  // "Link ready" snapshot under the same lock the rest of the client uses.
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool ready = ancsReady && connHandle != NO_CONN_HANDLE;
  xSemaphoreGive(stateMutex);

  if (!ready || !COMPANION_BLE.isEncrypted()) {
    // Link down / not yet encrypted — flush any stale backfill state so a new
    // session starts clean (the fresh preexisting replay re-enqueues UIDs). The
    // app-name cache PERSISTS across reconnect (bundle ids are stable), but an
    // app lookup caught in flight reverts to "needs request" so it retries.
    backfillHead = 0;
    backfillCount = 0;
    fetchRetryUid = 0;
    if (fetchInFlight && fetchInFlightIsApp) {
      AppNameEntry* e = findAppEntry(fetchInFlightBundle);
      if (e && e->state == 1) e->state = 0;
    }
    fetchInFlight = false;
    fetchInFlightIsApp = false;
    return;
  }

  // Rollover-safe timeout: a lost/never-arriving Data Source response must not
  // wedge the queue forever.
  if (fetchInFlight && static_cast<int32_t>(millis() - fetchDeadlineMs) >= 0) {
    if (fetchInFlightIsApp) {
      LOG_DBG("ANCS", "App-attr fetch timed out bundle=%s; releasing gate", fetchInFlightBundle);
      AppNameEntry* e = findAppEntry(fetchInFlightBundle);
      if (e && e->state == 1) e->state = 0;  // allow a later retry
    } else if (fetchInFlightUid != fetchRetryUid) {
      // First timeout for this UID: re-enqueue it (back of the queue). A
      // single lost Data Source response used to drop the notification
      // outright — nothing recovered it until the next reconnect replay.
      LOG_DBG("ANCS", "Backfill fetch timed out uid=%lu; retrying once",
              static_cast<unsigned long>(fetchInFlightUid));
      fetchRetryUid = fetchInFlightUid;
      // Clear first so enqueueBackfill() does not correctly dedupe this UID as
      // the still-in-flight request we are explicitly retiring.
      fetchInFlight = false;
      fetchInFlightIsApp = false;
      enqueueBackfill(fetchInFlightUid, fetchInFlightCategoryId, fetchInFlightFlags,
                      fetchInFlightSessionId, /*front=*/false);
    } else {
      LOG_DBG("ANCS", "Backfill fetch timed out uid=%lu again; giving up",
              static_cast<unsigned long>(fetchInFlightUid));
      fetchRetryUid = 0;
    }
    fetchInFlight = false;
    fetchInFlightIsApp = false;
  }
  if (fetchInFlight) return;  // wait for the outstanding response

  // A queued dismiss (or any Control Point write whose callback has not yet
  // fired) owns the single GATT procedure slot. pumpDismiss() runs first.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool controlBusy = controlPointWriteInFlight;
  xSemaphoreGive(stateMutex);
  if (controlBusy) return;

  // Notification attribute fetches take priority (the payload). Only when the
  // backfill queue is drained do we spend a gate slot resolving an app display
  // name — app-attr and notif-attr responses share the Data Source buffer, so
  // exactly one may be outstanding.
  if (backfillCount == 0) {
    for (uint8_t i = 0; i < appNameCacheCount; ++i) {
      if (appNameCache[i].state != 0) continue;  // resolved or already in flight
      appNameCache[i].state = 1;
      fetchInFlight = true;
      fetchInFlightIsApp = true;
      std::snprintf(fetchInFlightBundle, sizeof(fetchInFlightBundle), "%s", appNameCache[i].bundle);
      fetchDeadlineMs = millis() + kBackfillFetchTimeoutMs;
      LOG_DBG("ANCS", "App-attr fetch bundle=%s", fetchInFlightBundle);
      if (!requestAppAttributes(appNameCache[i].bundle)) {
        appNameCache[i].state = 0;
        fetchInFlight = false;
        fetchInFlightIsApp = false;
      }
      return;
    }
    return;  // nothing queued, no names pending
  }

  const BackfillItem item = backfillQueue[backfillHead];
  backfillHead = static_cast<uint8_t>((backfillHead + 1) % BACKFILL_CAPACITY);
  --backfillCount;

  fetchInFlight = true;
  fetchInFlightIsApp = false;
  fetchInFlightUid = item.uid;
  fetchInFlightSessionId = item.sessionId;
  fetchInFlightCategoryId = item.categoryId;
  fetchInFlightFlags = item.flags;
  fetchDeadlineMs = millis() + kBackfillFetchTimeoutMs;
  LOG_DBG("ANCS", "Backfill fetch uid=%lu remaining=%u", static_cast<unsigned long>(item.uid),
          static_cast<unsigned>(backfillCount));
  if (!requestNotificationAttributes(item.uid)) {
    fetchInFlight = false;
    enqueueBackfill(item.uid, item.categoryId, item.flags, item.sessionId, /*front=*/true);
  }
}

// Main-loop context (via processQueue). dataSourceBuffer/dataSourceLen are
// ONLY touched here and in removeNotification (also main-loop, via
// handleNotificationSource) — single-task access, parsed in place, no copy.
void CompanionAncsClient::handleDataSource(const uint8_t* data, std::size_t length,
                                           uint32_t sourceSessionId) {
  if (!data || length == 0) {
    setStatus("Empty ANCS data");
    return;
  }

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool currentSession = sourceSessionId != 0 && sourceSessionId == currentSessionId;
  xSemaphoreGive(stateMutex);
  if (!currentSession) {
    LOG_DBG("ANCS", "Discarding stale-session Data Source fragment");
    return;
  }

  // A response's first fragment is tagged by its command byte: notification
  // attributes (0) carry a UID, app attributes (1) a NUL-terminated bundle id.
  const bool startsNotif = length >= 5 && data[0] == GetNotificationAttributes;
  const bool startsApp = length >= 1 && data[0] == GetAppAttributes;
  uint32_t responseUid = startsNotif ? readLe32(data + 1) : 0;

  if (startsNotif || startsApp) {
    dataSourceLen = 0;  // new response supersedes any stale fragment
    dataSourceCmd = data[0];
    dataSourceBufferUid = responseUid;
  } else if (dataSourceLen > 0) {
    responseUid = dataSourceBufferUid;  // continuation of the buffered response
  } else {
    LOG_ERR("ANCS", "Unexpected ANCS data fragment len=%u first=0x%02x", static_cast<unsigned>(length), data[0]);
    setStatus("Unexpected ANCS data");
    return;
  }
  if (static_cast<std::size_t>(dataSourceLen) + length > sizeof(dataSourceBuffer)) {
    // Same >512B overflow reset behavior as the old vector path.
    dataSourceLen = 0;
    dataSourceBufferUid = 0;
    LOG_ERR("ANCS", "ANCS data response too large uid=%lu", static_cast<unsigned long>(responseUid));
    setStatus("ANCS data too large");
    return;
  }
  std::memcpy(dataSourceBuffer + dataSourceLen, data, length);
  dataSourceLen = static_cast<uint16_t>(dataSourceLen + length);

  // App display-name responses are parsed separately (different wire layout).
  if (dataSourceCmd == GetAppAttributes) {
    handleAppAttributesResponse();
    return;
  }

  ParsedAttributes parsed;
  bool needsMore = false;
  if (!parseAttributes(dataSourceBuffer, dataSourceLen, parsed, needsMore)) {
    if (needsMore) {
      LOG_INF("ANCS", "ANCS data fragment uid=%lu bytes=%u; waiting for more", static_cast<unsigned long>(responseUid),
              static_cast<unsigned>(dataSourceLen));
      setStatus("ANCS data fragment");
      return;
    }

    LOG_ERR("ANCS", "ANCS data parse incomplete uid=%lu bytes=%u", static_cast<unsigned long>(responseUid),
            static_cast<unsigned>(dataSourceLen));
    dataSourceLen = 0;
    dataSourceBufferUid = 0;
    setStatus("ANCS data parse failed");
    return;
  }

  if (dataSourceBufferUid == parsed.uid) {
    dataSourceLen = 0;
    dataSourceBufferUid = 0;
  }

  LOG_INF("ANCS", "Attributes uid=%lu app=%s titleBytes=%u subtitleBytes=%u messageBytes=%u date=%s",
          static_cast<unsigned long>(parsed.uid), parsed.app, static_cast<unsigned>(strlen(parsed.title)),
          static_cast<unsigned>(strlen(parsed.subtitle)), static_cast<unsigned>(strlen(parsed.message)), parsed.date);

  // Fold the display text down to what the bespoke font can draw: transliterate
  // smart punctuation, drop emoji/symbols (no more '?' tofu). The bundle id and
  // date are left raw — the id is a lookup key (see noteAppSeen) and the date is
  // pure digits. Length only ever shrinks, so the store fields still fit.
  normalizeAsciiInPlace(parsed.title);
  normalizeAsciiInPlace(parsed.subtitle);
  normalizeAsciiInPlace(parsed.message);

  // Hand-off into the fixed-buffer store (this is main-loop context, so the
  // store needs no locking). Fallback text mirrors the x4-os card bridge.
  const char* title = parsed.title[0] ? parsed.title : "Notification";
  const char* message = parsed.message;
  if (!parsed.message[0]) {
    message = parsed.subtitle[0] ? parsed.subtitle : parsed.date;
  }

  // Metadata belongs to the Notification Source event carried through the
  // backfill ring. A late response after timeout is still safe to display, but
  // gets session 0 / no action capability so CLEAR can never act on a UID whose
  // current-session provenance is no longer certain.
  const bool matchesFetch = fetchInFlight && !fetchInFlightIsApp && parsed.uid == fetchInFlightUid;
  const uint8_t categoryId = matchesFetch ? fetchInFlightCategoryId : CategoryOther;
  const uint8_t eventFlags = matchesFetch ? fetchInFlightFlags : 0;
  const uint32_t sessionId = matchesFetch ? fetchInFlightSessionId : 0;
  const uint64_t dateKey = parseAncsDateKey(parsed.date);

  if (NOTIFICATION_STORE.isTombstoned(dateKey, title)) {
    // Keep the tombstone after a match: Perform Notification Action has no
    // response, so a later replay is both another suppression and another
    // retry with that replay's fresh UID.
    LOG_INF("ANCS", "Tombstone suppressed replay uid=%lu date=%llu title=%s",
            static_cast<unsigned long>(parsed.uid), static_cast<unsigned long long>(dateKey), title);
    dismissNotification(parsed.uid, categoryId, eventFlags, sessionId);
    setStatus("ANCS cleared replay suppressed");
  } else {
    // Learn (and tally) every app before applying the blocklist. The phone's
    // picker is populated from this cache — sorted by how noisy each app is —
    // so hiding an app must not make it impossible to find and unhide later.
    noteAppSeen(parsed.app);
    if (!NOTIFICATION_FILTER.allows(parsed.app)) {
      // This is device presentation policy only: do not send an ANCS dismiss
      // action, because the user still wants the notification on the iPhone.
      LOG_INF("ANCS", "Hidden notification uid=%lu app=%s", static_cast<unsigned long>(parsed.uid), parsed.app);
      char filteredStatus[48];
      std::snprintf(filteredStatus, sizeof(filteredStatus), "ANCS hid %.31s", parsed.app);
      setStatus(filteredStatus);
    } else {
      NOTIFICATION_STORE.add(parsed.uid, parsed.app, title, message, dateKey, categoryId, eventFlags,
                             sessionId);
      setStatus("ANCS notification loaded");
    }
  }

  // Release the single-in-flight gate so pumpBackfill() can issue the next
  // queued fetch. Match the UID so a late/duplicate response for a different
  // notification can't clear a gate the pump has since re-armed for another.
  if (fetchInFlight && !fetchInFlightIsApp && parsed.uid == fetchInFlightUid) {
    fetchInFlight = false;
  }
}

// Main-loop context (via handleDataSource). Parse a fully-buffered
// GetAppAttributes response and land the app's display name in the cache. Wire
// layout: [cmd=1][bundle id bytes][NUL][attrId][len LE16][value ...]. Returns
// early (leaving the buffer intact) while any part is still missing so the next
// fragment can complete it — same reassembly discipline as the notif path.
void CompanionAncsClient::handleAppAttributesResponse() {
  const uint8_t* buf = dataSourceBuffer;
  const std::size_t size = dataSourceLen;

  std::size_t i = 1;  // scan the NUL-terminated bundle id (past the command byte)
  while (i < size && buf[i] != 0) ++i;
  if (i >= size) return;  // NUL not yet received — wait for more

  char bundle[NotificationStore::APP_ID_CHARS];
  const std::size_t idLen = std::min<std::size_t>(i - 1, sizeof(bundle) - 1);
  std::memcpy(bundle, buf + 1, idLen);
  bundle[idLen] = '\0';

  std::size_t off = i + 1;      // first attribute id, just past the NUL
  if (off + 3 > size) return;   // attrId + LE16 length not here yet
  const uint8_t attrId = buf[off++];
  const uint16_t attrLen = readLe16(buf + off);
  off += 2;
  if (off + attrLen > size) return;  // display-name value not fully received yet

  char name[APP_NAME_CHARS] = {0};
  if (attrId == AppAttributeDisplayName) {
    clipUtf8(name, sizeof(name), buf + off, attrLen);
    normalizeAsciiInPlace(name);  // an app could ship an emoji in its name too
  }

  // Complete: keep the resolved name (or leave the prettified fallback if iOS
  // returned an empty display name) and mark the entry resolved.
  AppNameEntry* e = findAppEntry(bundle);
  if (e) {
    if (name[0]) std::snprintf(e->name, sizeof(e->name), "%s", name);
    e->state = 2;
    ++appNameRevision;  // main loop repaints the list to swap the bundle id out
  }
  LOG_INF("ANCS", "App name %s -> %s", bundle, e ? e->name : "(uncached)");

  dataSourceLen = 0;
  dataSourceBufferUid = 0;
  dataSourceCmd = 0;
  if (fetchInFlight && fetchInFlightIsApp) {
    fetchInFlight = false;
    fetchInFlightIsApp = false;
  }
}

// Main-loop context. Linear search of the (small, fixed) app-name cache.
CompanionAncsClient::AppNameEntry* CompanionAncsClient::findAppEntry(const char* bundleId) {
  if (!bundleId || !bundleId[0]) return nullptr;
  for (uint8_t i = 0; i < appNameCacheCount; ++i) {
    if (std::strncmp(appNameCache[i].bundle, bundleId, sizeof(appNameCache[i].bundle)) == 0) {
      return &appNameCache[i];
    }
  }
  return nullptr;
}

// Main-loop context (via handleDataSource ingest). Add a cache entry for a
// newly-seen app with a prettified fallback name and mark it for lookup. When
// the cache is full the OLDEST entry is evicted (a re-seen app just re-adds and
// re-resolves — names are cheap).
void CompanionAncsClient::noteAppSeen(const char* bundleId) {
  if (!bundleId || !bundleId[0]) return;
  if (AppNameEntry* existing = findAppEntry(bundleId)) {
    // Tally per power cycle; the picker uses this to float noisy apps to the
    // top. Saturate rather than wrap so a chatty app can't sort to the bottom.
    if (existing->notifCount < UINT16_MAX) ++existing->notifCount;
    return;
  }

  uint8_t idx;
  if (appNameCacheCount < APP_NAME_CACHE_SIZE) {
    idx = appNameCacheCount++;
  } else {
    for (uint8_t i = 1; i < APP_NAME_CACHE_SIZE; ++i) appNameCache[i - 1] = appNameCache[i];
    idx = APP_NAME_CACHE_SIZE - 1;
  }
  AppNameEntry& e = appNameCache[idx];
  std::snprintf(e.bundle, sizeof(e.bundle), "%s", bundleId);
  prettifyBundle(bundleId, e.name, sizeof(e.name));  // shown until the real name resolves
  e.state = 0;                                        // pumpBackfill() will fetch it
  e.notifCount = 1;
}

// Main-loop context (Notifications scene render). Cache hit -> the iOS display
// name; miss -> the prettified bundle. Never triggers a fetch (ingest does).
void CompanionAncsClient::appDisplayName(const char* bundleId, char* out, std::size_t outSize) const {
  if (outSize == 0) return;
  if (bundleId && bundleId[0]) {
    for (uint8_t i = 0; i < appNameCacheCount; ++i) {
      if (std::strncmp(appNameCache[i].bundle, bundleId, sizeof(appNameCache[i].bundle)) == 0) {
        std::snprintf(out, outSize, "%s", appNameCache[i].name);
        return;
      }
    }
    prettifyBundle(bundleId, out, outSize);
    return;
  }
  std::snprintf(out, outSize, "%s", "unknown");
}

std::size_t CompanionAncsClient::appCount() const { return appNameCacheCount; }

bool CompanionAncsClient::getApp(const std::size_t index, char* bundle, const std::size_t bundleSize,
                                 char* name, const std::size_t nameSize, uint16_t* notifCount) const {
  if (bundle && bundleSize > 0) bundle[0] = '\0';
  if (name && nameSize > 0) name[0] = '\0';
  if (notifCount) *notifCount = 0;
  if (!bundle || !name || bundleSize == 0 || nameSize == 0 || index >= appNameCacheCount) return false;

  std::snprintf(bundle, bundleSize, "%s", appNameCache[index].bundle);
  std::snprintf(name, nameSize, "%s", appNameCache[index].name);
  if (notifCount) *notifCount = appNameCache[index].notifCount;
  return true;
}

// NimBLE HOST TASK context: copy the notify payload into the static queue and
// return. No parsing, no std::string, no large stack objects here — the
// nimble_host stack is a hard budget (docs/x4-core-learnings.md).
int CompanionAncsClient::handleGapEvent(ble_gap_event* event) {
  // M2.1d: this callback runs on the nimble_host task — capture its handle
  // once so the main loop can report its stack high-water mark.
  if (!hostTaskHandle) hostTaskHandle = xTaskGetCurrentTaskHandle();

  if (!event || event->type != BLE_GAP_EVENT_NOTIFY_RX) return 0;

  uint16_t sourceHandle = 0;
  uint16_t dataHandle = 0;
  uint16_t handle = NO_CONN_HANDLE;
  uint32_t sessionId = 0;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  handle = connHandle;
  sessionId = currentSessionId;
  sourceHandle = notificationSourceHandle;
  dataHandle = dataSourceHandle;
  xSemaphoreGive(stateMutex);

  if (event->notify_rx.conn_handle != handle) return 0;
  const bool isSource = event->notify_rx.attr_handle == sourceHandle;
  const bool isData = event->notify_rx.attr_handle == dataHandle;
  if (!isSource && !isData) return 0;

  const uint16_t packetLength = OS_MBUF_PKTLEN(event->notify_rx.om);

  if (isSource) {
    // Notification Source (<=8 B) -> dedicated DEEP small-slot queue. This is
    // where the reconnect replay burst lands; the depth (48) gives it headroom
    // so events are no longer dropped at the transport layer before the
    // backfill ever sees them. queueHighWater / queueDropCount track THIS queue
    // (the DS queue is serialized by the in-flight gate and rarely fills).
    if (!nsQueue) return 0;
    static AncsNsPacket nsScratch;  // single producer (nimble_host)
    const std::size_t copyLength = std::min<std::size_t>(packetLength, sizeof(nsScratch.data));
    if (os_mbuf_copydata(event->notify_rx.om, 0, copyLength, nsScratch.data) != 0) {
      LOG_ERR("ANCS", "ANCS NS notify copy failed");
      return 0;
    }
    nsScratch.len = static_cast<uint8_t>(copyLength);
    nsScratch.sessionId = sessionId;
    if (xQueueSend(nsQueue, &nsScratch, 0) != pdTRUE) {
      queueDropCount = queueDropCount + 1;
      LOG_ERR("ANCS", "ANCS NS queue full; dropping len=%u", static_cast<unsigned>(copyLength));
    } else {
      const UBaseType_t waiting = uxQueueMessagesWaiting(nsQueue);
      if (waiting > queueHighWater) queueHighWater = static_cast<uint8_t>(waiting);
    }
    return 0;
  }

  // Data Source fragment -> large-slot queue. A static scratch keeps the 512B
  // staging buffer off this task's small stack; xQueueSend copies it out.
  if (!packetQueue) return 0;
  static AncsPacket scratch;  // single producer (nimble_host)
  const std::size_t copyLength = std::min<std::size_t>(packetLength, MAX_ANCS_VALUE_BYTES);
  if (os_mbuf_copydata(event->notify_rx.om, 0, copyLength, scratch.data) != 0) {
    LOG_ERR("ANCS", "ANCS DS notify copy failed");
    return 0;
  }
  scratch.length = static_cast<uint16_t>(copyLength);
  scratch.sessionId = sessionId;
  if (xQueueSend(packetQueue, &scratch, 0) != pdTRUE) {
    // DS fragments are serialized by the single-in-flight backfill gate, so a
    // full DS queue is not expected; log if it ever happens.
    LOG_ERR("ANCS", "ANCS DS queue full; dropping len=%u", static_cast<unsigned>(copyLength));
  }
  return 0;
}
#endif
