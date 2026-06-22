# lxmf

## What is this?

**lxmf** is LXMF messaging on
[rns](../rns): multi-identity opportunistic
single-packet delivery plus Link + Resource transfer for larger
messages. DIRECT delivery uses **persistent per-peer conversation
Links** (upstream-LXMF style): the first direct send to a peer opens a
Link that is then kept and reused for the whole chat — for our sends
and for the peer's deliveries back over it — and left open (set
`s.lxmf.link.idle_s` to close idle links after that many seconds;
default 0 = never — LRU eviction at the 4-link cap and Reticulum's own
STALE teardown bound the cost). Set `s.lxmf.link_timeout` (seconds) to
override how long a conversation Link may sit establishing before it
fails; default 0 lets rnsd derive the budget from the next hop's
interface speed. While a
conversation Link is warm, even small messages ride it instead of going
opportunistic. Storage is the API — every frontend (browser, CLI,
on-device LCD) reads/writes the `s.lxmf.*` / `lxmf.*` / `secrets.lxmf.*`
keys, and the lxmf task subscribes to its own subtree. No frontend ever
talks to lxmf via ITS directly.

[LXMF](https://github.com/markqvist/LXMF) is Mark Qvist's
"Lightweight Extensible Message Format" — the canonical
human-messaging layer on Reticulum.

## What this straddle owns

```
lxmf/
├── esp-idf/
│   ├── include/lxmf.h
│   ├── src/lxmf.cpp         the lxmf task: multi-identity, mailbox, peers
│   └── lcd/src/lxmf_lcd.cpp  on-device LXMessenger LVGL program (slice)
└── browser/
    └── src/
        ├── modules/lxmf.ts             Pinia + RPC
        ├── panels/LxmfPanel.vue        Settings → LXMF
        ├── panels/MessagesWindow.vue   chat window (FloatingWindow)
        ├── panels/AnnouncesWindow.vue  live announce feed
        └── components/lxmf/            ConversationList, Composer,
                                        MessageBubble, PeerPicker, …
```

The LCD slice (`esp-idf/lcd/src/lxmf_lcd.cpp`) is the on-device
LXMessenger program — picked up by the [spangap-lcd](../../s/spangap-lcd)
activator and wrapped in `#if CONFIG_SPANGAP_LCD`.

## How others use it

```cpp
lxmfInit();    // after rnsdInit
```

The **API is storage**. Frontends:

- Read peer / message lists from `lxmf.directory.<32hex>.<field>`,
  `lxmf.threads.<peerhash>.*`.
- Write commands to `lxmf.cmd.*` and `lxmf.id.<n>.cmd.*` sentinels;
  lxmf subscribes only to those subtrees and processes each sentinel
  inline.
- Identities are under `s.lxmf.identities.*` (public) and
  `secrets.lxmf.identities.*` (private keys).

**No auto-create at boot.** Devices without identities run as transport-
only nodes. An identity is created explicitly via `lxmf identity
create` (CLI), or via the Settings panel.

## Message stages & delivery proofs

Outbound message records (`s.lxmf.id.<n>.msgs.<peer>.<key>.stage`) move
through:

| stage       | meaning                                                        |
| ----------- | -------------------------------------------------------------- |
| `draft`     | composed, not yet handed to the task                           |
| `queued`    | accepted; waiting (e.g. for a busy conversation link)          |
| `sending`   | in flight (path request, link establishment, transfer)         |
| `sent`      | **egressed / transfer accepted** — no proof of arrival (yet)   |
| `delivered` | **cryptographic delivery proof received**, or the Resource transfer ACK (which is proof-grade) |
| `failed`    | terminal error (`last_error` says why)                         |
| `cancelled` | user cancelled                                                 |
| `received`  | inbound messages                                               |

A proof timeout is **not** a failure: the message may well have arrived
(the peer may not prove incoming packets, or the proof was lost). The
stage stays `sent` and `last_error` is set to `no delivery proof`.

Both UIs render the stage on outbound bubbles:

- `queued` / `sending` — small grey `…`
- `sent` — one grey checkmark
- `delivered` — two green checkmarks
- `failed` / `cancelled` — red ✕ (web shows `last_error` as tooltip)

## Stamps (proof-of-work)

LXMF lets a recipient advertise a **stamp cost** in its announce — a
proof-of-work price (in bits; each bit doubles the work) a sender must
pay per message as spam friction. Three independent knobs (all surfaced
in the Settings panel / on-device pane):

- **`s.lxmf.stamp_cost`** (slider 0–18, default 16): the single cost we
  advertise to everyone. `0` advertises no cost. The cap of 18 keeps it
  in the range we can still generate ourselves with acceptable effort;
  validation is cheap at any cost. This is *only* what we advertise —
  whether we act on it on inbound is the separate toggle below.
- **`s.lxmf.generate_stamps`** (default on): when sending to a peer who
  advertises a cost > 0, compute a stamp meeting it and append it. The
  PoW runs on the lxmf task and takes ~4 s on a T-Deck (dominated by the
  one-time workblock build, so cost has little effect over this range);
  skipped entirely (no delay) when the peer advertises no cost — the
  common case. A peer advertising above 18 is refused (sent unstamped)
  rather than freezing the task for minutes. Turn off to never pay.
- **`s.lxmf.enforce_stamps`** (default off): when on, drop inbound
  messages lacking a valid stamp for the cost we advertise
  (`s.lxmf.stamp_cost`). Left off, inbound stamps are accepted but not
  required.

Both generation and validation run on the lxmf task and yield ~every
500 ms (covering the workblock build, not just the search) so the rest of
the system keeps ticking.

A stamp is a 32-byte nonce over a 768 KB workblock expanded from the
message_id (reference LXStamper, 3000 HKDF rounds — fixed by the
protocol). The stamp is payload element [4], appended after signing, so
it is neither signed nor part of the message_id; the receiver strips it
to re-derive both. Tickets (the contact-exemption mechanism) are not yet
implemented — every stamped send pays the full PoW.

## Dependencies

- [rns](../rns)
- (transports are decoupled — pick whichever ones suit the deployment)

## Read next

- [INTERNALS.md](INTERNALS.md) — task layout, sentinel cmd surface,
  storage shape, multi-identity model.
- Deep-dive in the consuming app:
  [docs/lxmf.md](../hw-tdeck/docs/lxmf.md) — black-box view;
  [docs/internals/lxmf.md](../hw-tdeck/docs/internals/lxmf.md)
  — the reach-inside view (upstream LXMF 0.9.8 summary, our deltas,
  full schema, phasing).
