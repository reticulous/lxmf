# lxmf — LXMF messaging on the mesh

**lxmf** is the device's LXMF mailbox: it sends and receives signed
[LXMF](https://github.com/markqvist/LXMF) messages over [rns](../rns),
holds up to four independent identities, and advertises each one so other
nodes can reach it. It interoperates with stock LXMF clients (Sideband,
NomadNet, MeshChat) wire-for-wire.

LXMF ("Lightweight Extensible Message Format") is Mark Qvist's
human-messaging layer on top of Reticulum: signed, store-and-forwardable
messages addressed to a node's self-generated cryptographic identity, with
no servers and no accounts.

## Origins

LXMF here is implemented from scratch in C++ against the
[`markqvist/LXMF`](https://github.com/markqvist/LXMF) Python reference,
pinned to **LXMF 0.9.8**. It sits *above* the Reticulum stack and has zero
microReticulum includes — it reaches the network only through rnsd's
byte-array C API (sha256, sign, verify, destination-hash, identity
generate/erase/recall, request-path). The wire codec, signature scope,
delivery-mode selection, and proof-of-work stamps are all native. The wire
format and the per-byte deltas from upstream are in
[INTERNALS.md](INTERNALS.md).

## What it does

- **Messaging.** Packs, signs, and transmits outbound messages; verifies,
  de-duplicates, and stores inbound ones. Signatures are checked against the
  sender's Reticulum identity — forged or corrupt messages are dropped
  silently.
- **Identities.** Up to `LXMF_MAX_IDENTITIES = 4` independent mailboxes
  (`lxmf.delivery` destinations), each with its own keypair, contacts, and
  message history, fully siloed. **No identity is created automatically** — a
  device with none runs as a transport-only node (it relays and tracks the
  mesh but has no mailbox of its own).
- **Selectable delivery mode.** Per message, lxmf picks a single
  opportunistic packet, a Reticulum Link (DIRECT), or a Resource transfer for
  large bodies. A per-identity/global method chooses how eagerly to hold a
  Link — from `link-always` through the default `link-if-one-exists` (ride a
  warm Link, else opportunistic) and `link-if-big` down to
  `opportunistic-or-fail`. The conversation header shows the live Link state
  and toggles it open/closed on tap.
- **Announces.** Each enabled identity periodically announces its delivery
  destination. Every inbound `lxmf.delivery` announce on the mesh is
  collected into a shared, cross-identity **announce catalogue** of everyone
  the device has heard of.
- **Stamps.** Pays and (optionally) enforces LXMF proof-of-work stamps as
  spam friction.
- **Propagation nodes.** Sends through and receives from classic
  `lxmf.propagation` store-and-forward nodes (see below) — a message can
  be resent via a node from its detail page, and a user-ordered node list
  is polled for held messages.
- **Message-notification sound.** Plays a short sound on inbound delivery
  via the optional [audio](../audio) engine.

## How it interacts with other straddles — storage is the API

There is **no DataChannel and no ITS port for consumers**. Every action is a
storage read or write. The lxmf task subscribes to its own command keys and
reacts; it publishes message records and live state back into storage.

```
Browser / LCD app / CLI ──storage read+write──► s.lxmf.* · lxmf.* · secrets.lxmf.*
                                                       │  (lxmf subscribes to *.cmd.*)
                                                       ▼
                                                  ┌────────┐
                                                  │  lxmf  │ ──► rnsd ──► interfaces
                                                  └────────┘
```

Internally lxmf is a consumer of rnsd: one hosted-destination (our-dest)
connection per identity on `RNSD_PORT_DEST`, outbound Links on
`RNSD_PORT_LINK`, and the announce fan-out on `RNSD_PORT_ANNOUNCES`. It also
hosts two private inbound ports for rnsd's back-connects (inbound Links and
Resource hand-off). None of that is client-facing — frontends only touch
storage.

Consequences a frontend can rely on:

- **Multi-frontend coherence is free.** Two browser tabs, the CLI, and the
  on-device app all see the same inbox. Mark a message read in one and the
  others reflect it. No locking, no merge.
- **Single writer per field.** Client-owned and firmware-owned fields are
  disjoint. You create a message record and set its content; the firmware
  owns `status`, `tries`, `wire`, `message_id`.

lxmf starts automatically when the straddle is in the build (its init is
folded into the generated startup dispatcher, after rnsd). The task always
runs — even with zero identities — so the announce subscription and the
command handlers are live.

### Key namespaces

| Prefix | Persistence | Who writes | Purpose |
|---|---|---|---|
| `s.lxmf.*` | survives reboot | client + firmware (disjoint fields) | identities, messages, contacts, config |
| `secrets.lxmf.*` | survives reboot | firmware | private keys (never leave the device) |
| `lxmf.*` | RAM, re-published ~1 Hz | firmware | live status, stats, announce catalogue |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*`, `lxmf.url_*` | transient | client writes | imperative actions (below) |

## Commands — self-clearing keys

Imperative actions are **sentinels**: you write the key, the firmware
performs the action and (for the identity/per-identity commands) deletes the
key. Presence = request in flight; absence = done.

**Identity-level** (`lxmf.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.cmd.identity_new` | optional label | generate a new identity, allocate the next slot, bring its mailbox up |
| `lxmf.cmd.identity_import` | 128-hex private key | import a key into a new slot |
| `lxmf.cmd.identity_destroy` | `<n>` | wipe identity `n` — its secret, all its storage, its subscriptions |
| `lxmf.cmd.pn_sync` | `all` or `<32-hex>` | check propagation nodes for held messages now (all check-marked, or one node) |
| `lxmf.cmd.pn_add` | `<32-hex>[\|<name>]` | append a propagation node at the first free list index |

**Per-identity** (`lxmf.id.<n>.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.id.<n>.cmd.send` | `<peer>/<key>[/pn:<hash>]` | pack, sign, and transmit the draft at `s.lxmf.id.<n>.msgs.<peer>.<key>`; the optional `pn:` segment uploads to that propagation node instead |
| `lxmf.id.<n>.cmd.cancel` | `<peer>/<key>` | cancel an in-flight send |
| `lxmf.id.<n>.cmd.delete` | `<peer>/<key>`, or bare `<peer>` | delete one message; bare `<peer>` deletes the whole conversation |
| `lxmf.id.<n>.cmd.announce` | any | emit a delivery announce for identity `n` now |
| `lxmf.id.<n>.cmd.ping` | `<peer>` | probe that contact — one packet out, its delivery proof back (see **Ping**) |

To make a sentinel atomic with its data, write the data fields and the
sentinel in one `storageBegin()/storageEnd()` transaction — the firmware
then sees a fully-populated record the instant the sentinel fires.

### Clickable `lxmf@<hash>` links

Micron pages rendered by [nomad](../nomad)'s browser turn an `lxmf@<32-hex>`
token into a clickable link. Activating one writes the destination hash to
one of two ephemeral sentinels:

| Key | Written by | Reaction |
|---|---|---|
| `lxmf.url_web` | the web nomad browser | the lxmf web panel comes forward and opens the conversation |
| `lxmf.url_lcd` | the on-device nomad browser | the on-device LXMF app comes forward (`lcdShowProgram("LXMF")`) and opens the thread |

The value is the 32-hex destination hash, optionally suffixed `:<nonce>` so
re-tapping the same hash re-fires. The lxmf **core task** reacts to *both*
keys: if the destination's identity is unknown it issues a path request, so
the contact's name and send-capability light up once the announce arrives.
The two UI surfaces each react only to their own key. These keys are **not**
self-deleting (the nonce already makes each tap a fresh value).

The reverse also holds: a message body may quote a **Nomad page URL**
(`<32-hex hash>:/path`, e.g. `a8d2…338:/page/index.mu`), which the messenger
renders as a tappable link. Activating one writes `nomad.url_web` /
`nomad.url_lcd` (same nonce trick), and [nomad](../nomad)'s browser comes
forward on that page — the symmetric counterpart of the `lxmf.url_*` keys above.

## Identities

Per loaded identity you can observe:

```
s.lxmf.id.<n>.label          "main" | "imported" | user-set
s.lxmf.id.<n>.enabled        1 (default) — 0 = identity dark: no announce, no send, inbound dropped
s.lxmf.id.<n>.display_name   utf-8, advertised in announces
s.lxmf.id.<n>.default_method  per-identity delivery method (see below); falls
                             back to the global s.lxmf.default_method
lxmf.id.<n>.up               1 once the mailbox is connected
lxmf.id.<n>.dest_hash        hex16 — this identity's lxmf.delivery address
lxmf.id.<n>.link.<peer>      conversation-link state to <peer>, ephemeral:
                             absent (down) | establishing | active
```

From firmware, `lxmf.h` exposes:

```cpp
int  lxmfCreateIdentity (const char* display_name, bool sync = false);
bool lxmfDestroyIdentity(int n,                    bool sync = false);
```

These write the corresponding `lxmf.cmd.*` sentinel. With `sync = true` the
call blocks (≤ 5 s) until the firmware finishes, then returns the allocated
slot (create) or success (destroy). The CLI's `lxmf create` / `lxmf destroy`
use the sync form so they can report the outcome.

## Sending a message

Messages are stored **per contact**: `<peer>` is the 32-hex destination,
`<key>` a local key (convention `o_<unix_ms>_<rand4>`).

1. Write the draft record:

   ```
   s.lxmf.id.<n>.msgs.<peer>.<key>.dir      = out
   s.lxmf.id.<n>.msgs.<peer>.<key>.peer     = <32-hex destination>
   s.lxmf.id.<n>.msgs.<peer>.<key>.title    = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.content  = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.thread   = <hex64 root message_id, or "">
   s.lxmf.id.<n>.msgs.<peer>.<key>.status   = 0            # DRAFT
   ```

   (`peer` is both the path segment and a field — the field is kept for the
   indexed-query contract.) A `DRAFT` record is filtered out of the thread, so
   you may edit it freely; the web composer skips this state entirely, keeping
   typed text in RAM and writing the record already `QUEUED`.

2. Commit with `lxmf.id.<n>.cmd.send = <peer>/<key>` (ideally in the same
   transaction as step 1), or `<peer>/<key>/pn:<32-hex>` to upload it to a
   propagation node instead of delivering it directly.

3. Watch `…status` — a single `u8` code carrying both the lifecycle stage and,
   if it stops, the reason:

   ```
   DRAFT → QUEUED → REQUESTING_PATH → SENDING → AWAITING_PROOF → DELIVERED
                                              ↘ RETRYING_LINK / RETRYING_DELIVERY
                                              ↘ NO_ROUTE | LINK_FAIL | … | CANCELLED
   ```

   The companion `…tries` byte is the try count for the current phase, and
   `tries == 255` is the **one** definitive terminal marker: below it the
   message is still in play and a sweep may retry it, whatever the status says.
   `DELIVERED` means a cryptographic delivery proof (or the proof-grade
   Resource transfer acknowledgement) arrived — nothing else does.

**Delivery method.** lxmf resolves per-message `method` →
`s.lxmf.id.<n>.default_method` → global `s.lxmf.default_method` →
`link-if-one-exists`. The four methods form a spectrum of link eagerness:

| method | behaviour |
| --- | --- |
| `link-always` | always a Reticulum Link. |
| `link-if-one-exists` *(default)* | ride our own warm conversation Link to the peer if one exists, else opportunistic. A peer's inbound Link into us never counts — we only ride Links we opened. |
| `link-if-big` | opportunistic when the wire fits one packet, a Link only when it's oversize. |
| `opportunistic-or-fail` | never a Link; oversize hard-fails with `last_error = "too large for opportunistic"`. |

A message fits opportunistic when `title + content + ~32 B` is within one
packet (budget ~311 B). Oversize forces a Link in every mode **except**
`opportunistic-or-fail`, and a Link carries large bodies as a Resource
transfer. The legacy names still parse: `auto`→`link-if-one-exists`,
`direct`→`link-always`, `opportunistic`→`opportunistic-or-fail`.

**Link toggle.** The conversation header (web and LCD) shows a link icon —
green when a Link to the peer is open, amber while establishing, grey when
down — that tapping opens or closes on demand (`lxmf.id.<n>.cmd.link_open` /
`cmd.link_close`, value `<peer>`; CLI `lxmf link open|close|status <peer>`).
It reflects a Link torn down for any reason, tracking the per-second
`lxmf.id.<n>.link.<peer>` state.

A message that gave up (`tries == 255`) is not retried on its own — write
`cmd.send` again to re-send it.

## Propagation nodes

Classic LXMF **propagation nodes** (reference `lxmf.propagation`
store-and-forward, as run by NomadNet et al.) are supported as a client,
wire-compatible with LXMF 0.9.8:

- **Node list.** A global, ordered list at `s.lxmf.pn.<i>.{hash,name,check}`
  (`i` = 0…7; a slot whose `hash` isn't 32-hex is free). Client-owned —
  view, add, reorder and delete it from the LXMF settings panel (web) or
  the on-device settings pane; the web panel also renames in place.
  `check = 1` marks a node to be polled for messages held for this
  device's identities.
- **Sending.** An outbound message's detail page (web and LCD) has a
  **Resend** button opening a dialog: resend **directly**, via the
  **contact's node** (when one is set on the contact), or via one of **our
  nodes**. A node resend encrypts the wire to the recipient, pays the node's
  announced propagation stamp (a separate, smaller proof-of-work than the
  recipient delivery stamp), and uploads over an `lxmf.propagation` link.
  Success settles the message **`ON_PN`** — final, shown as the single
  "stored for pickup" tick: a propagation node never proves delivery; the
  recipient collects the message on its own next sync. Failures settle
  `PN_FAIL` (link/transfer) or `PN_REJECTED` (the node refused, e.g. an
  under-paid stamp).
- **Per-contact node.** Each contact's details page can name that
  contact's propagation node (any dest hash — a quick-pick of our own
  list plus free entry). It is offered as the resend dialog's second
  option; nothing is sent to it automatically.
- **Receiving.** Every node marked `check` is synced on a timer
  (`s.lxmf.pn.check_interval_s`, default 1800; `0` = manual only) and on
  demand (`lxmf.cmd.pn_sync = all` or a node hash). By hand, that is the
  **envelope button** the messenger shows while at least one node is
  configured — right of the Contacts / On-the-Mesh tabs on the LCD, in the
  window's top-right corner on the web. It says the check will happen in
  the background and restarts the interval, so the next automatic pass is a
  whole interval away. A sync identifies over the link so the node
  serves exactly this identity's mail, downloads the held messages
  through the normal verify/dedup/store pipeline, and confirms them so
  the node deletes its copies. Per-node results land in
  `lxmf.pn.<hash>.{last_check_s,last_err,last_got}` (RAM);
  `lxmf.pn.sync` holds the node currently being checked.

Command sentinels: `lxmf.id.<n>.cmd.send = <peer>/<key>/pn:<hash>`
(resend via node), `lxmf.cmd.pn_sync`, and
`lxmf.cmd.pn_add = <hash>[|<name>]` (append to the list — used by the
on-device pane).

This device does not *run* a propagation node — it is a client only.

## Receiving a message

Inbound messages are verified, de-duplicated, and stored at
`s.lxmf.id.<n>.msgs.<peer>.<message_id>.*` with `status = RECEIVED`,
`dir = in`, `read = 0`. `<peer>` is the sender's 32-hex destination; the
64-hex key is the real LXMF `message_id`. Dedup survives reboots, so the
same message arriving twice is stored once.

The sender is stubbed into the per-identity address book at
`s.lxmf.id.<n>.contacts.<peer>.*` (with `trust = 0`) on first contact, and
`last_seen` is refreshed. To mark a message read, set `…read = 1` (the
firmware ignores this field — it is purely for your UI).

A message from a sender the device can't yet verify (it has never heard that
sender's announce, so rnsd has no cached public key) is **buffered, not
dropped**: the raw wire is parked in a small per-identity queue (25 deep,
30-minute TTL, oldest evicted on overflow) and the device asks the network for
a path — which prompts the sender to re-announce. Once the sender's identity
is known the buffered message is replayed, verified, and stored. Opportunistic
LXMF has no retransmission, so buffering is what keeps a single-packet message
from a not-yet-known sender from being lost.

## Delivery status & proofs

One `u8` `status` code per message carries the lifecycle stage, the terminal
outcome and the give-up reason together. The names below are what the CLI and
both frontends print; the numbers are persisted, so the list is append-only
(`LxmfStatus` in `esp-idf/include/lxmf.h`, mirrored in the browser's
`modules/lxmf.ts`).

| group | statuses | meaning |
|---|---|---|
| progress | `DRAFT` `QUEUED` `REQUESTING_PATH` `SENDING` `AWAITING_PROOF` `RETRYING_LINK` `RETRYING_DELIVERY` | still in play; a sweep may act |
| settled | `DELIVERED` `CANCELLED` `RECEIVED` | proof received / user cancelled / inbound |
| gave up | `NO_PROOF` `NO_ROUTE` `TOO_LARGE` `LINK_FAIL` `LINK_OPEN_FAIL` `RES_SEND` `RADIO_BUSY` `OUTBOX_FULL` … | why it stopped |
| in someone else's custody | `REMOTE_RLPG` `OUR_RLPG` `ON_PN` `REMOTE_RLPG_FULL` `REMOTE_RLPG_ERR` `PN_FAIL` `PN_REJECTED` | mailbox / propagation-node states |

The companion `tries` byte, not the status, is the definitive terminal marker:
`tries == 255` means gave up, and below that the message is still live whatever
the status reads. A one-shot status sets it to 255 the moment it occurs.

**Egress is not delivery.** Opportunistic packets get no native
acknowledgement, so a proof timeout is *not* a failure — the message may well
have arrived, the peer may not prove inbound, or the proof was lost. That
settles as `NO_PROOF`, distinct from `DELIVERED`, which only a cryptographic
delivery proof (or the proof-grade Resource transfer acknowledgement) produces.

Both frontends render this on outbound bubbles as the ALL-CAPS status name
plus a glyph: grey `…` while in play, two green checks for `DELIVERED` (which
needs no name), a grey ✕ for `CANCELLED`, a red ✕ once `tries` hits 255. A
message sitting in someone else's custody — parked at an RLPG mailbox
(`REMOTE_RLPG`/`OUR_RLPG`) or uploaded to a propagation node (`ON_PN`) — gets a
single open-circle tick: stored for pickup, no proof of arrival.

## Ping

```
us ──probe packet (peer_dest | our_dest)──►  peer
us ◄──────── delivery proof (+ rx report) ── peer
```

The contact detail page's **Ping** button (web and LCD, plus `lxmf ping <peer>`)
measures a contact: round-trip time, hop count, and — when the link is a direct
radio one — the transmit power and received signal at *both* ends, from one
packet each way.

It is `rnprobe lxmf.delivery <hash>` with the one difference that matters, which
is who it comes from. `rnprobe` sends from rnsd's own identity with a zero
payload, so the far end sees a sender it holds no contact record for and answers
with a plain proof. The probe here goes out on the identity's own destination
with our `lxmf.delivery` hash as the first sixteen bytes of the plaintext —
exactly where the peer's rnsd looks for the sender — so we are recognised as a
contact that advertised the rx-report capability and the proof comes back
**extended**, carrying the peer's own rx of us and the power it answered at (see
[rns](../rns)). The payload is that source hash and nothing else: it is not a
valid LXM wire, and the peer's lxmf names it as a probe and drops it *after* its
rnsd has already proved it, since proving happens on hand-off and before any
parsing.

Results are published per peer, RAM-only, overwritten by the next probe:

```
lxmf.ping.<peer>.state       probing | path | ok | no-proof | no-route |
                             timeout | cancelled | failed | offline
lxmf.ping.<peer>.ts          unix seconds of the last state change
lxmf.ping.<peer>.rtt_ms      round trip; present on `ok`
lxmf.ping.<peer>.hops
lxmf.ping.<peer>.tx          our antenna tx power, dBm
lxmf.ping.<peer>.rssi/.snr   our rx of the delivery proof
lxmf.ping.<peer>.peer_tx     the peer's antenna tx power, dBm
lxmf.ping.<peer>.peer_rssi/.peer_snr   the peer's rx of our probe
```

Each side's `tx` pairs with the *other* side's `rssi`, which is what makes the
pair a path loss rather than two unrelated numbers. Every surface renders the
two directions on that pairing — `us->them` is our `tx` with the peer's
`peer_rssi`, `them->us` is `peer_tx` with our `rssi` — never a side's own tx
next to its own rx, which measures nothing. Only `state` is always present: a
hop that isn't radio has no signal to report at either end, and those keys are
simply absent rather than zero.

A peer that appends no rx report (any client that isn't ours) leaves the
`peer_*` half unmeasured, so both directions would render mostly as
placeholders — which reads as a measurement that failed rather than one that
was never offered. Every surface collapses that case to one sentence of what we
do hold, round trip included, so it replaces the reading rather than sitting
under a header that repeats it:

```
Probe sent at 17 dBm, proof RSSI -94 dBm / SNR 8.5 dB, 1 hop, round trip 1102 ms.
```

In the CLI it follows the command's own outcome and wait:

```
ok after 1204 ms | Probe sent at 17 dBm, proof RSSI -94 dBm / SNR 8.5 dB, 1 hop, round trip 1102 ms.
```

One ping per identity is in flight at a time; pressing again supersedes rather
than queues, and a probe that draws no result at all settles `timeout` after
20 s so the display never sticks on `probing`.

The buttons fire and forget — the keys above are what they render. `lxmf ping`
instead holds the prompt until `state` settles, then prints the outcome with
the time it waited. It waits by reading the client's input in 100 ms slices,
which is what makes **Ctrl-C** (or Ctrl-D) end the wait, keeps the cli task's
ITS inbox serviced so new CLI connections aren't refused for the probe's
duration, and ends the wait when the session closes. Cancelling ends the
*wait*, not the probe: it is the lxmf task's, and its result still lands under
`lxmf.ping.<peer>`. Without an interactive session (cron, `spangap cli`) there
is nothing to read, and a plain delay paces the loop instead.

The 20 s settle must stay inside rnsd's proof window (`s.rnsd.proof_timeout_s`):
a probe that gives up before the transport does reports `no-proof` for a proof
still in flight.

## Announces

- Each enabled identity announces ~30 s after startup and after each
  interface-up debounce, then every `s.lxmf.announce_interval_s` seconds
  (default 1800; `0` disables periodic). Force one with
  `lxmf.id.<n>.cmd.announce`.
- Every `lxmf.delivery` announce the device hears is written to the
  **announce catalogue**, one packed leaf per destination:

  ```
  lxmf.announces.<dest_hex> = "<last_s>|<cost>|<hops>|<ratchet>|<name>"
  ```

  It is ephemeral (RAM), bounded by `s.lxmf.max_announces` (default 2048,
  `0` disables eviction; oldest evicted on overflow), and is the source for
  a "people we've heard of" picker. It is distinct from per-identity
  `contacts`, which is each identity's own address book.

## Stamps (proof-of-work)

LXMF lets a recipient advertise a **stamp cost** in its announce — a
proof-of-work price (in bits; each bit doubles the work) a sender pays per
message as spam friction. Three knobs, all surfaced in the Settings panel
and the on-device settings pane:

- **`s.lxmf.stamp_cost`** (slider 0–18, default 8): the single cost we
  advertise to everyone. `0` advertises none. The cap of 18 keeps it
  generatable on-device; validation is cheap at any cost. This is *only*
  what we advertise.
- **`s.lxmf.generate_stamps`** (default 1): when sending to a peer who
  advertises a cost > 0, compute a stamp meeting it and append it. The PoW
  runs on the lxmf task (~4 s on a T-Deck, dominated by the one-time
  768 KB workblock build, so cost barely matters over this range); skipped
  entirely when the peer advertises no cost. A peer advertising above 18 is
  refused (sent unstamped) rather than freezing the task for minutes.
- **`s.lxmf.enforce_stamps`** (default 0): when on, drop inbound messages
  lacking a valid stamp for the cost we advertise.

Both generation and validation yield ~every 500 ms so the rest of the system
keeps ticking.

Uploading to a propagation node pays a **second, separate stamp** on top of
whatever the recipient's own cost put in the message: the node's announced
propagation cost, over a third-size workblock (256 KB against the recipient
stamp's 768 KB). It is paid whenever the node's announce advertises one — the
three knobs above govern only the recipient stamp. A node whose announce we
have never heard is sent an unstamped upload rather than one paying a guessed
cost, and it may reject it (`PN_REJECTED`).

## Storage variables

### Settings (`s.lxmf.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.lxmf.announce_interval_s` | `1800` | Periodic re-announce seconds; `0` = on demand only. |
| `s.lxmf.max_announces` | `2048` | Announce-catalogue entry cap; `0` = no eviction. |
| `s.lxmf.stamp_cost` | `8` | Advertised PoW cost (bits, 0–18; `0` = none). |
| `s.lxmf.generate_stamps` | `1` | Pay a peer's advertised stamp cost when sending. |
| `s.lxmf.enforce_stamps` | `0` | Drop inbound without a valid stamp for our cost. |
| `s.lxmf.link_timeout` | `0` | Conversation-Link establishment budget, seconds; `0` = let rnsd derive it from the next hop's interface speed. |
| `s.lxmf.link.idle_s` | `600` | Close a conversation Link idle past this many seconds (10 min); `0` = keep open (LRU at the 4-link cap and Reticulum's STALE teardown still bound it). |
| `s.lxmf.pn.<i>.hash` | — | Propagation-node list, index-ordered (`i` = 0–7); non-32-hex = free slot. |
| `s.lxmf.pn.<i>.name` | `""` | Optional display name for that node. |
| `s.lxmf.pn.<i>.check` | `1` | Poll this node for held messages. |
| `s.lxmf.pn.check_interval_s` | `1800` | Propagation-node check cadence; `0` = manual only. |
| `s.lxmf.sound` | `/fixed/lxmf/ding.wav` | Message-notification WAV (point at your own device-rate file if you like). |
| `s.lxmf.sound_enabled` | `1` | Play the notification sound on inbound delivery. |
| `s.lxmf.debug.only_local` | `0` | Demote per-announce catalogue debug logs to verbose. |
| `s.lxmf.cli.selected_id` | `0` | The CLI's selected identity. |

`s.lxmf.max_resource_size` (default 262144) gates the largest inbound
Resource and is consumed by rnsd, not lxmf — it is documented in
[rns](../rns).

### Per-identity (`s.lxmf.id.<n>.*`)

```
label            "main" | "imported" | user-set
enabled          1 (default); 0 = dark
display_name     utf-8, advertised in announces
default_method   link-always | link-if-one-exists | link-if-big | opportunistic-or-fail
                 (empty ⇒ inherit global s.lxmf.default_method, default link-if-one-exists)
contacts.<peer>.{hash,nick,display_name,trust,last_seen,pn}   address book (firmware stubs on first inbound/outbound; display_name follows the peer's announces; pn = this contact's propagation node, all-zero = none)
msgs.<peer>.<key>.{dir,status,tries,peer,title,content,thread,method,ts,recv_ts,
                   read,wire,message_id}    per-conversation message records
```

### Runtime & telemetry (`lxmf.*`, RAM)

```
lxmf.up                          task alive
lxmf.id.<n>.up                   identity's mailbox connected
lxmf.id.<n>.dest_hash            hex16 lxmf.delivery address
lxmf.id.<n>.last_announce_s      unix seconds of last announce
lxmf.id.<n>.stats.{sent,received,pending,failed}
lxmf.announces.<dest_hex>        "<last_s>|<cost>|<hops>|<ratchet>|<name>"
lxmf.msgmeta.<message_id>.{last,hops,first_hop,dir,iface,rssi,snr,remote_rssi,remote_snr}
                                 per-message routing + radio-signal telemetry (RAM, browser-mirrored).
                                 rssi/snr = our rx — of the message (inbound) or of its delivery proof
                                 (outbound). remote_rssi/remote_snr = the peer's rx of an outbound
                                 message we sent, from its rx-report proof (reticulous peers only).
lxmf.contactsig.<peer>.{rssi,snr}   per-contact direct signal: our rx of the last zero-hop radio packet
                                 from that peer; deleted when a relayed or non-radio packet supersedes it.
lxmf.ping.<peer>.*               latest probe result for that contact — see Ping above.
```

**Signal display.** A received message shows either its radio signal (amber bars)
when it reached us direct, or an "L" when it was relayed — never both. When a
reticulous peer has reported its own rx of an outbound message (via the rx-report
proof), the bars render as a two-set "valley" (remote descending, then local
ascending). The contacts list and conversation header show the per-contact
direct signal (`lxmf.contactsig.*`), the header falling back to the gateway
signal (`rnsd.gw.*`) when there's no direct sample. The message-info page lists
the numeric rssi/snr (and remote rssi/snr when present).

### Secrets

```
secrets.lxmf.id.<n>.privkey      128-hex Ed25519+X25519 key (wiped by identity_destroy)
```

## CLI — `lxmf`

All verbs act on the **selected identity** (`s.lxmf.cli.selected_id`,
default 0) unless noted.

```
lxmf create <name>          generate a new identity (prints the slot, or failure)
lxmf destroy <n>            wipe identity at slot <n> (secrets + storage)
lxmf id                     list identities (* = selected)
lxmf id <n>                 switch selected identity
lxmf chats                  list conversations (one row per peer; numbered)
lxmf msgs [<arg>]           no arg = chats; <peer> = that thread (newest first);
                            a bare status name = cross-conversation filter
                            (case-insensitive, e.g. `lxmf msgs delivered`)
lxmf read <n>               print message #n from the last `lxmf msgs`; marks it read
lxmf c[ontacts] [<arg>]     list this identity's contacts (numbered); <arg> =
                            case-insensitive substring of display_name or nick
lxmf announces [<arg>]      cross-identity announce catalogue; <arg> = 32-hex
                            (one row) or a name substring; no arg = full dump
lxmf send <peer> <msg>      send; <peer> = 32-hex, a number from the last numbered
                            listing, or a name/nick substring
lxmf a[nnounce]             announce the selected identity now
lxmf p[ing] <peer>          probe <peer> (same forms as `send`); holds the prompt
                            until the probe settles, its 20 s timeout expires, or
                            Ctrl-C, then prints the outcome, the wait, and both
                            ends' signal
```

Numbered listings (`chats`, `msgs`, `contacts`, `announces`) feed the index
arguments of `read` / `send` / `ping` / `msgs <#>`. A `<peer>` substring is
matched case-insensitively against this identity's contacts (display_name and
nick) first, then the announce catalogue; multiple matches print a numbered
disambiguation list instead of acting, so the retry can pick a line number. Run
any of these on-device with `spangap cli "<command>"`.

