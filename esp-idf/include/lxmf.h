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
 * identity — except one whose `s.lxmf.id.<n>.proxy_role` is `client`, which
 * registers nothing and exchanges messages with the always-on device holding
 * its account over a permanently-held Channel instead (lxmproxy_wire.h, and
 * the proxy-client section of lxmf.cpp). At every moment exactly one device
 * registers and announces an account's lxmf.delivery — never zero, never two.
 */
#pragma once

#include "service.h"
#include <cstdint>
#include <string>

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

/* Longest quoted fragment (FIELD_REPLY_QUOTE) kept from an inbound message. A
 * quote is one line of a bubble; anything past this is a peer spending our
 * flash on text no screen will show, so it is dropped rather than truncated —
 * a cut fragment would no longer be found in the message it quotes and would
 * be refused on display anyway. */
static constexpr size_t LXMF_REPLY_QUOTE_MAX = 512;

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
    /* Proxy states, on a client whose account is served by an lxmproxy server.
     *
     * An outbound that goes through a proxy has FIVE states and no others,
     * however many the server itself passes through:
     *
     *   QUEUED             nothing has left this device yet
     *   SENDING_TO_PROXY   handed to the Channel, not yet acknowledged   …
     *   ON_OUR_PROXY       our proxy has it and is trying                ✓
     *   OUR_PROXY_DELIVERED  it reached the recipient                    ✓✓
     *   OUR_PROXY_GAVE_UP  our proxy tried and stopped                   ✕
     *
     * The server's own machinery — path requests, link retries, proof waits —
     * is its business, not a state of ours: the record collapses every one of
     * them to ON_OUR_PROXY and keeps the real `LxmfStatus` in `proxy_status`,
     * where the message's detail page can say WHICH failure it gave up with.
     * A refusal is not one of the five: PROXY_REFUSED is the server declining
     * to take the message at all, which is trouble reaching the proxy rather
     * than the proxy's verdict on reaching the recipient. */
    LXMF_ST_ON_OUR_PROXY      = 29,  /* our proxy holds it and is sending it */
    LXMF_ST_PROXY_REFUSED     = 30,  /* the server refused it: over quota, or the
                                      * account is not one it serves */
    LXMF_ST_SENDING_TO_PROXY  = 31,  /* the SEND is on the Channel; no answer yet */
    LXMF_ST_OUR_PROXY_DELIVERED = 32,/* our proxy says the recipient has it */
    /* 33 retired */
    LXMF_ST_RADIO_BUSY        = 34,  /* send failed while the local LoRa radio was
                                      * shedding frames to channel contention — the
                                      * own channel is jammed, not the peer silent */
    /* Classic lxmf.propagation node states (tries == 255). ON_PN is the
     * final state of a propagated send — a propagation node issues no
     * delivery proof; the recipient pulls the message on its next sync. */
    LXMF_ST_ON_PN             = 35,  /* uploaded to a propagation node */
    LXMF_ST_PN_FAIL           = 36,  /* propagation upload failed (link/transfer) */
    LXMF_ST_PN_REJECTED       = 37,  /* the node refused the upload (stamp/access) */
    LXMF_ST_DELIVERY_TIMEOUT  = 38,  /* not delivered within s.lxmf.delivery_timeout
                                      * minutes of attempts from the delivery queue */
    LXMF_ST_OUR_PROXY_GAVE_UP = 39,  /* our proxy stopped trying. WHY is in the
                                      * record's `proxy_status`, verbatim from
                                      * the server — this code says only that
                                      * the attempt is over and that it is the
                                      * proxy's verdict, not a failure to reach
                                      * the proxy (that is PROXY_REFUSED, or a
                                      * message still sitting at QUEUED). */
};

/* Has this outbound stopped moving? A VERDICT is a status nothing is going to
 * change: delivered, cancelled, or one of the ways it failed. Everything else
 * is still in play — the sender's own attempts, and the two states that mean
 * another node is holding it.
 *
 * This is what a proxy relays, and it is the whole of what it relays: its
 * client wants the outcome, not the commentary. One STATUS per outgoing
 * message, at the end. That the server took the message at all is not news —
 * the client saw its own SEND go out. */
