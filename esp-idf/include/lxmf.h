/**
 * lxmf — LXMF messaging protocol task.
 *
 * Multi-identity (slots 0..LXMF_MAX_IDENTITIES-1); opportunistic single-packet
 * delivery plus on-demand Link + Resource transfer for larger messages. No
 * auto-create at boot — identities are created explicitly (lxmfCreateIdentity),
 * so a node with none runs transport-only. Storage-driven lifecycle for
 * outbound (client writes stage=draft → ready, firmware packs/signs/queues/
 * sends); inbound verification + dedup + storage write.
 *
 * Storage is the API — no DataChannel, no ITS port for clients. Every
 * frontend (browser, CLI, on-device UI) reads/writes the `s.lxmf.*` /
 * `lxmf.*` / `secrets.lxmf.*` keys; the lxmf task subscribes to its own
 * subtree and reacts.
 *
 * Consumer of rnsd over RNSD_PORT_DEST: one hosted-destination (our-dest) connection per
 * identity.
 */
#pragma once

#include "service.h"
#include <cstdint>

/* ── Message status (stored as the u8 `status` record field) ────────────────
 * A single unified state: lifecycle progress, terminal outcome, or gave-up
 * reason all live here. Companion field `tries` (u8) is the try count for the
 * current phase; tries == LXMF_TRIES_GAVEUP (255) is the ONE definitive terminal
 * marker — while tries < 255 the message is still in play (a sweep may retry a
 * progress status; one-shot statuses are set to 255 the moment they occur).
 *
 * These integer VALUES are persisted in every message record, so the list is
 * APPEND-ONLY: add new members at the end, never renumber or remove one.
 * MIRROR: keep in exact sync with the TS `LxmfStatus` enum + STATUS_NAME in
 * lxmf/browser/src/modules/lxmf.ts (same names, same numbers). */
static constexpr uint8_t LXMF_TRIES_GAVEUP = 255;

enum LxmfStatus : uint8_t {
    /* progress (tries < 255; a sweep may act) */
    LXMF_ST_DRAFT             = 0,   /* unsent; filtered from the thread */
    LXMF_ST_QUEUED            = 1,
    LXMF_ST_REQUESTING_PATH   = 2,
    LXMF_ST_SENDING           = 3,
    LXMF_ST_AWAITING_PROOF    = 4,   /* egressed, no proof yet — shown in flight, not a check */
    LXMF_ST_RETRYING_DELIVERY = 5,
    LXMF_ST_RETRYING_LINK     = 6,
    /* done */
    LXMF_ST_DELIVERED         = 7,   /* cryptographic proof received */
    LXMF_ST_CANCELLED         = 8,
    LXMF_ST_RECEIVED          = 9,   /* inbound */
    /* gave-up reasons (paired with tries == 255) */
    LXMF_ST_NO_PROOF          = 10,
    LXMF_ST_NO_ROUTE          = 11,
    LXMF_ST_TOO_LARGE         = 12,
    LXMF_ST_EVICTED           = 13,
    LXMF_ST_BAD_PEER          = 14,
    LXMF_ST_DISABLED          = 15,
    LXMF_ST_MAILBOX_STARTING  = 16,
    LXMF_ST_PACK_FAIL         = 17,
    LXMF_ST_OUTBOX_FULL       = 18,
    LXMF_ST_LINK_OPEN_FAIL    = 19,
    LXMF_ST_RES_MALLOC        = 20,
    LXMF_ST_RES_SEND          = 21,
    LXMF_ST_LINK_SEND_DROP    = 22,
    LXMF_ST_PACKET_SEND_DROP  = 23,
    LXMF_ST_RES_TRANSFER      = 24,
    LXMF_ST_LINK_FAIL         = 25,
    LXMF_ST_LINK_CLOSED       = 26,
    LXMF_ST_UNKNOWN           = 27,
    LXMF_ST_NO_RESPONSE       = 28,  /* egressed opportunistically (no link), no delivery
                                      * proof came back — the peer may simply be offline,
                                      * so we can't claim it was received. Distinct from
                                      * NO_PROOF, which is a link send that went unproven. */
};