## Frontends

**Settings** (Settings → Reticulum Mesh → LXMF Messages): described by the
`settings:` block in `straddle.yaml` and lowered by the build to both surfaces.
The four identity slots are rows gated on the slot being occupied; the
propagation nodes are a collection over the `lxmf.pnode.*` sentinels; the create
and import forms submit to `lxmf.identity.*`, where the 128-hex and 32-hex rules
are stated once instead of as a regex per UI.

**Browser** (`browser/`, registered via `registerLxmf`): a Pinia store + RPC
(`modules/lxmf.ts`), the chat window (`panels/MessagesWindow.vue`), and the chat
components
(`components/lxmf/`: `PeerAvatar`, `ConversationList`, `ContactCard`,
`MessageBubble`, `Composer`, `AnnouncesView`, `ConversationThread`).

**On-device LCD app** (`esp-idf/conditional/spangap-lcd/src/lxmf_lcd.cpp`):
the **LXMF** app, an `LcdApp` installed via `lcdInstall(new LxmfApp())`. The
whole file lives
under `conditional/spangap-lcd/` and is compiled and registered only when
the [spangap-lcd](../spangap-lcd) straddle is in the build (the
`lxmfLcdRegister` init hook is `when:`-gated) — no `#if` anywhere.

Both frontends share the contact-info pattern: clicking anywhere on a
conversation's header (or, on the LCD, a contact row's circled-i — the info
icon is a cue, not the sole target) opens a per-peer info page showing the
destination hash grouped in fours for eye comparison (the web copy button
still yields the bare unspaced hex) and holding the delete-conversation
flow behind an explicit "Are you sure?" confirm. The page's back chevron
returns to whichever screen opened it — contact overview or message view.

