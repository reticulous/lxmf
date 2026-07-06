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
- **Automatic delivery mode.** Per message, lxmf picks a single
  opportunistic packet for small messages, a Reticulum Link (DIRECT)
  otherwise, and a Resource transfer for large bodies — transparently. While
  a per-peer conversation Link is warm, even small messages ride it.
- **Announces.** Each enabled identity periodically announces its delivery
  destination. Every inbound `lxmf.delivery` announce on the mesh is
  collected into a shared, cross-identity **announce catalogue** of everyone
  the device has heard of.
- **Stamps.** Pays and (optionally) enforces LXMF proof-of-work stamps as
  spam friction.
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
  owns `stage`, `wire`, `message_id`, `attempts`, `last_error`.

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

**Per-identity** (`lxmf.id.<n>.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.id.<n>.cmd.send` | `<peer>/<key>` | pack, sign, and transmit the draft at `s.lxmf.id.<n>.msgs.<peer>.<key>` |
| `lxmf.id.<n>.cmd.cancel` | `<peer>/<key>` | cancel an in-flight send |
| `lxmf.id.<n>.cmd.delete` | `<peer>/<key>`, or bare `<peer>` | delete one message; bare `<peer>` deletes the whole conversation |
| `lxmf.id.<n>.cmd.announce` | any | emit a delivery announce for identity `n` now |

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
s.lxmf.id.<n>.default_method  auto (default) | opportunistic | direct
lxmf.id.<n>.up               1 once the mailbox is connected
lxmf.id.<n>.dest_hash        hex16 — this identity's lxmf.delivery address
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
   s.lxmf.id.<n>.msgs.<peer>.<key>.stage    = draft
   ```

   (`peer` is both the path segment and a field — the field is kept for the
   indexed-query contract.) While `stage == draft` you may edit freely.

2. Commit with `lxmf.id.<n>.cmd.send = <peer>/<key>` (ideally in the same
   transaction as step 1).

3. Watch `…stage` progress:

   ```
   draft → queued → sending → sent          (delivered only on a proven DIRECT/Resource transfer)
                            ↘ failed | cancelled
   ```

   `last_error` carries a short human string during the attempt
   (`requesting path`, `establishing link`, …); `attempts` counts retries.

**Delivery method is automatic.** lxmf resolves
per-message `method` → `s.lxmf.id.<n>.default_method` → `auto`. Under `auto`
a message goes opportunistic when `title + content + ~32 B` fits one packet
(budget ~311 B) **and** no conversation Link to that peer is already warm;
otherwise it goes DIRECT (a Link), with a Resource transfer for large
bodies. An explicit `method = opportunistic` that exceeds the budget fails
with `last_error = "too large for opportunistic"`.

There is **no automatic retry** — to re-send after `stage = failed`, write
`cmd.send` again.

## Receiving a message

Inbound messages are verified, de-duplicated, and stored at
`s.lxmf.id.<n>.msgs.<peer>.<message_id>.*` with `stage = received`,
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

## Delivery stages & proofs

| stage | meaning |
|---|---|
| `draft` | composed, not yet handed to the task |
| `queued` | accepted; waiting (e.g. for a busy conversation Link) |
| `sending` | in flight (path request, link establishment, transfer) |
| `sent` | egressed / transfer accepted — no proof of arrival |
| `delivered` | cryptographic delivery proof received, or the proof-grade Resource transfer ACK |
| `failed` | terminal error (`last_error` says why) |
| `cancelled` | user cancelled |
| `received` | inbound message |

`sent` is **not** `delivered`. Opportunistic packets get no native ack — a
proof timeout is *not* a failure (the message may have arrived; the peer may
not prove inbound, or the proof was lost). The stage stays `sent` with
`last_error = "no delivery proof"`. Only a proven DIRECT/Resource transfer
reaches `delivered`.

Both frontends render the stage on outbound bubbles: `queued`/`sending` →
grey `…`, `sent` → one grey check, `delivered` → two green checks,
`failed`/`cancelled` → red ✕ (web shows `last_error` as a tooltip).

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

- **`s.lxmf.stamp_cost`** (slider 0–18, default 16): the single cost we
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

## Storage variables

### Settings (`s.lxmf.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.lxmf.announce_interval_s` | `1800` | Periodic re-announce seconds; `0` = on demand only. |
| `s.lxmf.max_announces` | `2048` | Announce-catalogue entry cap; `0` = no eviction. |
| `s.lxmf.stamp_cost` | `16` | Advertised PoW cost (bits, 0–18; `0` = none). |
| `s.lxmf.generate_stamps` | `1` | Pay a peer's advertised stamp cost when sending. |
| `s.lxmf.enforce_stamps` | `0` | Drop inbound without a valid stamp for our cost. |
| `s.lxmf.link_timeout` | `0` | Conversation-Link establishment budget, seconds; `0` = let rnsd derive it from the next hop's interface speed. |
| `s.lxmf.link.idle_s` | `600` | Close a conversation Link idle past this many seconds (10 min); `0` = keep open (LRU at the 4-link cap and Reticulum's STALE teardown still bound it). |
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
default_method   auto (default) | opportunistic | direct
contacts.<peer>.{hash,nick,display_name,trust,last_seen}   address book (firmware stubs on first inbound)
msgs.<peer>.<key>.{dir,stage,peer,title,content,thread,method,ts,read,
                   wire,message_id,attempts,last_error}    per-conversation message records
```

### Runtime & telemetry (`lxmf.*`, RAM)

```
lxmf.up                          task alive
lxmf.id.<n>.up                   identity's mailbox connected
lxmf.id.<n>.dest_hash            hex16 lxmf.delivery address
lxmf.id.<n>.last_announce_s      unix seconds of last announce
lxmf.id.<n>.stats.{sent,received,pending,failed}
lxmf.announces.<dest_hex>        "<last_s>|<cost>|<hops>|<ratchet>|<name>"
```

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
                            a bare <stage> word = cross-conversation filter
lxmf read <n>               print message #n from the last `lxmf msgs`; marks it read
lxmf contacts               list this identity's contacts (numbered)
lxmf announces [<arg>]      cross-identity announce catalogue; <arg> = 32-hex
                            (one row) or a name substring; no arg = full dump
lxmf send <peer> <msg>      send; <peer> = 32-hex, a number from the last numbered
                            listing, or a name substring
lxmf announce               announce the selected identity now
```

Numbered listings (`chats`, `msgs`, `contacts`, `announces`) feed the index
arguments of `read` / `send` / `msgs <#>`. A name substring with multiple
matches prints a disambiguation list instead of sending. Run any of these
on-device with `spangap cli "<command>"`.

## Frontends

**Browser** (`browser/`, registered via `registerLxmf`): a Pinia store + RPC
(`modules/lxmf.ts`), the Settings panel (`panels/LxmfPanel.vue`), the chat
window (`panels/MessagesWindow.vue`), and the chat components
(`components/lxmf/`: `PeerAvatar`, `ConversationList`, `ContactCard`,
`MessageBubble`, `Composer`, `AnnouncesView`, `ConversationThread`).

**On-device LCD app** (`esp-idf/conditional/spangap-lcd/src/lxmf_lcd.cpp`):
the **LXMF** app, an `LcdApp` installed via `lcdInstall(new LxmfApp())` and a
settings pane registered under `Mesh Network/LXMF`. The whole file lives
under `conditional/spangap-lcd/` and is compiled and registered only when
the [spangap-lcd](../spangap-lcd) straddle is in the build (the
`lxmfLcdRegister` init hook is `when:`-gated) — no `#if` anywhere.

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