/* status code → its ALL-CAPS enum name for display (meta line, CLI). This is the
 * only direction ever needed — a stored code is never parsed back from text.
 * MIRROR: keep in sync with STATUS_NAME in lxmf/browser/src/modules/lxmf.ts. */
static inline const char* lxmfStatusName(uint8_t s) {
    switch (s) {
        case LXMF_ST_DRAFT:             return "DRAFT";
        case LXMF_ST_QUEUED:            return "QUEUED";
        case LXMF_ST_REQUESTING_PATH:   return "REQUESTING_PATH";
        case LXMF_ST_SENDING:           return "SENDING";
        case LXMF_ST_AWAITING_PROOF:    return "AWAITING_PROOF";
        case LXMF_ST_RETRYING_DELIVERY: return "RETRYING_DELIVERY";
        case LXMF_ST_RETRYING_LINK:     return "RETRYING_LINK";
        case LXMF_ST_DELIVERED:         return "DELIVERED";
        case LXMF_ST_CANCELLED:         return "CANCELLED";
        case LXMF_ST_RECEIVED:          return "RECEIVED";
        case LXMF_ST_NO_PROOF:          return "NO_PROOF";
        case LXMF_ST_NO_ROUTE:          return "NO_ROUTE";
        case LXMF_ST_TOO_LARGE:         return "TOO_LARGE";
        case LXMF_ST_EVICTED:           return "EVICTED";
        case LXMF_ST_BAD_PEER:          return "BAD_PEER";
        case LXMF_ST_DISABLED:          return "DISABLED";
        case LXMF_ST_MAILBOX_STARTING:  return "MAILBOX_STARTING";
        case LXMF_ST_PACK_FAIL:         return "PACK_FAIL";
        case LXMF_ST_OUTBOX_FULL:       return "OUTBOX_FULL";
        case LXMF_ST_LINK_OPEN_FAIL:    return "LINK_OPEN_FAIL";
        case LXMF_ST_RES_MALLOC:        return "RES_MALLOC";
        case LXMF_ST_RES_SEND:          return "RES_SEND";
        case LXMF_ST_LINK_SEND_DROP:    return "LINK_SEND_DROP";
        case LXMF_ST_PACKET_SEND_DROP:  return "PACKET_SEND_DROP";
        case LXMF_ST_RES_TRANSFER:      return "RES_TRANSFER";
        case LXMF_ST_LINK_FAIL:         return "LINK_FAIL";
        case LXMF_ST_LINK_CLOSED:       return "LINK_CLOSED";
        case LXMF_ST_UNKNOWN:           return "UNKNOWN";
        case LXMF_ST_NO_RESPONSE:       return "NO_RESPONSE";
        default:                        return "";
    }
}

/** Bring up the lxmf task. The task is always started — even on a
 *  transport-only node — so the announce-fanout subscription and the
 *  cmd handlers are live. With zero identities allocated it simply
 *  never announces an LXMF destination of its own. */
class LxmfService : public Service {
public:
    void onInit() override;
};

/** Create a new LXMF identity with the given display name.
 *
 *  Both modes write the `lxmf.cmd.identity_new` sentinel; the lxmf
 *  task's storage subscription processes it on its own task. Sync
 *  mode then waits (vTaskDelay-polls storage) for the sentinel to
 *  clear before returning. The detailed error reason — if any — lives
 *  in the [lxmf] err()/warn() log on the lxmf task.
 *
 *  Returns:
 *    sync=false : 0 on validation success, -1 if display_name is empty
 *    sync=true  : allocated slot index (0..LXMF_MAX_IDENTITIES-1) on
 *                 success, -1 on validation error, lxmf-side failure,
 *                 or 5 s timeout */
int lxmfCreateIdentity(const char* display_name, bool sync = false);

/** Destroy the identity at slot `n`. Wipes secrets.lxmf.id.<n>.privkey
 *  + every s.lxmf.id.<n>.* and lxmf.id.<n>.* storage key, closes the
 *  our-dest, unsubscribes the per-id cmd handler.
 *
 *  Sync mode polls storage until secrets disappear (or 5 s timeout).
 *  Returns true on success, false on validation error / timeout /
 *  lxmf-side failure. */
bool lxmfDestroyIdentity(int n, bool sync = false);