The page opens with a row of buttons, each sized to its own text: **Delete**
(the conversation, behind the confirm) and **Ping**. The web Ping result is a
popover under the button, dismissed by a touch anywhere; the LCD's is inline
under the button row instead, which leaves the button reachable so a re-measure
is one press.

## What it owns

```
lxmf/
├── esp-idf/
│   ├── include/
│   │   ├── lxmf.h          public API (lxmfInit, lxmfCreateIdentity, lxmfDestroyIdentity)
│   │   └── lxmf_stamp.h    stamp generate/validate
│   ├── src/
│   │   ├── lxmf.cpp        the lxmf task: identities, mailbox, send/recv, announces
│   │   └── lxmf_stamp.cpp  LXStamper-compatible PoW (self-contained SHA-256/HMAC/HKDF)
│   ├── conditional/spangap-lcd/src/lxmf_lcd.cpp   on-device LXMF app (LVGL)
│   └── data/lxmf/ding.wav                          notification sound → /fixed/lxmf/ding.wav
└── browser/
    └── src/{modules,panels,components}/…           web UI (see Frontends)
```

## Dependencies

- [rns](../rns) — the Reticulum stack; lxmf is a consumer over rnsd's ITS
  ports and byte-array API. Interfaces are decoupled — pick whichever ones
  suit the deployment.
- [audio](../audio) — soft, default-on dependency (`spangap/audio`) for the
  notification sound; pruned silently when absent (every call site is gated,
  so a build without it still links, just with no sound).

## Read next

- [INTERNALS.md](INTERNALS.md) — the wire codec, the ITS framing, the
  task/threading model, the identity model, our deltas from upstream LXMF,
  and maintainer pitfalls.