static inline bool lxmfStatusIsVerdict(uint8_t s) {
    switch (s) {
        case LXMF_ST_DRAFT:
        case LXMF_ST_QUEUED:
        case LXMF_ST_REQUESTING_PATH:
        case LXMF_ST_SENDING:
        case LXMF_ST_AWAITING_PROOF:
        case LXMF_ST_RETRYING_DELIVERY:
        case LXMF_ST_RETRYING_LINK:
        case LXMF_ST_SENDING_TO_PROXY:
        case LXMF_ST_ON_OUR_PROXY:
        case LXMF_ST_ON_PN:
            return false;
        default:
            return true;
    }
}

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
        case LXMF_ST_ON_OUR_PROXY:      return "ON_OUR_PROXY";
        case LXMF_ST_PROXY_REFUSED:     return "PROXY_REFUSED";
        case LXMF_ST_SENDING_TO_PROXY:  return "SENDING_TO_PROXY";
        case LXMF_ST_OUR_PROXY_DELIVERED: return "OUR_PROXY_DELIVERED";
        case LXMF_ST_RADIO_BUSY:        return "RADIO_BUSY";
        case LXMF_ST_ON_PN:             return "ON_PN";
        case LXMF_ST_PN_FAIL:           return "PN_FAIL";
        case LXMF_ST_PN_REJECTED:       return "PN_REJECTED";
        case LXMF_ST_DELIVERY_TIMEOUT:  return "DELIVERY_TIMEOUT";
        case LXMF_ST_OUR_PROXY_GAVE_UP: return "OUR_PROXY_GAVE_UP";
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

/** Import an existing account into a free slot: `privkey_hex` is its 128-hex
 *  Ed25519+X25519 private key, `display_name` the name it already advertises
 *  (may be null/empty), `role` the value for `s.lxmf.id.<n>.proxy_role` — null
 *  or "" leaves it `off`, "server" marks a slot this device hosts on somebody
 *  else's behalf. The slot registers and announces `lxmf.delivery` at once,
 *  because nothing else would ever say the destination is here now.
 *
 *  Both modes write the `lxmf.cmd.identity_import` sentinel. Sync mode waits
 *  (≤ 5 s) and returns the allocated slot, or -1; async returns 0 on a
 *  well-formed request. */
int lxmfImportIdentity(const char* privkey_hex, const char* display_name,
                       const char* role, bool sync);

/** Which slot holds the identity behind `dest_hash` (its `lxmf.delivery`
 *  destination), or -1 when none does. Reads the published addresses, so it is
 *  safe from any task and answers from the moment the slot loads. */
int lxmfSlotForDest(const uint8_t dest_hash[16]);

/** Destroy the identity at slot `n`. Wipes secrets.lxmf.id.<n>.privkey
 *  + every s.lxmf.id.<n>.* and lxmf.id.<n>.* storage key, closes the
 *  our-dest, unsubscribes the per-id cmd handler.
 *
 *  Sync mode polls storage until secrets disappear (or 5 s timeout).
 *  Returns true on success, false on validation error / timeout /
 *  lxmf-side failure. */
bool lxmfDestroyIdentity(int n, bool sync = false);

/** The radio's reading of the link to `peer_hex`, as a person reads it — one
 *  line per direction the radio has measured, joined by `sep`:
 *
 *      us->them 91 dB path loss, SNR 11 dB @ tx +10 dBm (10 mW)
 *      them->us 88 dB path loss, SNR 9 dB @ tx +22 dBm (158 mW)
 *
 *  A peer outside SUPE states no power, so neither direction has a loss; what
 *  it has is what this radio knows on its own, and that is rendered instead
 *  (`heard @ -95 dBm / SNR 9.5 dB @ tx +22 dBm (158 mW)`). Empty when no radio
 *  has measured the peer at all.
 *
 *  Read LIVE from iface-lora's `lora.<n>.meas.*`, not from the ping record: the
 *  measurement is republished on that straddle's own beat and owes the probe
 *  nothing, so a copy taken when a probe settled was stale the next time the
 *  radio heard the peer — and on a first probe, empty. One implementation so
 *  that every frontend describes the same measurement the same way; the browser
 *  mirror is `peerMeasOf` + `pingLinkLines` in lxmf/browser. */
std::string lxmfPingLink(const std::string& peer_hex, const char* sep = "\n");
