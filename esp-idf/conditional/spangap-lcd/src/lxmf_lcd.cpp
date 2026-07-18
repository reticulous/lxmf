/**
 * lxmf_lcd.cpp — on-device "LXMessenger" launcher program (LVGL).
 *
 * Models the web UI LXMF messenger across three screens within the one program
 * layer (mirroring the Settings program's page model):
 *   - Identity picker: shown first only when more than one identity is usable
 *     (up + announcing). A tap selects the slot and the program continues into
 *     the list — the web spawns one Messages window per identity; on a single
 *     launcher tile we pick up front instead. One usable identity skips it;
 *     none falls through to the list's "create an identity" guidance.
 *   - List: two tabs, each with its own search box on top (focused on entry so
 *     a hardware keyboard types straight into it). Contacts is selected first.
 *       Contacts:    peers you've messaged, newest-comms first; name +
 *                     last-message preview, a last-heard-announce age badge at
 *                     the right, and a circled-i button opening the contact
 *                     info page (which holds the delete-conversation flow). A
 *                     bare 32-hex query offers "message this address".
 *       On the Mesh:  every dest heard announcing within s.lxmf.on_mesh_expire
 *                     seconds (default 3600; 0 = all), newest first, age-badged.
 *                     No dedup with Contacts. The announce catalogue can hold
 *                     thousands, so it's capped (MESH_ROW_CAP) with a footer
 *                     noting the remainder; the search box narrows it.
 *   - Thread:   a top bar (back chevron + peer name; tapping anywhere else on
 *     it opens the contact info page; a scroll-to-bottom chevron at the right
 *     appears only while the view isn't already at the bottom), the message
 *     bubbles (in left / out right), and a compose row (textarea + Send) at
 *     the end of the chat. The compose field is focused when the thread opens.
 *     The thread runs fullscreen (no system status bar).
 *   - Contact info: peer name + the destination hash grouped in fours + a
 *     last-heard line + a Delete-conversation button behind an explicit
 *     "Are you sure?" confirm. Opened from a contact row's circled-i or the
 *     thread top bar; its back chevron returns to whichever screen opened it.
 *
 * Layout is tuned for density: 1px vertical padding throughout, a smaller font,
 * single-line conversation summaries. Message/preview/name text is run through
 * printable() — control bytes and any codepoint the font can't draw are dropped,
 * so unrenderable unicode never leaves placeholder boxes (printable codepoints
 * the font does carry, e.g. accented Latin, pass through unchanged).
 *
 * Storage is the API (same keys the browser uses):
 *   s.lxmf.id.<n>.msgs.<peer>.<key>.{dir,content,ts,read,stage}   messages
 *   s.lxmf.id.<n>.contacts.<peer>.display_name                    names
 *   lxmf.id.<n>.up / .dest_hash                                   identity live
 *   lxmf.announces.<hex> = "<last>|<cost>|<hops>|<ratchet>|<name>" heard peers
 *   lxmf.id.<n>.cmd.send = "<peer>/<key>"                         send sentinel
 * Everything runs on the lcd task; storage subscriptions are dispatched there,
 * so we touch LVGL straight from the change callback.
 */
#include "lcd.h"
#include "lcd_app.h"   /* LcdApp + lcdInstall */
#include "lcd_input_box.h"   /* generic auto-grow text entry + caret behaviours */
#include "lxmf_app.h"  /* LxmfApp — this straddle's services: class */
#include "lxmf.h"      /* LxmfStatus / lxmfStatusName — shared message-status enum */
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "esp_timer.h"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace {

/* ---- look & feel (dense) ---- */

/* The vector UI face at the platform zoom (was the montserrat_12 bitmap). Set by
 * refreshFont() at the top of each top-level build, before any label or the
 * printable() coverage check reads it. All UI + its storage callbacks run on the
 * lcd task, so the (non-thread-safe) vector glyph lookups are safe. The bitmap
 * default only covers the pre-build window. */
const lv_font_t* kFont = &lv_font_montserrat_12_latin;
const lv_font_t* kFontSmall = &lv_font_montserrat_12_latin;   /* meta time / date pills */
const lv_font_t* kFontTiny  = &lv_font_montserrat_12_latin;   /* status name (a touch smaller) */
void refreshFont() {
    kFont      = lcdFont(LcdFace::UI, lcdPx(14));
    kFontSmall = lcdFont(LcdFace::UI, lcdPx(11));
    kFontTiny  = lcdFont(LcdFace::UI, lcdPx(9));
}

const int HDR_H = 30;   /* thread top bar height — tall: the bar itself is the tap
                           target that opens the contact info page */

/* On-the-mesh render cap: heard announces can number in the thousands
 * (s.lxmf.max_announces defaults to 2048). One LVGL row per peer would blow the
 * heap and stall the list, so we sort by recency and draw at most this many; a
 * footer notes the remainder, and the search box narrows the column. */
const int MESH_ROW_CAP = 48;

/* Debounce the search: rebuildList walks the message store + the (up to
 * thousands) announce catalogue and re-creates every row, which runs on the lcd
 * task at a higher prio than the core-1 keyboard poll. Doing it inline on every
 * keystroke stalls that poll long enough that the C3 keyboard (which holds only
 * the last unread key, no real FIFO) overwrites keys typed during the rebuild —
 * they go missing. So coalesce: rebuild once the typing pauses this long. */
const int SEARCH_DEBOUNCE_MS = 300;

/* Thread pagination. A long conversation renders only a window of bubbles: the
 * newest PAGE_SIZE on open, extended by "load earlier"/"newer". PAGE_MIN is the
 * runt-guard — a "newer" gap this small snaps straight to the full newest page
 * instead of minting a sliver. MAXSPAN bounds the window once you page back into
 * history (while pinned to newest it grows freely, keeping the reconcile append-
 * only for live messages). */
const size_t PAGE_SIZE = 40, PAGE_MIN = 12, MAXSPAN = 80;

/* Resident scroll-list top pad. scrollThreadBottom grows it past this to absorb
 * slack so the compose entry's bottom always meets the screen bottom (see there). */
const int THREAD_PAD_TOP = 1;

/* On-the-Mesh live-refresh throttle. While that column is the visible tab it's
 * fed by the announce firehose (several writes a second on a busy mesh). Even
 * coalesced, rebuilding the sorted catalogue that often burns the lcd task and
 * fights the reader, so: rebuild at most once per MESH_MIN_REBUILD_MS, and hold
 * off entirely until the list has been still (no user scroll) for
 * MESH_SCROLL_QUIET_MS — never re-render under a moving finger. */
const uint32_t MESH_MIN_REBUILD_MS  = 2000;
const uint32_t MESH_SCROLL_QUIET_MS = 5000;

/* Keep only what the UI font can actually draw: strip C0/C1 control bytes and any
 * codepoint with no glyph (emoji, CJK, …). Valid UTF-8 multibyte sequences whose
 * glyph the font carries (accented Latin, if present) pass through unchanged.
 * `oneLine` folds CR/LF/TAB to a single space (previews); else newlines stay. */
std::string printable(std::string_view in, bool oneLine) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)          { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0){ cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0){ cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0){ cp = b & 0x07; len = 4; }
        else { i++; continue; }                       /* stray continuation / invalid lead */
        if (i + len > n) break;                        /* truncated tail */
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;

        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) continue;   /* C0 / DEL / C1 */
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) continue;
        out.append(in.data() + at, len);
    }
    return out;
}

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, kFont, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

/* ---- model (lcd-task-only, no locks) ---- */

struct Msg {
    std::string peer, key, content;
    std::string message_id;   /* SHA-256 hex64; join key into lxmf.msgmeta */
    std::string iface;        /* beautified interface from msgmeta ("" = none recorded) */
    uint8_t status = 0;    /* LxmfStatus code */
    uint8_t tries  = 0;    /* try count; 255 = gave up (terminal) */
    long ts = 0;           /* sender's clock (display) */
    long recv_ts = 0;      /* monotonic receive time (date-separator anchor) */
    bool in = false;
};

struct Ann { std::string hash, name; long last = 0; };   /* a heard announce */

int               g_id = -1;            /* active identity index, -1 = none/unpicked */
std::string       g_msgsPrefix;         /* "s.lxmf.id.N.msgs" */
std::vector<Msg>  g_msgs;               /* non-draft messages for the open conversation */

/* One entry per rendered bubble, in render order, so a live change reconciles
 * against the existing widgets instead of clearing + rebuilding the whole
 * thread: a stage transition updates one glyph, a new message appends one
 * bubble, and only a structural change (delete/reorder) falls back to a full
 * rebuild. `meta` is the bubble's meta row (timestamp + delivery glyph). */
struct BubbleRef { std::string mid; uint8_t status; uint8_t tries; lv_obj_t* meta; };
std::vector<BubbleRef> g_bubbles;
std::vector<Ann>  g_anns;               /* heard announces (on-the-mesh column) */
std::unordered_map<std::string, size_t> g_annIx;   /* hash -> index in g_anns (O(1) name/last lookup) */
/* ---- list rows: reconcile model (tag + patch instead of clear + rebuild) ----
 * Each visible row of a tab list is tracked by a stable KEY (the peer hash for a
 * conversation / mesh row; a "\x01…" sentinel for the New/footer/empty decoration
 * rows) plus a SIGNATURE of everything it renders EXCEPT the age badge. A refresh
 * builds the target row set as specs, then reconciles against the live widgets:
 * an unchanged key+sig is reused untouched, a changed sig recreates that one row,
 * a new key builds one row, a vanished key deletes one, and a final move pass
 * reorders in place. Age is deliberately out of the sig, so time passing alone
 * never churns a row — a badge re-ages only when its row is next legitimately
 * rebuilt (ages were always event-driven, never a ticking clock). This mirrors
 * the thread's bubble reconcile (g_bubbles). */
enum RowKind { RK_CONTACT, RK_PEER, RK_GROUP, RK_EMPTY };
struct RowSpec {
    RowKind     kind = RK_EMPTY;
    std::string key, sig;               /* reconcile identity + change detector */
    std::string peer, title, sub, age;
    lv_color_t  titleColor{};
    int         unread = 0;
};
struct ListRow { std::string key, sig; lv_obj_t* obj = nullptr; };   /* one live row */
std::vector<ListRow> g_rowsC;           /* Contacts rows in render order (search box excluded) */
std::vector<ListRow> g_rowsM;           /* On-the-Mesh rows in render order */
bool g_annsLoaded = false;              /* g_anns walked at least once (gate the O(N) reload) */

/* A large populate (fresh open / identity switch / cleared search) is spread over
 * several LCD-loop turns: build LIST_CHUNK rows, let the loop render + drain input,
 * repeat. Off-screen rows draw for free, so this only ever adds responsiveness. */
const int LIST_CHUNK = 8;
struct ChunkJob {
    lv_obj_t*             list;
    std::vector<ListRow>* model;
    std::vector<RowSpec>  target;       /* 1:1 with *model; pending slots have obj == null */
    bool                  keepScroll;
    int32_t               scrollY;
    lv_timer_t*           timer;
    ChunkJob**            slot;         /* &g_chunkC / &g_chunkM — nulled on finish */
};
ChunkJob* g_chunkC = nullptr;
ChunkJob* g_chunkM = nullptr;
std::vector<std::string> g_nomadTargets;/* "<hash>:<path>" per tappable Nomad link in the open thread */
std::string       g_curPeer;            /* peer of the open thread */
std::string       g_pendingOpenPeer;    /* contact tapped in nomad, awaiting an identity pick */
std::string       g_qContacts;          /* Contacts-tab search-box text */
std::string       g_qMesh;              /* On-the-Mesh-tab search-box text */
int               g_activeTab = 0;      /* 0 = Contacts, 1 = On the Mesh (default Contacts) */
bool              g_subscribed = false;
bool              g_refreshPending = false;   /* a coalesced view rebuild is queued */
bool              g_refreshMsgs    = false;   /* pending changes dirtied the msgs walk */
bool              g_refreshAnns    = false;   /* pending changes dirtied the announce walk */
bool              g_listDirty      = false;   /* list rows need a rebuild: a contacts/msg change always
                                                 sets it; an announce-only change sets it only while the
                                                 mesh column is the visible tab (else it's deferred to the
                                                 tab switch — see onStorageChange/setActiveTab). Survives
                                                 thread-view refreshes, so returning from a conversation
                                                 with no change never rebuilds */
int               g_listBuiltId    = -2;      /* identity the visible list was last built for (-2 = never).
                                                 A fresh open / identity switch differs → forces one build;
                                                 a plain return from a conversation matches → no rebuild */
lv_timer_t*       g_searchTimer = nullptr;        /* one-shot search debounce (null = idle) */
uint32_t          g_lastMeshBuild  = 0;   /* millis() of the last rebuild while the mesh column was visible */
uint32_t          g_lastMeshScroll = 0;   /* millis() of the last USER scroll on the mesh list (throttle gate) */

lv_obj_t* s_layer    = nullptr;
lv_obj_t* s_idpick   = nullptr;         /* identity picker (first screen, >1 ident) */
lv_obj_t* s_contacts = nullptr;         /* list screen: tab bar + two tab pages */
lv_obj_t* s_tabBtnC  = nullptr;         /* "Contacts" tab button */
lv_obj_t* s_tabBtnM  = nullptr;         /* "On the Mesh" tab button */
lv_obj_t* s_tabContacts = nullptr;      /* Contacts page (search + list); shown when tab 0 */
lv_obj_t* s_tabMesh  = nullptr;         /* On-the-Mesh page (search + list); shown when tab 1 */
lv_obj_t* s_searchC  = nullptr;         /* search textarea atop the Contacts list */
lv_obj_t* s_searchM  = nullptr;         /* search textarea atop the On-the-Mesh list */
lv_obj_t* s_listC    = nullptr;         /* Contacts row container (cleaned+rebuilt) */
lv_obj_t* s_listM    = nullptr;         /* On-the-Mesh row container (cleaned+rebuilt) */
lv_obj_t* s_thread   = nullptr;         /* conversation screen (built once, reused) */
lv_obj_t* s_msgList  = nullptr;         /* scroll container inside s_thread */
lv_obj_t* s_bubbles  = nullptr;         /* bubble column (cleaned+rebuilt) inside s_msgList */
lv_obj_t* s_earlierBtn = nullptr;       /* "load earlier messages" (top of the scroll area) */
lv_obj_t* s_newerBtn = nullptr;         /* "▼ N newer messages" (bottom of the scroll area) */
lv_obj_t* s_newerLbl = nullptr;         /* label inside s_newerBtn — count is live-updated */
lv_obj_t* s_loading  = nullptr;         /* "<loading conversation>" placeholder (floating, centered) */
/* History overlay — a second scroll page stacked over the resident one (which
 * stays built, always at the newest messages). "Load earlier" opens it; it pages
 * back independently and holds no composer. Closing just hides it, so returning to
 * the newest is instant with no re-render. */
lv_obj_t* s_histWrap    = nullptr;      /* grow-sibling of s_msgList; shown ⇒ resident hidden */
lv_obj_t* s_histList    = nullptr;      /* its scroll container */
lv_obj_t* s_histBubbles = nullptr;      /* its bubble column (clean+rebuilt each page) */
lv_obj_t* s_histEarlier = nullptr;      /* overlay "load earlier" */
lv_obj_t* s_histNewer   = nullptr;      /* overlay "load newer" (toward the resident boundary) */
lv_obj_t* s_histNewerLbl = nullptr;
std::vector<std::pair<lv_obj_t*, long>> g_histDateSeps;
size_t    h_winLo = 0, h_winHi = 0;     /* overlay window into the curPeer slice */
/* Rendered window over the open conversation's message list (indices into the
 * curPeer slice of g_msgs). The resident page is pinned to newest (g_atNewest
 * stays true); older messages are browsed in the history overlay. */
size_t    g_winLo = 0, g_winHi = 0;
bool      g_atNewest = true;
lv_timer_t* g_threadLoadTimer = nullptr;   /* defers the heavy load/render so the shell paints first */
bool      g_needMsgLoad = false;           /* the pending render must refreshMsgs() first (a fresh open) */
int       g_scrollAfter = 0;               /* 0 = to bottom, 1 = keep g_anchorMid in view */
std::string g_anchorMid;                   /* bubble to hold steady across a paging rebuild */
/* Inline day separators (recv_ts-anchored) + the floating sticky date that shows
 * the current day while scrolling and fades after a pause. */
std::vector<std::pair<lv_obj_t*, long>> g_dateSeps;   /* separator widget + its recv_ts */
lv_obj_t* s_stickyDate = nullptr;          /* floating date pill below the header */
lv_obj_t* s_stickyLbl  = nullptr;
lv_timer_t* g_stickyFadeTimer = nullptr;   /* fades the sticky 2 s after the last scroll */
lv_obj_t* s_threadName = nullptr;       /* header peer-name label */
lv_obj_t* s_threadDown = nullptr;       /* header scroll-to-bottom chevron (hidden at bottom) */
lv_obj_t* s_threadLink = nullptr;       /* header link indicator/toggle image (chain=up, broken-chain=down) */
lv_obj_t* s_info     = nullptr;         /* contact info page (covers list or thread; rebuilt per open) */
lv_obj_t* s_msgDetail = nullptr;        /* per-message detail page (covers the thread; rebuilt per open) */
lv_obj_t* s_confirm  = nullptr;         /* delete-conversation confirm overlay (child of s_info) */
std::string g_infoPeer;                 /* peer shown on the contact info page */
bool      g_infoFromThread = false;     /* where its back chevron returns to */
lv_obj_t* s_compose  = nullptr;         /* compose entry (lcdInputBox) */
lv_obj_t* s_comp     = nullptr;         /* the compose row — last item in the scroll stream */
lv_obj_t* s_rc       = nullptr;         /* controls stacked at the entry's right (pill + Send) */
lv_obj_t* s_send     = nullptr;         /* Send button — shown only once the composer is expanded */
lv_obj_t* s_sendIcon = nullptr;         /* the Send button's paper-plane image (white on blue) */
lv_obj_t* s_composePill  = nullptr;     /* expand pill (↑), left of the entry — shown collapsed */
lv_obj_t* s_collapsePill = nullptr;     /* collapse pill (↓), right by Send — shown expanded */
bool      g_composeExpanded  = false;   /* fixed 8-line composer (vs the 1–4 line quick field) */
lv_obj_t* s_newIdTa  = nullptr;         /* "Add identity" name field (settings pane) */
lv_obj_t* s_importTa = nullptr;         /* "Import identity" hex field (settings pane) */

void refreshMsgs();
void refreshAnnounces();
void rebuildList(bool keepScroll = false);
void rebuildThread();
void showMsgDetail(const std::string& mkey);
void bindMsgDetail(lv_obj_t* o, const std::string& key);
void threadSnapNewest();
void applyComposeMode();
void scrollThreadBottom(bool anim);
void openHistory();
void closeHistory();
void renderHistory();
void updateHistButtons(size_t total);
void updatePageButtons(size_t total);
void showLoading(bool on);
void beginThreadLoad();
void updateStickyDate();
void showContacts();
void setActiveTab(int n, bool keepScroll = false);
void openThread(const std::string& peer);
void scheduleRefresh();
void scheduleRefreshIn(uint32_t ms);
void maybeOpenPending();
void onLcdOpenUrl(const char* key, const char* val);
void showInfo(const std::string& peer);
void closeInfo();
void updateThreadDownVis();
void updateThreadLink();

/* ---- deferred focus ----
 * The launcher tile (or a tapped row) is focused into the input group on
 * click-release — AFTER our open handlers run — so an immediate focus is stolen
 * back. Fire it from a one-shot timer once that settles (mirrors cliFocus). */
lv_obj_t* g_focusTarget = nullptr;
void focusTimerCb(lv_timer_t*) {
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    g_focusTarget = o;
    lv_timer_t* ft = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(ft, 1);
}

/* Deferred focus that pins a scroll position. The search box is child 0 of the
 * scrollable list, so focusing it drags the list to the top — unwanted when
 * returning from a conversation (the list should stay exactly where it was).
 * Capture the scroll now and restore it right after the deferred focus lands. */
lv_obj_t* g_pinList = nullptr;
int32_t   g_pinScrollY = 0;
void pinFocusCb(lv_timer_t*) {
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
    if (g_pinList && lv_obj_is_valid(g_pinList))
        lv_obj_scroll_to_y(g_pinList, g_pinScrollY, LV_ANIM_OFF);
}
void deferFocusPinScroll(lv_obj_t* o, lv_obj_t* list) {
    if (!o) return;
    g_focusTarget = o;
    g_pinList     = list;
    g_pinScrollY  = list ? lv_obj_get_scroll_y(list) : 0;
    lv_timer_t* ft = lv_timer_create(pinFocusCb, 40, nullptr);
    lv_timer_set_repeat_count(ft, 1);
}

/* ---- small text helpers ---- */

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}
bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
/* needle is already lowercased + trimmed; hash is lowercase hex. */
bool qmatch(const std::string& needle, const std::string& name, const std::string& hash) {
    if (needle.empty()) return true;
    return lower(name).find(needle) != std::string::npos ||
           hash.find(needle) != std::string::npos;
}

/* ---- identity ---- */

/* Loaded = the firmware has published this slot's delivery address. That happens
 * from local crypto at boot, before the mailbox connects — so a loaded
 * identity's stored history is viewable straight away, and only sending waits
 * for `up`. A config-only slot (no private key) never gets a dest_hash, so it's
 * excluded — which is exactly right, it can never come up. */
void loadedIds(std::vector<int>& out) {
    out.clear();
    for (int n = 0; n < 4; n++) {
        char k[40];
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        if (!storageGetStr(k, "").empty()) out.push_back(n);
    }
}

/* Fully up = the firmware has connected this slot's delivery dest, so it can
 * send/receive. Sending is gated on this even once history is already shown. */
bool idUp(int n) {
    if (n < 0) return false;
    char k[40];
    snprintf(k, sizeof k, "lxmf.id.%d.up", n);
    return storageGetInt(k, 0) == 1;
}

/* Persistent slots that exist in config (the label survives reboot), split by
 * whether they're enabled. Available from storage the instant we open, long
 * before the firmware brings a mailbox up. A configured-but-not-yet-up slot is
 * the normal post-reset state while rnsd + the mailbox come up — distinct from
 * "no identity at all", so the empty-list guidance can say the right thing
 * instead of falsely telling the user to create one. */
void configuredIds(std::vector<int>& enabled, std::vector<int>& disabled) {
    enabled.clear(); disabled.clear();
    for (int n = 0; n < 4; n++) {
        char k[48];
        snprintf(k, sizeof k, "s.lxmf.id.%d.label", n);
        if (!storageExists(k)) continue;
        snprintf(k, sizeof k, "s.lxmf.id.%d.enabled", n);
        (storageGetInt(k, 1) != 0 ? enabled : disabled).push_back(n);
    }
}

std::string idLabel(int n) {
    char k[48];
    snprintf(k, sizeof k, "s.lxmf.id.%d.display_name", n);
    std::string s = storageGetStr(k, "");
    if (s.empty()) { snprintf(k, sizeof k, "s.lxmf.id.%d.label", n); s = storageGetStr(k, ""); }
    if (s.empty()) s = "identity " + std::to_string(n);
    return printable(s, true);
}

void selectId(int n) {
    g_id = n;
    g_msgsPrefix = (n >= 0) ? ("s.lxmf.id." + std::to_string(n) + ".msgs") : "";
    refreshMsgs();
}

std::string peerName(const std::string& peer) {
    char k[96];
    snprintf(k, sizeof k, "s.lxmf.id.%d.contacts.%s.display_name", g_id, peer.c_str());
    std::string n = storageGetStr(k, "");
    if (!n.empty()) return printable(n, true);
    /* No stored contact name — fall back to the name we last heard this dest
     * announce (already parsed into g_anns for the on-the-mesh column). Covers
     * contacts stubbed hash-only before an announce arrived, and any left over
     * from before the send path learned to seed the name. */
    auto it = g_annIx.find(peer);
    if (it != g_annIx.end() && !g_anns[it->second].name.empty())
        return g_anns[it->second].name;
    return peer.substr(0, 8) + "...";
}

/* ---- message store: rebuilt from storage via a leaf walk ---- */

void msgCb(const char* key, const char* val) {
    /* key = "s.lxmf.id.N.msgs.<peer>.<mkey>.<field>" */
    size_t plen = g_msgsPrefix.size();
    if (strncmp(key, g_msgsPrefix.c_str(), plen) != 0) return;
    const char* rest = key + plen;
    if (*rest == '.') rest++;
    const char* d1 = strchr(rest, '.'); if (!d1) return;
    std::string peer(rest, d1 - rest);
    const char* mk = d1 + 1;
    const char* d2 = strchr(mk, '.'); if (!d2) return;
    std::string mkey(mk, d2 - mk);
    const char* field = d2 + 1;
    if (strchr(field, '.')) return;     /* deeper than a leaf field */

    Msg* m = nullptr;
    for (auto& x : g_msgs) if (x.peer == peer && x.key == mkey) { m = &x; break; }
    if (!m) { g_msgs.emplace_back(); m = &g_msgs.back(); m->peer = peer; m->key = mkey; }

    if      (!strcmp(field, "dir"))     m->in     = (val && !strcmp(val, "in"));
    else if (!strcmp(field, "content")) m->content = val ? val : "";
    else if (!strcmp(field, "ts"))      m->ts     = val ? atol(val) : 0;
    else if (!strcmp(field, "recv_ts")) m->recv_ts = val ? atol(val) : 0;
    else if (!strcmp(field, "status"))  m->status = val ? (uint8_t)atoi(val) : 0;
    else if (!strcmp(field, "tries"))   m->tries  = val ? (uint8_t)atoi(val) : 0;
    else if (!strcmp(field, "message_id")) m->message_id = val ? val : "";
}

void refreshMsgs() {
    /* Load ONLY the open conversation's messages. Message bodies live in the
       per-conversation record store now; walking the whole "s.lxmf.id.N.msgs"
       prefix would load + decompress every conversation under CFG_LOCK on the
       LCD task — stalling the storage actor (the O(all-messages) refresh the
       structured-DB rework removes). The conversation LIST reads the maintained
       directory (contacts.*) instead; see rebuildList. */
    g_msgs.clear();
    if (g_id < 0 || g_curPeer.empty()) return;
    std::string prefix = g_msgsPrefix + "." + g_curPeer;
    storageForEach(prefix.c_str(), msgCb);
    g_msgs.erase(std::remove_if(g_msgs.begin(), g_msgs.end(),
                                [](const Msg& m) { return m.status == LXMF_ST_DRAFT; }),
                 g_msgs.end());
    /* Join routing telemetry from the RAM-only msgmeta store (keyed by
     * message_id) — just the interface string, for the LoRa bubble pill; the
     * detail screen reads the rest on open. */
    for (auto& m : g_msgs)
        m.iface = m.message_id.empty() ? std::string()
                : storageGetStr(("lxmf.msgmeta." + m.message_id + ".iface").c_str(), "");
}

/* ---- announce catalogue: per-field records "lxmf.announces.<hex>.<field>" ---- */

void annCb(const char* key, const char* val) {
    const char* tail = key + (sizeof("lxmf.announces.") - 1);
    const char* dot = strchr(tail, '.');
    if (!dot) return;                    /* need <hex>.<field> */
    std::string hash(tail, dot - tail);
    const char* field = dot + 1;
    if (strchr(field, '.')) return;      /* leaf field only */
    /* Fields of one hash arrive contiguously (per-record store) → only the last
     * accumulator can match; O(1) per leaf instead of O(N). */
    Ann* a = (!g_anns.empty() && g_anns.back().hash == hash) ? &g_anns.back() : nullptr;
    if (!a) { g_anns.push_back(Ann{}); a = &g_anns.back(); a->hash = hash; }
    if      (!strcmp(field, "last")) a->last = val ? atol(val) : 0;
    else if (!strcmp(field, "name")) a->name = val ? printable(val, true) : std::string();
}

void refreshAnnounces() {
    g_anns.clear();
    storageForEach("lxmf.announces.", annCb);
    std::sort(g_anns.begin(), g_anns.end(),
              [](const Ann& a, const Ann& b) { return a.last > b.last; });
    g_annIx.clear();
    g_annIx.reserve(g_anns.size());
    for (size_t i = 0; i < g_anns.size(); i++) g_annIx.emplace(g_anns[i].hash, i);
}

/* ---- compose / send ---- */

void sendMessage(const std::string& peer, const std::string& text) {
    if (g_id < 0 || !idUp(g_id) || peer.empty() || text.empty()) return;
    static unsigned seq = 0;
    char key[40];
    snprintf(key, sizeof key, "o_%u_%u", (unsigned)millis(), ++seq);
    char base[120];
    snprintf(base, sizeof base, "s.lxmf.id.%d.msgs.%s.%s", g_id, peer.c_str(), key);
    char k[200];
    long ts = (long)time(nullptr);
    auto setf = [&](const char* f, const char* v) {
        snprintf(k, sizeof k, "%s.%s", base, f);
        storageSet(k, v);
    };
    /* One atomic write: all fields plus the send sentinel land together, so the
       lxmf task never picks up a half-written message and the s.lxmf.id
       subscription fires once for the whole message instead of once per field. */
    storageBegin();
    setf("dir", "out");
    setf("peer", peer.c_str());
    setf("title", "");
    setf("content", text.c_str());
    setf("thread", "");
    /* Optimistic status: QUEUED (not draft) so the bubble appears the instant we
       send (renders as the "..." in-flight chip). The lxmf task drives it on. */
    snprintf(k, sizeof k, "%s.status", base);
    storageSet(k, (int)LXMF_ST_QUEUED);
    snprintf(k, sizeof k, "%s.ts", base);
    storageSet(k, (int)ts);
    /* Send sentinel (ephemeral): "<peer>/<key>". The lxmf task drives the stage. */
    char sentinel[48], payload[80];
    snprintf(sentinel, sizeof sentinel, "lxmf.id.%d.cmd.send", g_id);
    snprintf(payload, sizeof payload, "%s/%s", peer.c_str(), key);
    storageSet(sentinel, payload);
    storageEnd();

    /* Optimistic render: append the outgoing message to the in-RAM store now,
       on the lcd task, so onSend can draw the bubble immediately. Without this
       the bubble only appears once the storage write has round-tripped through
       the storage task and back via the change subscription — seconds under
       load. The eventual refreshMsgs() rebuilds g_msgs from storage and dedups
       on peer+key, so this entry is replaced in place rather than doubled. */
    Msg m;
    m.peer = peer; m.key = key; m.content = text; m.status = LXMF_ST_QUEUED;
    m.ts = ts; m.in = false;
    g_msgs.push_back(std::move(m));
}

/* Per-conversation compose drafts. `s_compose` is one reused textarea, so
 * switching threads would otherwise bleed the old draft into the new peer. Hold
 * the in-progress text in EPHEMERAL storage (lxmf.draft.<id>.<peer> — in-RAM,
 * mirrored, never flashed) so it reappears when you come back to that
 * conversation; the browser sees the same key for free. */
std::string draftKey(int n, const std::string& peer) {
    char k[96];
    snprintf(k, sizeof k, "lxmf.draft.%d.%s", n, peer.c_str());
    return k;
}
void saveDraft() {
    if (g_id < 0 || g_curPeer.empty() || !s_compose) return;
    const char* t = lv_textarea_get_text(s_compose);
    if (t && *t) storageSet(draftKey(g_id, g_curPeer).c_str(), t);
    else         storageUnset(draftKey(g_id, g_curPeer).c_str());
}
void loadDraft(const std::string& peer) {
    if (!s_compose) return;
    std::string d = (g_id >= 0) ? storageGetStr(draftKey(g_id, peer).c_str(), "") : "";
    lv_textarea_set_text(s_compose, d.c_str());
}

void onSend(lv_event_t*) {
    if (!s_compose || g_curPeer.empty()) return;
    if (g_id < 0 || !idUp(g_id)) return;   /* mailbox not up yet — sending is held */
    const char* t = lcdInputBoxText(s_compose);   /* trailing whitespace stripped */
    if (t && *t) {
        sendMessage(g_curPeer, t);
        lv_textarea_set_text(s_compose, "");
        storageUnset(draftKey(g_id, g_curPeer).c_str());   /* draft consumed by the send */
        rebuildThread();     /* draw the optimistic bubble now, not on the storage round-trip */
        /* Paint it NOW. The Send button happens to fire in the lcd loop's input
         * step, right before lv_timer_handler renders — the bubble shows at once.
         * Keyboard Enter fires inside the loop's aux-drain instead, and before
         * that drain finishes it is fed our own send's storage-change echoes
         * (each a full refreshMsgs + rebuildThread — seconds under load), so the
         * bubble only appeared after the storm. One inline refresh makes both
         * paths visually identical: bubble first, then the echoes. */
        lv_refr_now(nullptr);
        deferFocus(s_compose);   /* keep the cursor in the entry box for the next message */
        g_composeExpanded = false;               /* a send reverts to the quick field */
        applyComposeMode();
        scrollThreadBottom(false);               /* the shrink keeps the composer bottom at the screen bottom */
    }
}

/* "Add identity" commit — reads the name field and writes the ephemeral create
 * sentinel (the lxmf task picks a free slot and generates the identity). Fired
 * by the explicit Create button or by Enter, so it works without the keystroke
 * being obvious. No-op on an empty name. */
void onAddIdentity(lv_event_t*) {
    if (!s_newIdTa) return;
    const char* t = lv_textarea_get_text(s_newIdTa);
    if (t && *t) {
        storageSet("lxmf.cmd.identity_new", t);
        lv_textarea_set_text(s_newIdTa, "");
    }
}

/* Import commit — writes the ephemeral import sentinel with a 128-hex private
 * key (the lxmf task validates + picks a free slot). No-op on an empty field. */
void onImportIdentity(lv_event_t*) {
    if (!s_importTa) return;
    const char* t = lv_textarea_get_text(s_importTa);
    if (t && *t) {
        storageSet("lxmf.cmd.identity_import", t);
        lv_textarea_set_text(s_importTa, "");
    }
}

/* ---- per-identity Destroy (two-tap arm; wipes key + all of its storage) ----
 * Destroy is irreversible, so the first tap arms ("Confirm?") and a 3 s timer
 * disarms it; the second tap within that window writes the destroy sentinel. */
struct DestroyState { int slot; bool armed; lv_obj_t* lbl; lv_timer_t* t; };

void destroyDisarm(lv_timer_t* tm) {
    auto* d = static_cast<DestroyState*>(lv_timer_get_user_data(tm));
    d->armed = false; d->t = nullptr;
    if (d->lbl && lv_obj_is_valid(d->lbl)) lv_label_set_text(d->lbl, "Destroy");
}

void onDestroyClick(lv_event_t* e) {
    auto* d = static_cast<DestroyState*>(lv_event_get_user_data(e));
    if (!d->armed) {
        d->armed = true;
        lv_label_set_text(d->lbl, "Confirm?");
        d->t = lv_timer_create(destroyDisarm, 3000, d);
        lv_timer_set_repeat_count(d->t, 1);
    } else {
        if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
        char v[8];
        snprintf(v, sizeof v, "%d", d->slot);
        storageSet("lxmf.cmd.identity_destroy", v);
        d->armed = false;
        lv_label_set_text(d->lbl, "Destroy");
    }
}

void onDestroyDelete(lv_event_t* e) {
    auto* d = static_cast<DestroyState*>(lv_event_get_user_data(e));
    if (d->t) { lv_timer_delete(d->t); d->t = nullptr; }
    free(d);
}

/* One identity's admin block in the settings pane: name + up/down status, dest
 * (live), enabled toggle, and a two-tap Destroy. Mirrors the web LxmfPanel row.
 * destKey/enKey must be string literals — lcdSettingValue/Switch keep the key by
 * pointer — so the caller passes one literal pair per slot. */
void lxmfIdentityBlock(lv_obj_t* p, int n, const char* destKey, const char* enKey) {
    char k[48];

    lv_obj_t* hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(hdr, 6, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(k, sizeof k, "s.lxmf.id.%d.display_name", n);
    std::string nm = storageGetStr(k, "");
    if (nm.empty()) { snprintf(k, sizeof k, "s.lxmf.id.%d.label", n); nm = storageGetStr(k, ""); }
    if (nm.empty()) nm = "identity " + std::to_string(n);
    mkLabel(hdr, printable(nm, true) + "  (slot " + std::to_string(n) + ")", lv_color_white());

    snprintf(k, sizeof k, "lxmf.id.%d.up", n);
    bool up = storageGetInt(k, 0) == 1;
    mkLabel(hdr, up ? "up" : "down", up ? lv_color_hex(0x6fb98f) : lv_color_hex(0xa06868));

    lcdSettingValue (p, "dest",    destKey);
    lcdSettingSwitch(p, "enabled", enKey);

    lv_obj_t* b = lv_button_create(p);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5a2a2a), 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, "Destroy");
    lv_obj_center(l);
    auto* d = static_cast<DestroyState*>(gp_alloc(sizeof(DestroyState)));
    d->slot = n; d->armed = false; d->lbl = l; d->t = nullptr;
    lv_obj_add_event_cb(b, onDestroyClick,  LV_EVENT_CLICKED, d);
    lv_obj_add_event_cb(b, onDestroyDelete, LV_EVENT_DELETE,  d);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
}

void markRead(const std::string& peer) {
    /* Advance the per-conversation read watermark to the newest message and clear
       the maintained unread counter. Reads the newest ts from the directory
       (contacts.last_ts) rather than the message store, so it works before the
       conversation body is loaded. One or two writes, never O(messages). */
    char base[160];
    snprintf(base, sizeof base, "s.lxmf.id.%d.contacts.%s", g_id, peer.c_str());
    long newest = storageGetInt((std::string(base) + ".last_ts").c_str(), 0);
    storageBegin();
    if (newest > 0 && storageGetInt((std::string(base) + ".read_ts").c_str(), 0) < (int)newest)
        storageSet((std::string(base) + ".read_ts").c_str(), (int)newest);
    if (storageGetInt((std::string(base) + ".unread").c_str(), 0) != 0)
        storageSet((std::string(base) + ".unread").c_str(), 0);
    storageEnd();
}

/* ---- scrolling ---- */

/* The header down-chevron is a "there's more below" cue: shown only while the
 * view isn't already at the bottom. Re-checked on every scroll event (including
 * each animation frame, so an animated jump hides it as it lands). */
void updateThreadDownVis() {
    if (!s_threadDown || !s_msgList) return;
    if (lv_obj_get_scroll_bottom(s_msgList) <= 2)
        lv_obj_add_flag(s_threadDown, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(s_threadDown, LV_OBJ_FLAG_HIDDEN);
}

/* Conversation-link state to `peer`, as published ephemerally by the lxmf
 * task at lxmf.id.<n>.link.<peer>: "" (down), "establishing", or "active". */
std::string linkStateOf(const std::string& peer) {
    if (g_id < 0 || peer.empty()) return "";
    char k[80];
    snprintf(k, sizeof k, "lxmf.id.%d.link.%s", g_id, peer.c_str());
    return storageGetStr(k, "");
}

/* ---- runtime SVG icons (send / link / link-off, shipped to /fixed/icons) ---- */

/* Pixel sizes, scaled with the UI zoom. The link icon sits in the HDR_H header;
 * the send plane fills its 2-line button. */
int linkIconPx() { return lcdPx(18); }
int sendIconPx() { return lcdPx(22); }

/* Point an lv_image at a runtime-rasterized icon, tinted to `col` (the rasters
 * are monochrome, so image-recolor at COVER paints the glyph shape any colour).
 * The raster loads off the lcd task: if it isn't cached yet, request it and
 * leave the slot as-is — the icon-settle timer / thread refresh re-applies once
 * it lands. */
void applyIcon(lv_obj_t* img, const char* base, int px, lv_color_t col) {
    if (!img) return;
    lv_obj_set_style_image_recolor(img, col, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    const lv_image_dsc_t* dsc = lcdIconDsc(base, px);
    if (dsc) lv_image_set_src(img, dsc);
    else     lcdIconRequest(base, px);
}

/* Set the header link glyph for the open thread's peer: the chain when a link is
 * up (green active / amber establishing), the broken chain when it's down (grey).
 * Driven from openThread and every thread refresh, so it follows a link torn
 * down for any reason (the state key is re-derived by the lxmf task each second). */
void updateThreadLink() {
    if (!s_threadLink) return;
    std::string st = linkStateOf(g_curPeer);
    bool up = !st.empty();
    lv_color_t col = st == "active"       ? lv_color_hex(0x4abf6a)   /* green: open */
                   : st == "establishing" ? lv_color_hex(0xd6a12a)   /* amber: opening */
                                          : lv_color_hex(0x6a7280);  /* grey: down */
    applyIcon(s_threadLink, up ? "link" : "link-off", linkIconPx(), col);
}

/* Re-apply the thread's icons (send + header link), requesting any not yet
 * rasterized. Returns true once every icon this screen needs is cached, so the
 * settle timer can stop. Both link variants are warmed so a toggle is instant. */
bool applyThreadIcons() {
    if (s_sendIcon) applyIcon(s_sendIcon, "send", sendIconPx(), lv_color_white());
    updateThreadLink();
    lcdIconRequest("link", linkIconPx());
    lcdIconRequest("link-off", linkIconPx());
    return lcdIconReady("send", sendIconPx())
        && lcdIconReady("link", linkIconPx())
        && lcdIconReady("link-off", linkIconPx());
}

/* Icons rasterize off-task, so they may not be ready the instant a thread opens.
 * Tick a short settle after open, re-applying until all land (or a safety cap),
 * then stop — no dependency on the async loader callback (launcher-owned). */
lv_timer_t* g_iconSettle = nullptr;
int         g_iconSettleTicks = 0;
void iconSettleCb(lv_timer_t* t) {
    if (applyThreadIcons() || ++g_iconSettleTicks >= 40) {   /* ~3.2 s cap */
        lv_timer_delete(t);
        g_iconSettle = nullptr;
    }
}
void startIconSettle() {
    if (g_iconSettle) lv_timer_delete(g_iconSettle);
    g_iconSettleTicks = 0;
    g_iconSettle = lv_timer_create(iconSettleCb, 80, nullptr);
}

/* The board flips the ephemeral sys.standby key around lcdScreenSleep/Wake. */
bool lcdAwake()      { return storageGetInt("sys.standby", 0) == 0; }
bool threadAtBottom(){ return s_msgList && lv_obj_get_scroll_bottom(s_msgList) <= 4; }
/* "Actively reading": the thread is open and awake with the newest messages in
 * view — the condition under which an arriving message stays marked read. */
bool threadReading() {
    return s_thread && !lv_obj_has_flag(s_thread, LV_OBJ_FLAG_HIDDEN)
        && !g_curPeer.empty() && lcdAwake() && threadAtBottom();
}

void scrollThreadBottom(bool anim) {
    if (!s_msgList) return;
    /* Keep the compose entry pinned to the screen bottom. When the content is
     * shorter than the viewport — a new/short thread, or a bubble that just shrank
     * (a delivered receipt hugging its timestamp onto the last line) — scrolling
     * alone can't pull the entry down: there's no scroll range, so it floats in
     * mid-air. Reset any prior spacer, measure the gap between the entry's bottom
     * and the viewport's, and absorb it with a top spacer so the two always meet.
     * Only the newest page carries the composer; on a history page (entry hidden)
     * we leave the base pad and just scroll. */
    lv_obj_set_style_pad_top(s_msgList, THREAD_PAD_TOP, 0);
    lv_obj_update_layout(s_msgList);
    if (s_comp && !lv_obj_has_flag(s_comp, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t la, ca;
        lv_obj_get_coords(s_msgList, &la);
        lv_obj_get_coords(s_comp, &ca);
        int32_t slack = la.y2 - ca.y2;   /* >0 ⇒ the entry's bottom floats above the viewport's */
        if (slack > 0) {
            lv_obj_set_style_pad_top(s_msgList, THREAD_PAD_TOP + slack, 0);
            lv_obj_update_layout(s_msgList);
        }
    }
    lv_obj_scroll_to_y(s_msgList, LV_COORD_MAX, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    updateThreadDownVis();   /* instant jumps land now; animated ones keep updating per frame */
}

/* Back to the newest, in view. The resident page is always the newest messages, so
 * this is just: hide the history overlay (instant — the resident stayed built) and
 * scroll to the bottom. Typing does this so a keystroke — which the always-focused
 * compose receives even while the overlay covers it — lands the message in view. */
void threadSnapNewest() {
    closeHistory();
    scrollThreadBottom(false);
}

/* ---- navigation ---- */

void showContacts() {
    closeInfo();                            /* a leftover info page would sit on top */
    closeHistory();                         /* don't leave the overlay up for the next open */
    lcdProgramFullscreen(false);            /* status bar back for the list screen */
    if (s_thread) lv_obj_add_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    if (s_idpick) lv_obj_add_flag(s_idpick, LV_OBJ_FLAG_HIDDEN);
    if (s_contacts) {
        /* Just REVEAL the list that stayed built underneath the thread — leaving a
           conversation is an instant visibility flip, no rebuild. Anything that
           changed while the thread was open is folded in by the deferred refresh
           below (cheap now: the list reads the directory, not the message store),
           so the reveal never blocks on a walk. */
        lv_obj_remove_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
        setActiveTab(g_activeTab, /*keepScroll=*/true);   /* returning from a conv keeps the list position */
    }
    saveDraft();                            /* hold the in-progress text for this conversation */
    g_curPeer.clear();
    /* Only rebuild when needed: a fresh open / identity switch (the list was built
     * for a different identity, or never) must render; a change that arrived while
     * away (g_listDirty) must fold in; but a plain return from a conversation is a
     * pure visibility flip — no rebuild, no scroll change. */
    if (g_listBuiltId != g_id) g_listDirty = true;
    scheduleRefresh();
}

/* ---- thread pagination + deferred load ---- */

void showLoading(bool on) {
    if (s_loading) { if (on) lv_obj_remove_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
                     else     lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN); }
    if (on) {   /* the page buttons + compose never show over the placeholder */
        if (s_earlierBtn) lv_obj_add_flag(s_earlierBtn, LV_OBJ_FLAG_HIDDEN);
        if (s_newerBtn)   lv_obj_add_flag(s_newerBtn,   LV_OBJ_FLAG_HIDDEN);
        if (s_comp)       lv_obj_add_flag(s_comp,       LV_OBJ_FLAG_HIDDEN);   /* re-shown by updatePageButtons after render */
    }
}

/* Show/hide + label the earlier/newer affordances for the current window. The
 * compose row lives at the bottom of the scroll stream and shows only on the
 * newest page — a history page (paged back) has no composer. */
void updatePageButtons(size_t total) {
    if (s_comp) { if (g_atNewest) lv_obj_remove_flag(s_comp, LV_OBJ_FLAG_HIDDEN);
                  else            lv_obj_add_flag(s_comp, LV_OBJ_FLAG_HIDDEN); }
    if (s_earlierBtn) { if (g_winLo > 0) lv_obj_remove_flag(s_earlierBtn, LV_OBJ_FLAG_HIDDEN);
                        else              lv_obj_add_flag(s_earlierBtn, LV_OBJ_FLAG_HIDDEN); }
    if (s_newerBtn && s_newerLbl) {
        if (g_winHi < total) {
            char b[40];
            snprintf(b, sizeof b, LV_SYMBOL_DOWN "  %u newer", (unsigned)(total - g_winHi));
            lv_label_set_text(s_newerLbl, b);
            lv_obj_remove_flag(s_newerBtn, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(s_newerBtn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Hold g_anchorMid at the top of the viewport across a paging rebuild (so
 * "load earlier" doesn't jump you). Falls back to the bottom if it's gone. */
void scrollToAnchor() {
    if (!s_msgList) return;
    for (auto& r : g_bubbles) {
        if (r.mid != g_anchorMid || !r.meta) continue;
        lv_obj_update_layout(s_msgList);
        lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(r.meta));   /* meta → bubble → row */
        if (row) { lv_obj_scroll_to_y(s_msgList, lv_obj_get_y(s_bubbles) + lv_obj_get_y(row), LV_ANIM_OFF);
                   updateThreadDownVis(); return; }
    }
    scrollThreadBottom(false);
}

void threadRenderCb(lv_timer_t*) {
    g_threadLoadTimer = nullptr;
    if (g_curPeer.empty()) return;
    if (g_needMsgLoad) {
        refreshMsgs();                 /* the heavy per-conversation record load */
        markRead(g_curPeer);
        g_needMsgLoad = false;
        size_t total = 0; for (auto& m : g_msgs) if (m.peer == g_curPeer) total++;
        g_atNewest = true;
        g_winHi = total;
        g_winLo = total > PAGE_SIZE ? total - PAGE_SIZE : 0;   /* newest page */
    }
    showLoading(false);
    rebuildThread();                   /* renders the window + sets the page buttons */
    if (g_scrollAfter == 1) scrollToAnchor();
    else                    scrollThreadBottom(false);
    g_anchorMid.clear();
}

/* Reveal the placeholder + defer the heavy load/render one loop turn so the shell
 * paints immediately (used by open + load-earlier/newer). */
void beginThreadLoad() {
    if (!s_bubbles) return;
    lv_obj_clean(s_bubbles);
    g_bubbles.clear();
    g_nomadTargets.clear();
    g_dateSeps.clear();
    if (s_stickyDate) lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN);
    /* Drop any composer-pin spacer left by the prior thread (scrollThreadBottom),
     * else it shifts the centred "<loading conversation>" placeholder downward. */
    if (s_msgList) lv_obj_set_style_pad_top(s_msgList, THREAD_PAD_TOP, 0);
    showLoading(true);
    lv_refr_now(nullptr);              /* paint shell + placeholder before the heavy work */
    if (g_threadLoadTimer) lv_timer_delete(g_threadLoadTimer);
    g_threadLoadTimer = lv_timer_create(threadRenderCb, 20, nullptr);
    lv_timer_set_repeat_count(g_threadLoadTimer, 1);
}

size_t threadTotal() {
    size_t total = 0; for (auto& m : g_msgs) if (m.peer == g_curPeer) total++;
    return total;
}

/* The resident page's "load earlier" opens the history overlay (the resident stays
 * pinned to newest). */
void onLoadEarlier(lv_event_t*) { openHistory(); }

void onLoadNewer(lv_event_t*) {
    size_t total = threadTotal();
    if (g_winHi >= total) return;
    g_anchorMid = g_bubbles.empty() ? std::string() : g_bubbles.back().mid;
    g_winHi += PAGE_SIZE;
    if (g_winHi >= total || total - g_winHi <= PAGE_MIN) g_winHi = total;   /* snap: no runt page */
    g_atNewest = (g_winHi >= total);
    if (g_winHi - g_winLo > MAXSPAN) g_winLo = g_winHi - MAXSPAN;
    g_scrollAfter = g_atNewest ? 0 : 1;                        /* newest → bottom; else keep the join */
    g_needMsgLoad = false;
    beginThreadLoad();
}

/* Reflect the compose mode onto the widgets. Two states:
 *   collapsed: 1–4 line quick field, expand pill ↑ on the LEFT, Enter sends.
 *   expanded:  fixed 8-line composer, collapse pill ↓ + Send on the RIGHT,
 *              Enter = newline.
 * Either pill toggles the mode; a send reverts to collapsed. */
void applyComposeMode() {
    if (!s_compose) return;
    lcdInputBoxSetLines(s_compose, g_composeExpanded ? 8 : 1, g_composeExpanded ? 8 : 4);
    lcdInputBoxSetSubmitOnEnter(s_compose, !g_composeExpanded);   /* expanded → Enter = newline */
    /* Left expand pill only when collapsed; right collapse pill + Send only when
     * expanded. */
    auto show = [](lv_obj_t* o, bool on) {
        if (!o) return;
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(s_composePill,  !g_composeExpanded);
    show(s_collapsePill,  g_composeExpanded);
    show(s_send,          g_composeExpanded);
    /* Size the controls column to the entry so the collapse pill rides its top and
     * the Send its bottom; collapsed, the column is empty (both hidden). */
    if (s_rc) {
        if (g_composeExpanded) { lv_obj_update_layout(s_compose);
                                 lv_obj_set_height(s_rc, lv_obj_get_height(s_compose)); }
        else                     lv_obj_set_height(s_rc, LV_SIZE_CONTENT);
    }
}

void buildThreadShell() {
    s_thread = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_thread);
    lv_obj_set_size(s_thread, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_thread, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_thread, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_thread, LV_FLEX_FLOW_COLUMN);

    /* Header: back chevron + peer name + (conditional) scroll-to-bottom
     * chevron. The bar itself is a tap target too — anywhere that isn't one of
     * the clickable chevrons (they win their own hit-test) opens the contact
     * info page, mirroring the web thread header. */
    lv_obj_t* hdr = lv_obj_create(s_thread);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, [](lv_event_t*) {
        if (!g_curPeer.empty()) showInfo(g_curPeer);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { showContacts(); }, LV_EVENT_CLICKED, nullptr);

    s_threadName = mkLabel(hdr, "", lv_color_white());
    lv_obj_align(s_threadName, LV_ALIGN_LEFT_MID, 28, 0);

    /* Link indicator/toggle, pinned rightmost: the chain icon (green=open,
     * amber=establishing) or the broken chain (grey=down), matching the web
     * header. Tapping opens the link when down, closes it when up (writes the
     * cmd.link_open/link_close sentinel the lxmf task consumes). The glyph is a
     * runtime-rasterized SVG (updateThreadLink sets src + recolour). */
    s_threadLink = lv_image_create(hdr);
    lv_obj_set_size(s_threadLink, linkIconPx(), linkIconPx());
    lv_obj_align(s_threadLink, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(s_threadLink, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_threadLink, 12);
    lv_obj_add_event_cb(s_threadLink, [](lv_event_t*) {
        if (g_id < 0 || g_curPeer.empty()) return;
        bool up = !linkStateOf(g_curPeer).empty();
        char sentinel[48];
        snprintf(sentinel, sizeof sentinel, "lxmf.id.%d.cmd.%s",
                 g_id, up ? "link_close" : "link_open");
        storageSet(sentinel, g_curPeer.c_str());
    }, LV_EVENT_CLICKED, nullptr);

    /* Scroll-to-bottom chevron, 20 px clear of the link icon (only shown when the
     * view isn't already at the bottom). */
    s_threadDown = mkLabel(hdr, LV_SYMBOL_DOWN, lv_color_hex(0xc0c8d0));
    lv_obj_align(s_threadDown, LV_ALIGN_RIGHT_MID, -(6 + linkIconPx() + 20), 0);
    lv_obj_add_flag(s_threadDown, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_threadDown, 12);
    lv_obj_add_event_cb(s_threadDown, [](lv_event_t*) { closeHistory(); scrollThreadBottom(true); }, LV_EVENT_CLICKED, nullptr);

    /* Scroll area (grows to fill): the bubble column only. The compose row is a
     * sibling below it (not a child), so it stays pinned to the bottom. */
    s_msgList = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_msgList);
    lv_obj_set_width(s_msgList, lv_pct(100));
    lv_obj_set_flex_grow(s_msgList, 1);
    lv_obj_set_flex_flow(s_msgList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_msgList, 4, 0);
    lv_obj_set_style_pad_ver(s_msgList, 1, 0);
    lv_obj_set_style_pad_row(s_msgList, 1, 0);
    lv_obj_add_event_cb(s_msgList, [](lv_event_t*) { updateThreadDownVis(); updateStickyDate(); },
                        LV_EVENT_SCROLL, nullptr);

    /* Floating sticky date pill, pinned just under the header (a child of the
     * thread, not the scroll list, so it stays put while messages scroll). Shown
     * during scroll when no inline separator is at the top; fades after 2 s. */
    s_stickyDate = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_stickyDate);
    lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_stickyDate, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_stickyDate, LV_ALIGN_TOP_MID, 0, HDR_H + 3);
    lv_obj_set_style_bg_color(s_stickyDate, lv_color_hex(0xffffcc), 0);   /* pale yellow, distinct */
    lv_obj_set_style_bg_opa(s_stickyDate, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_stickyDate, 4, 0);
    lv_obj_set_style_pad_hor(s_stickyDate, 10, 0);
    lv_obj_set_style_pad_ver(s_stickyDate, 2, 0);
    lv_obj_remove_flag(s_stickyDate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN);
    s_stickyLbl = mkLabel(s_stickyDate, "", lv_color_black());
    lv_obj_set_style_text_font(s_stickyLbl, kFontSmall, 0);

    /* "Load earlier messages" — first child of the scroll area, above the bubbles;
     * shown only when the window doesn't reach the oldest message. */
    s_earlierBtn = lv_button_create(s_msgList);
    lv_obj_remove_style_all(s_earlierBtn);
    lv_obj_set_width(s_earlierBtn, lv_pct(100));
    lv_obj_set_height(s_earlierBtn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_earlierBtn, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(s_earlierBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_earlierBtn, 4, 0);
    lv_obj_set_style_pad_ver(s_earlierBtn, 9, 0);
    lv_obj_set_style_margin_ver(s_earlierBtn, 2, 0);
    lv_obj_remove_flag(s_earlierBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_earlierBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(mkLabel(s_earlierBtn, LV_SYMBOL_UP "  Load earlier messages", lv_color_hex(0xc0c8d0)));
    lv_obj_add_event_cb(s_earlierBtn, onLoadEarlier, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_earlierBtn);

    s_bubbles = lv_obj_create(s_msgList);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, lv_pct(100));
    lv_obj_set_height(s_bubbles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_bubbles, 1, 0);
    lv_obj_remove_flag(s_bubbles, LV_OBJ_FLAG_SCROLLABLE);

    /* "▼ N newer messages" — last child, below the bubbles; shown only while the
     * window is paged back off the newest message. */
    s_newerBtn = lv_button_create(s_msgList);
    lv_obj_remove_style_all(s_newerBtn);
    lv_obj_set_width(s_newerBtn, lv_pct(100));
    lv_obj_set_height(s_newerBtn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_newerBtn, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(s_newerBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_newerBtn, 4, 0);
    lv_obj_set_style_pad_ver(s_newerBtn, 9, 0);
    lv_obj_set_style_margin_ver(s_newerBtn, 2, 0);
    lv_obj_remove_flag(s_newerBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_newerBtn, LV_OBJ_FLAG_HIDDEN);
    s_newerLbl = mkLabel(s_newerBtn, LV_SYMBOL_DOWN "  newer", lv_color_hex(0xc0c8d0));
    lv_obj_center(s_newerLbl);
    lv_obj_add_event_cb(s_newerBtn, onLoadNewer, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_newerBtn);

    /* Loading placeholder — floating (out of the flex flow), centered over the
     * scroll area; shown while the deferred load/render runs. */
    s_loading = mkLabel(s_msgList, "<loading conversation>", lv_color_hex(0x8a93a0));
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(s_loading);
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);

    /* Compose row — the LAST item in the scroll stream (part of the messages, not a
     * pinned bar), so it scrolls with them and is simply hidden on a history page
     * (see updatePageButtons). The entry fills the width with the controls stacked
     * to its right (vertical space is precious). Collapsed: just the expand pill
     * right of the 1–4 line field. Expanded: the collapse pill above a 2×-height
     * Send, both right of the 8-line composer. A little spacing sets it off from
     * the bubbles; drag-bar clearance below. A click anywhere in the
     * row focuses the entry (the whole field is the target, not just its text). */
    s_comp = lv_obj_create(s_msgList);
    lv_obj_t* comp = s_comp;
    lv_obj_remove_style_all(comp);
    lv_obj_set_width(comp, lv_pct(100));
    lv_obj_set_height(comp, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(comp, 2 * (2 + lcdPx(3)), 0);   /* a little spacing above — no divider */
    lv_obj_set_style_pad_bottom(comp, lcdPx(12), 0);
    lv_obj_set_style_pad_hor(comp, 4, 0);
    lv_obj_set_style_pad_column(comp, 4, 0);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(comp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(comp, [](lv_event_t*) { lcdInputBoxActivate(s_compose); }, LV_EVENT_CLICKED, nullptr);

    int32_t inH  = lv_font_get_line_height(kFont) + 8;
    int32_t btnW = lcdPx(32);   /* shared width of the expand / collapse pills and the round Send */

    /* Expand pill on the LEFT of the entry — shown only when collapsed. Tapping it
     * grows the quick field into the 8-line composer (whose own collapse pill then
     * lives on the RIGHT, by Send). Content-width, level with the first line. */
    s_composePill = lv_button_create(comp);
    lv_obj_remove_style_all(s_composePill);
    lv_obj_set_size(s_composePill, btnW, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_composePill, lv_color_hex(0x2a313a), 0);
    lv_obj_set_style_bg_opa(s_composePill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_composePill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_ver(s_composePill, 3, 0);
    lv_obj_set_ext_click_area(s_composePill, 8);
    lv_obj_remove_flag(s_composePill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(mkLabel(s_composePill, LV_SYMBOL_UP, lv_color_hex(0xc0c8d0)));
    lv_obj_add_event_cb(s_composePill, [](lv_event_t*) {
        g_composeExpanded = true;                 /* quick(1–4) → 8-line composer */
        applyComposeMode();
        scrollThreadBottom(false);                /* keep the composer's bottom at the screen bottom */
    }, LV_EVENT_CLICKED, nullptr);

    /* Entry (middle, grows to fill the width). Device text behaviours (caret-arrow
     * mode, double-space → ". ", trailing-trim). Keeps keypad focus for the thread's
     * life so typing always lands here, wherever the reader scrolled. */
    s_compose = lcdInputBoxCreate(comp, 1, 4);
    lv_textarea_set_placeholder_text(s_compose, "LXMF Message");
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_pad_ver(s_compose, 1, 0);
    lv_obj_set_style_pad_hor(s_compose, 4, 0);
    lv_obj_set_flex_grow(s_compose, 1);
    /* Any click in the compose row focuses the entry: the entry bubbles its clicks
     * up to comp (whose handler focuses), and comp catches the bare areas directly,
     * so the WHOLE row — not just the entry's text — is the focus target. The pill
     * and Send don't bubble, so they keep their own actions. */
    lv_obj_add_flag(s_compose, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);                 /* Enter = Send (quick field) */
    /* Typing snaps the view to newest. Deferred to after the input box finishes its
     * own value-changed edit (the double-space → ". " rewrite deletes/re-adds
     * chars): a synchronous scroll here would relayout reentrantly mid-edit. */
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) {
        lv_async_call([](void*) { threadSnapNewest(); }, nullptr);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    /* Controls column on the RIGHT — the collapse pill on top, the round Send below
     * it, distributed top-and-bottom (SPACE_BETWEEN). Both show only when expanded
     * (the left expand pill takes over when collapsed). The column is sized to the
     * entry by applyComposeMode. Not in the input group; the compose keeps keypad
     * focus for the thread's life. */
    lv_obj_t* rc = lv_obj_create(comp);
    s_rc = rc;
    lv_obj_remove_style_all(rc);
    lv_obj_set_size(rc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(rc, LV_OBJ_FLAG_SCROLLABLE);

    /* Collapse pill (↓): shrinks the 8-line composer back to the quick field. */
    s_collapsePill = lv_button_create(rc);
    lv_obj_remove_style_all(s_collapsePill);
    lv_obj_set_size(s_collapsePill, btnW, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_collapsePill, lv_color_hex(0x2a313a), 0);
    lv_obj_set_style_bg_opa(s_collapsePill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_collapsePill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_ver(s_collapsePill, 3, 0);
    lv_obj_set_ext_click_area(s_collapsePill, 8);
    lv_obj_remove_flag(s_collapsePill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(mkLabel(s_collapsePill, LV_SYMBOL_DOWN, lv_color_hex(0xc0c8d0)));
    lv_obj_add_event_cb(s_collapsePill, [](lv_event_t*) {
        g_composeExpanded = false;                /* 8-line composer → quick(1–4) */
        applyComposeMode();
        scrollThreadBottom(false);                /* keep the composer's bottom at the screen bottom (no anim) */
    }, LV_EVENT_CLICKED, nullptr);

    /* Send button: the white paper-plane on a blue fill (no text label), a full
     * circle of the shared width. The icon is a runtime-rasterized SVG, set/tinted
     * by applyThreadIcons once it lands. */
    s_send = lv_button_create(rc);
    lv_obj_set_style_pad_all(s_send, 0, 0);
    lv_obj_set_size(s_send, btnW, btnW);                            /* round: diameter = width */
    lv_obj_set_style_radius(s_send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_send, lv_color_hex(0x2f80ed), 0);   /* blue */
    lv_obj_set_style_bg_opa(s_send, LV_OPA_COVER, 0);
    s_sendIcon = lv_image_create(s_send);
    lv_obj_set_size(s_sendIcon, sendIconPx(), sendIconPx());
    lv_obj_center(s_sendIcon);
    applyIcon(s_sendIcon, "send", sendIconPx(), lv_color_white());
    lv_obj_add_event_cb(s_send, onSend, LV_EVENT_CLICKED, nullptr);

    applyComposeMode();   /* start collapsed: quick field, no Send, pill ↑, Enter sends */

    /* History overlay — a second scroll page, a grow-sibling of s_msgList. Hidden
     * until "load earlier"; showing it hides the resident (which stays built, so
     * closing is instant). It carries no composer. */
    s_histWrap = lv_obj_create(s_thread);
    lv_obj_remove_style_all(s_histWrap);
    lv_obj_set_width(s_histWrap, lv_pct(100));
    lv_obj_set_flex_grow(s_histWrap, 1);
    lv_obj_set_flex_flow(s_histWrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_histWrap, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_histWrap, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_histWrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_histWrap, LV_OBJ_FLAG_HIDDEN);

    s_histList = lv_obj_create(s_histWrap);
    lv_obj_remove_style_all(s_histList);
    lv_obj_set_width(s_histList, lv_pct(100));
    lv_obj_set_flex_grow(s_histList, 1);
    lv_obj_set_flex_flow(s_histList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_histList, 4, 0);
    lv_obj_set_style_pad_ver(s_histList, 1, 0);
    lv_obj_set_style_pad_row(s_histList, 1, 0);
    lv_obj_add_event_cb(s_histList, [](lv_event_t*) { updateStickyDate(); }, LV_EVENT_SCROLL, nullptr);

    s_histEarlier = lv_button_create(s_histList);
    lv_obj_remove_style_all(s_histEarlier);
    lv_obj_set_width(s_histEarlier, lv_pct(100));
    lv_obj_set_height(s_histEarlier, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_histEarlier, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(s_histEarlier, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_histEarlier, 4, 0);
    lv_obj_set_style_pad_ver(s_histEarlier, 9, 0);
    lv_obj_set_style_margin_ver(s_histEarlier, 2, 0);
    lv_obj_remove_flag(s_histEarlier, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(mkLabel(s_histEarlier, LV_SYMBOL_UP "  Load earlier messages", lv_color_hex(0xc0c8d0)));
    lv_obj_add_event_cb(s_histEarlier, [](lv_event_t*) {
        if (h_winLo == 0) return;
        h_winLo = h_winLo > PAGE_SIZE ? h_winLo - PAGE_SIZE : 0;
        if (h_winHi - h_winLo > MAXSPAN) h_winHi = h_winLo + MAXSPAN;
        renderHistory();
    }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_histEarlier);

    s_histBubbles = lv_obj_create(s_histList);
    lv_obj_remove_style_all(s_histBubbles);
    lv_obj_set_width(s_histBubbles, lv_pct(100));
    lv_obj_set_height(s_histBubbles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_histBubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_histBubbles, 1, 0);
    lv_obj_remove_flag(s_histBubbles, LV_OBJ_FLAG_SCROLLABLE);

    s_histNewer = lv_button_create(s_histList);
    lv_obj_remove_style_all(s_histNewer);
    lv_obj_set_width(s_histNewer, lv_pct(100));
    lv_obj_set_height(s_histNewer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_histNewer, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(s_histNewer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_histNewer, 4, 0);
    lv_obj_set_style_pad_ver(s_histNewer, 9, 0);
    lv_obj_set_style_margin_ver(s_histNewer, 2, 0);
    lv_obj_remove_flag(s_histNewer, LV_OBJ_FLAG_SCROLLABLE);
    s_histNewerLbl = mkLabel(s_histNewer, LV_SYMBOL_DOWN "  Load newer messages", lv_color_hex(0xc0c8d0));
    lv_obj_center(s_histNewerLbl);
    lv_obj_add_event_cb(s_histNewer, [](lv_event_t*) {
        h_winHi += PAGE_SIZE;
        if (h_winHi > g_winLo) h_winHi = g_winLo;               /* never into the resident page */
        if (h_winHi - h_winLo > MAXSPAN) h_winLo = h_winHi - MAXSPAN;
        renderHistory();
    }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_histNewer);

    /* Bottom bar: jump straight back to the newest (closes the overlay). */
    lv_obj_t* histBack = lv_button_create(s_histWrap);
    lv_obj_remove_style_all(histBack);
    lv_obj_set_width(histBack, lv_pct(100));
    lv_obj_set_height(histBack, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(histBack, lv_color_hex(0x2563a0), 0);
    lv_obj_set_style_bg_opa(histBack, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(histBack, 8, 0);
    lv_obj_remove_flag(histBack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(mkLabel(histBack, LV_SYMBOL_DOWN "  Back to latest", lv_color_white()));
    lv_obj_add_event_cb(histBack, [](lv_event_t*) { threadSnapNewest(); }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), histBack);
}

/* Reflect the active identity's connection state onto the compose field. Until
 * its mailbox is up (the post-reset window while rnsd connects), history is
 * readable but a send would be held — so disable the field and say why, rather
 * than take text that can't go out. Re-run whenever `up` may have flipped. */
void composeReflectUp() {
    if (!s_compose) return;
    if (g_id >= 0 && idUp(g_id)) {
        lv_obj_remove_state(s_compose, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(s_compose, "LXMF Message");
    } else {
        lv_obj_add_state(s_compose, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(s_compose, "Waiting for initialization…");
    }
}

void openThread(const std::string& peer) {
    closeInfo();                            /* e.g. a nomad-link open while the info page is up */
    if (!g_curPeer.empty() && g_curPeer != peer) saveDraft();   /* preserve the conv we're leaving */
    g_curPeer = peer;
    if (!s_thread) buildThreadShell();
    closeHistory();                         /* a fresh conversation opens on its newest page */
    lv_label_set_text(s_threadName, peerName(peer).c_str());
    updateThreadLink();
    startIconSettle();                      /* land send + link icons (they rasterize off-task) */
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    if (s_idpick)   lv_obj_add_flag(s_idpick,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(true);             /* immersive chat: no status bar */
    loadDraft(peer);                        /* restore this conversation's in-progress text */
    composeReflectUp();
    deferFocus(s_compose);                  /* focus always rests on the entry box */
    /* Paint the shell + "<loading conversation>" placeholder NOW; the heavy
     * record load + bubble build (seconds for a long thread) is deferred so the
     * screen never looks frozen. The window resets to the newest page. */
    g_needMsgLoad = true;
    g_scrollAfter = 0;                      /* land at newest once loaded */
    beginThreadLoad();
}

void onContactClick(lv_event_t* e) {
    openThread(*static_cast<std::string*>(lv_event_get_user_data(e)));
}

/* A contact tapped in the nomad browser (lxmf.url_lcd) waits here until an
 * identity is active — opened immediately for a single-identity box, or after
 * the user picks one (onIdPick) when the picker is up. */
void maybeOpenPending() {
    if (g_id < 0 || g_pendingOpenPeer.empty()) return;
    std::string p = g_pendingOpenPeer;
    g_pendingOpenPeer.clear();
    openThread(p);
}

/* The on-device nomad browser tapped an lxmf@<hash> link and wrote the dest
 * hash to lxmf.url_lcd. Runs on the lcd task (subscribed via lcdRun in
 * lxmfLcdRegister), so LVGL is safe here. The core lxmf task issues the path
 * request off the same key; this is UI only. */
void onLcdOpenUrl(const char* /*key*/, const char* val) {
    if (!val || !*val) return;                 /* a clear */
    std::string peer(val);
    size_t colon = peer.find(':');             /* "<hash>[:<nonce>]" */
    if (colon != std::string::npos) peer.erase(colon);
    if (peer.size() != 32) return;             /* 16-byte dest hash = 32 hex */
    for (char c : peer)
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return;
    for (auto& c : peer) if (c>='A'&&c<='Z') c = (char)(c - 'A' + 'a');

    g_pendingOpenPeer = peer;
    lcdShowProgram("LXMF");   /* build (first open) + raise; routes to picker if >1 id */
    maybeOpenPending();       /* single id: opens now; picker: waits for onIdPick */
}

/* ---- thread rendering ---- */

/* ---- Nomad page links in message text ----
 * A message may quote a Nomad page URL "<32hex>:/path". We render those as
 * tappable links; a tap hands the URL to the Nomad browser via nomad.url_lcd
 * (nomad_lcd subscribes, brings itself forward, and navigates). This is the
 * exact reverse of nomad_lcd's lxmf@<hash> links → lxmf.url_lcd. */

/* Match a Nomad page link at line[pos]: 32 hex + ':' + '/' + a run of
 * non-space path chars, not part of a longer hex token. Fills `url` with
 * "<hash>:<path>" (hash lowercased); `len` is its byte length. */
bool matchNomadAt(const std::string& s, size_t pos, std::string& url, size_t& len) {
    auto hex = [](char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    if (pos + 34 > s.size()) return false;                  /* 32 + ':' + '/' */
    for (int k = 0; k < 32; k++) if (!hex(s[pos + k])) return false;
    if (s[pos + 32] != ':' || s[pos + 33] != '/') return false;
    if (pos > 0 && hex(s[pos - 1])) return false;           /* inside a longer hex run */
    size_t e = pos + 33;                                    /* at the '/' */
    while (e < s.size() && !isspace((unsigned char)s[e])) e++;
    while (e > pos + 34 && strchr(".,;:!?)]}'\"", s[e - 1])) e--;   /* trailing punctuation */
    url.assign(s, pos, e - pos);
    for (int k = 0; k < 32; k++) if (url[k] >= 'A' && url[k] <= 'Z') url[k] = (char)(url[k] - 'A' + 'a');
    len = e - pos;
    return true;
}

void onNomadLinkClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= g_nomadTargets.size()) return;
    /* "<hash>:<path>|<nonce>" — nonce makes repeat taps re-fire (nomad_lcd
     * strips it). Path fits comfortably; guard the buffer regardless. */
    char v[160];
    snprintf(v, sizeof v, "%s|%u", g_nomadTargets[idx].c_str(), (unsigned)millis());
    storageSet("nomad.url_lcd", v);
}

/* Render a message body into the bubble. No link → one wrapping label, exactly
 * as before. With link(s) → the body is split at link boundaries into plain
 * labels and clickable blue link labels, each wrapping within the bubble; they
 * stack in the bubble's column. (A label can't make a substring clickable, so
 * a link becomes its own widget — the same constraint nomad_lcd handles for
 * lxmf@ links.) */
/* Returns the last plain text label when the message has no Nomad links (so the
 * inline-timestamp path can measure its last line); nullptr when it also holds
 * link widgets. */
lv_obj_t* addBubbleText(lv_obj_t* bub, const std::string& content) {
    std::string body = printable(content, false);   /* drop unrenderables, keep newlines */

    /* One label per line — a CR ends a label. A long body would otherwise be a
     * single mega-label that LVGL re-wraps in full on every reflow; splitting it
     * keeps each label's layout cheap. Returns the last label made in [from,to). */
    auto addText = [&](size_t from, size_t to) -> lv_obj_t* {
        if (to <= from) return nullptr;              /* empty run (e.g. leading/adjacent link) */
        lv_obj_t* last = nullptr;
        size_t s = from;
        while (s <= to) {
            size_t nl = body.find('\n', s);
            size_t e = (nl == std::string::npos || nl > to) ? to : nl;
            lv_obj_t* l = mkLabel(bub, body.substr(s, e - s), lv_color_white());
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_max_width(l, 228, 0);
            last = l;
            if (e == to) break;
            s = e + 1;                               /* past the CR */
        }
        return last;
    };

    size_t i = 0, textStart = 0;
    bool any = false;
    while (i < body.size()) {
        std::string url; size_t len;
        if (matchNomadAt(body, i, url, len)) {
            addText(textStart, i);
            lv_obj_t* l = mkLabel(bub, url, lv_color_hex(0x6db3ff));
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_max_width(l, 228, 0);
            lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(l, 4);
            size_t idx = g_nomadTargets.size();
            g_nomadTargets.push_back(url);
            lv_obj_add_event_cb(l, onNomadLinkClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
            if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), l);
            i += len;
            textStart = i;
            any = true;
        } else {
            i++;
        }
    }
    if (!any) return addText(0, body.size());        /* single plain label */
    addText(textStart, body.size());
    return nullptr;                                   /* has links → not a single label */
}

/* Fill a bubble's meta row: the ALL-CAPS status name (outbound only, left, in
 * smaller print) at one end; the timestamp + delivery glyph at the other. Rebuilt
 * whole on a status/tries change — three small labels, and only on an actual
 * transition. Glyph: DELIVERED ✓✓ green · CANCELLED ✕ grey · gave-up
 * (tries == 255) ✕ red · else … in flight. Inbound shows only the timestamp. */
/* Yellow "L" pill — appended to a bubble's meta row (rightmost) when the
 * message travelled a LoRa interface. Modelled on the sticky-date pill. */
void addLoraPill(lv_obj_t* meta) {
    lv_obj_t* pill = lv_obj_create(meta);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xffd400), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, lcdPx(6), 0);
    lv_obj_set_style_pad_hor(pill, lcdPx(3), 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* l = lv_label_create(pill);
    lv_obj_set_style_text_font(l, kFontSmall, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, "L");
}

void fillMeta(lv_obj_t* meta, const Msg& m) {
    lv_obj_clean(meta);

    /* Status name (outbound, tiny) on the LEFT — SUPPRESSED for DELIVERED (the ✓✓
     * says it), single line so a long name never stacks (DE-LIV-ER-ED). */
    bool showStatus = !m.in && m.status != LXMF_ST_DELIVERED;
    if (showStatus) {
        lv_obj_t* st = lv_label_create(meta);
        lv_obj_set_style_text_font(st, kFontTiny, 0);
        lv_obj_set_style_text_color(st, lv_color_hex(0x8a93a0), 0);
        lv_label_set_long_mode(st, LV_LABEL_LONG_CLIP);
        lv_label_set_text(st, lxmfStatusName(m.status));
    }

    /* Grow-spacer that pushes the time + glyph to the RIGHT — after the status when
     * there is one (a 22px min-gap that also sizes short bubbles), or LEADING when
     * there isn't (inbound / delivered), so the time is right-aligned there too. */
    lv_obj_t* sp = lv_obj_create(meta);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, showStatus ? 22 : 1, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

    char tbuf[64] = "";
    if (m.ts > 0) {
        time_t tt = m.ts;
        struct tm tmv {};
        localtime_r(&tt, &tmv);
        /* Per-message time format is a user setting honoured here + in the web UI. */
        std::string fmt = storageGetStr("s.lxmf.msg_time_format", "%H:%M");
        strftime(tbuf, sizeof tbuf, fmt.c_str(), &tmv);
    }
    lv_obj_t* tl = lv_label_create(meta);        /* time (right, small) */
    lv_obj_set_style_text_font(tl, kFontSmall, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xc0c8d0), 0);
    lv_label_set_text(tl, tbuf);

    if (!m.in) {
        const char* sym = "...";                     /* queued/requesting/sending/awaiting → in flight */
        lv_color_t  col = lv_color_hex(0x8a93a0);
        if (m.status == LXMF_ST_DELIVERED)      { sym = LV_SYMBOL_OK LV_SYMBOL_OK; col = lv_color_hex(0x4abf6a); }
        else if (m.status == LXMF_ST_CANCELLED) { sym = LV_SYMBOL_CLOSE;           col = lv_color_hex(0x8a93a0); }
        else if (m.tries == LXMF_TRIES_GAVEUP)  { sym = LV_SYMBOL_CLOSE;           col = lv_color_hex(0xd9534f); }
        lv_obj_t* ic = lv_label_create(meta);    /* delivery glyph (small) */
        lv_obj_set_style_text_font(ic, kFontSmall, 0);
        lv_obj_set_style_text_color(ic, col, 0);
        lv_label_set_text(ic, sym);
    }
}

/* Pin the time to the bubble's right at ALL widths: measure the meta line's
 * natural width, reserve it as the bubble's min-width, then let the meta fill the
 * bubble so its grow-spacer spreads status-left / time-right. Called after every
 * fillMeta (bubble grows/shrinks as the status name changes). */
void fitBubbleMeta(lv_obj_t* meta) {
    if (!meta) return;
    lv_obj_t* bub = lv_obj_get_parent(meta);
    if (!bub) return;
    lv_obj_set_width(meta, LV_SIZE_CONTENT);     /* measure the natural line width */
    lv_obj_update_layout(meta);
    int32_t mw = lv_obj_get_width(meta) + 12;    /* + the bubble's horizontal padding */
    lv_obj_set_style_min_width(bub, mw > 60 ? mw : 60, 0);
    lv_obj_set_width(meta, lv_pct(100));         /* fill the bubble → spacer spreads → time right */
}

/* WhatsApp-style inline meta: if the message's last line ends with room for the
 * meta to its right — within the max bubble width — pin the meta to the bubble's
 * RIGHT on that line, nudged 3px lower. The bubble widens just enough to clear the
 * text by GAP; a single-line message grows toward max width to make room; if it
 * still won't fit, we bail and the caller drops the meta to its own line.
 * Returns true when inlined. `textLabel` is the message's LAST text label. */
bool tryInlineMeta(lv_obj_t* bub, lv_obj_t* textLabel, lv_obj_t* meta) {
    if (!bub || !textLabel || !meta) return false;
    lv_obj_set_width(meta, LV_SIZE_CONTENT);
    lv_obj_update_layout(bub);
    int32_t metaW = lv_obj_get_width(meta);
    int32_t lineH = lv_font_get_line_height(kFont);

    const char* lt = lv_label_get_text(textLabel);
    lv_point_t pos;
    /* char_id past the end clamps to the text end → position after the last glyph.
     * A byte count is >= the UTF-8 letter count, so it always lands there. pos is in
     * the LABEL's own space; add the label's offset within the bubble content so a
     * CR-split body tucks onto its last label's last line, not the first. */
    lv_label_get_letter_pos(textLabel, (uint32_t)strlen(lt), &pos);
    lv_area_t la, ba;
    lv_obj_get_coords(textLabel, &la);
    lv_obj_get_content_coords(bub, &ba);
    int32_t labelTop = la.y1 - ba.y1;

    int32_t metaH = lv_obj_get_height(meta);
    const int32_t GAP = 12;   /* min horizontal space between the text and the meta */
    if (pos.x + GAP + metaW > 228) return false;       /* won't fit even at max width */

    lv_obj_add_flag(meta, LV_OBJ_FLAG_FLOATING);       /* out of the column flow */
    lv_obj_set_style_margin_top(meta, 0, 0);
    /* Widen just enough that the meta clears the last line's text by GAP — a wider
     * line elsewhere keeps its width (this min is only a floor). */
    int32_t need = pos.x + GAP + metaW + 12;           /* + bubble pad_hor */
    lv_obj_set_style_min_width(bub, need > 60 ? need : 60, 0);
    /* Pin to the bubble's RIGHT (not just after the last word). The meta font is
     * smaller than the text, so sit it at the last line's BOTTOM, then nudge 3px
     * lower — the balloon grows by exactly that 3px spill. */
    int32_t off = lcdPx(3);
    int32_t y = labelTop + pos.y + (lineH - metaH) + off;
    lv_obj_set_style_pad_bottom(bub, off, 0);
    lv_obj_align(meta, LV_ALIGN_TOP_RIGHT, 0, y);
    return true;
}

/* Place the meta after a (re)fill: tuck it onto the message's last line when it
 * fits (single text label only), else pin it right on its own line. Applies to
 * every message — sent, delivered, inbound. Resets prior inline/own-line state
 * first so a status transition re-lays-out cleanly both ways. */
void layoutMeta(lv_obj_t* bub, lv_obj_t* meta, uint8_t /*status*/) {
    if (!bub || !meta) return;
    lv_obj_remove_flag(meta, LV_OBJ_FLAG_FLOATING);                        /* back into the column */
    lv_obj_set_style_margin_top(meta, lv_font_get_line_height(kFont) / 3, 0);
    lv_obj_set_style_pad_bottom(bub, 0, 0);                                /* undo any ⅓-line growth */
    lv_obj_set_style_min_width(bub, 60, 0);
    /* Inline onto the last text label (the child just before meta) — but only when
     * no child is a link widget, so a floated meta never covers a tappable link. */
    lv_obj_t* textLabel = nullptr;
    uint32_t n = lv_obj_get_child_count(bub);
    if (n >= 2) {
        bool hasLink = false;
        for (uint32_t i = 0; i + 1 < n; i++)
            if (lv_obj_has_flag(lv_obj_get_child(bub, i), LV_OBJ_FLAG_CLICKABLE)) { hasLink = true; break; }
        if (!hasLink) textLabel = lv_obj_get_child(bub, n - 2);
    }
    if (textLabel && textLabel != meta && tryInlineMeta(bub, textLabel, meta))
        return;
    fitBubbleMeta(meta);
}

/* Extra gap above a bubble, scaled with the UI zoom: 1px baseline between all
 * balloons, +3px when it's more than 15 min since the previous message. */
int bubbleTopMargin(long prevAnchor, long curAnchor) {
    int m = lcdPx(1);
    if (prevAnchor > 0 && curAnchor > 0 && (curAnchor - prevAnchor) > 15 * 60)
        m += lcdPx(3);
    return m;
}

BubbleRef addBubble(const Msg& m, int topMargin, lv_obj_t* container = nullptr) {
    lv_obj_t* row = lv_obj_create(container ? container : s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_margin_top(row, topMargin, 0);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, m.in ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bub = lv_obj_create(row);
    lv_obj_remove_style_all(bub);
    lv_obj_set_width(bub, LV_SIZE_CONTENT);
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(bub, 60, 0);           /* fit a timestamp even under a 2-char message */
    lv_obj_set_style_max_width(bub, 240, 0);          /* fixed px — a pct of a content-sized chain collapses */
    lv_obj_set_style_bg_color(bub, m.in ? lv_color_hex(0x2a313a) : lv_color_hex(0x2563a0), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 6, 0);
    lv_obj_set_style_pad_ver(bub, 1, 0);
    lv_obj_set_style_pad_hor(bub, 6, 0);
    lv_obj_set_flex_flow(bub, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(bub, LV_OBJ_FLAG_SCROLLABLE);

    addBubbleText(bub, m.content);                    /* text + tappable Nomad links, wrapped to 228 */

    /* Meta row: status name + timestamp + delivery glyph. CONTENT-sized (not
     * full-width) so it becomes a lower bound on the bubble width — a bubble under
     * a tiny message ("test") grows to fit the whole line instead of clipping the
     * time/checkmarks. Re-rendered via fillMeta on a status change. */
    lv_obj_t* meta = lv_obj_create(bub);
    lv_obj_remove_style_all(meta);
    lv_obj_set_width(meta, LV_SIZE_CONTENT);
    lv_obj_set_height(meta, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta, 4, 0);
    lv_obj_set_style_margin_top(meta, lv_font_get_line_height(kFont) / 3, 0);   /* ~⅓-line gap above */
    lv_obj_remove_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
    fillMeta(meta, m);
    layoutMeta(bub, meta, m.status);

    /* Long-press a bubble → its detail screen. Clickable so LVGL raises the
     * long-press; the record key rides as freed-with-widget user_data. */
    lv_obj_add_flag(bub, LV_OBJ_FLAG_CLICKABLE);
    bindMsgDetail(bub, m.key);

    return BubbleRef{ m.key, m.status, m.tries, meta };
}

/* ---- day separators (anchored to recv_ts) + floating sticky date ---- */

bool sameLocalDay(long a, long b) {
    if (a <= 0 || b <= 0) return a == b;
    time_t ta = a, tb = b; struct tm x{}, y{};
    localtime_r(&ta, &x); localtime_r(&tb, &y);
    return x.tm_year == y.tm_year && x.tm_yday == y.tm_yday;
}
void dayLabel(long recv_ts, char* buf, size_t n) {
    buf[0] = 0;
    if (recv_ts > 0) { time_t t = recv_ts; struct tm tmv {}; localtime_r(&t, &tmv);
                       strftime(buf, n, "%a %d %b", &tmv); }
}

/* A centered date pill in the bubble column, before the first message of a day.
 * `anchor` is recv_ts (or ts when recv_ts is unset) — the caller guarantees > 0,
 * so the pill is never an empty (grey-circle) label. Distinct pale-yellow look.
 * `continued` appends " (continued)" — used on the window's top separator when
 * earlier messages of the same date sit above the loaded window. */
void addDateSep(long anchor, bool continued = false, lv_obj_t* container = nullptr,
                std::vector<std::pair<lv_obj_t*, long>>* seps = nullptr) {
    char buf[48]; dayLabel(anchor, buf, sizeof buf);
    if (continued) { size_t n = strlen(buf); snprintf(buf + n, sizeof buf - n, " (continued)"); }
    lv_obj_t* wrap = lv_obj_create(container ? container : s_bubbles);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, lv_pct(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(wrap, 3, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* pill = lv_label_create(wrap);
    lv_obj_set_style_text_font(pill, kFontSmall, 0);
    lv_obj_set_style_text_color(pill, lv_color_black(), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xffffcc), 0);   /* nearest RGB565 pale yellow */
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 4, 0);
    lv_obj_set_style_pad_hor(pill, 8, 0);
    lv_obj_set_style_pad_ver(pill, 1, 0);
    lv_label_set_text(pill, buf);
    (seps ? *seps : g_dateSeps).push_back({ wrap, anchor });
}

void stickyFadeCb(lv_timer_t*) {
    g_stickyFadeTimer = nullptr;
    if (s_stickyDate) lv_obj_fade_out(s_stickyDate, 400, 0);
}

/* On scroll: float the day of the content at the top of the viewport as a pill —
 * always a date while scrolling, suppressed only when that day's own inline
 * separator is itself at the top (it's already showing the date). Re-arms the 2 s
 * fade. (The "(continued)" marker lives on the fixed top-of-window separator, not
 * here.) Parameterized on the scrolling list / bubble column / seps so the resident
 * page and the history overlay share one pill (s_stickyDate lives above both). */
void updateStickyFor(lv_obj_t* list, lv_obj_t* bubbles,
                     std::vector<std::pair<lv_obj_t*, long>>& seps) {
    if (!s_stickyDate || !list || !bubbles) return;
    if (seps.empty()) { lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN); return; }
    int32_t top    = lv_obj_get_scroll_y(list);
    int32_t bubOff = lv_obj_get_y(bubbles);
    long    curDay = 0;
    bool    haveCur = false, sepAtTop = false;
    long    firstDay = 0;                                    /* oldest sep = topmost content's day */
    for (auto& s : seps) {
        if (!lv_obj_is_valid(s.first)) continue;
        int32_t y = bubOff + lv_obj_get_y(s.first);
        if (firstDay == 0) firstDay = s.second;              /* seps are oldest-first */
        if (y <= top + 1) { curDay = s.second; haveCur = true; }   /* last day above the top */
        if (y >= top && y <= top + 26) sepAtTop = true;      /* an inline separator is at the top */
    }
    if (!haveCur) curDay = firstDay;                         /* scrolled above the first separator */
    if (curDay <= 0 || sepAtTop) {                           /* no date to show / inline sep is doing it */
        lv_anim_del(s_stickyDate, nullptr);
        lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN);   /* instant, not a fade — no mid-scroll flicker */
        return;
    }
    char buf[32]; dayLabel(curDay, buf, sizeof buf);
    lv_label_set_text(s_stickyLbl, buf);
    lv_anim_del(s_stickyDate, nullptr);                      /* cancel a running fade */
    lv_obj_set_style_opa(s_stickyDate, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN);
    if (g_stickyFadeTimer) lv_timer_reset(g_stickyFadeTimer);
    else { g_stickyFadeTimer = lv_timer_create(stickyFadeCb, 2000, nullptr);
           lv_timer_set_repeat_count(g_stickyFadeTimer, 1); }
}

/* Whichever page is on screen drives the pill (only one is visible at a time). */
void updateStickyDate() {
    if (s_histWrap && !lv_obj_has_flag(s_histWrap, LV_OBJ_FLAG_HIDDEN))
        updateStickyFor(s_histList, s_histBubbles, g_histDateSeps);
    else
        updateStickyFor(s_msgList, s_bubbles, g_dateSeps);
}

void rebuildThread() {
    if (!s_bubbles || g_curPeer.empty()) return;
    refreshFont();

    /* Order = arrival order = the record store's arena order (g_msgs is loaded in
       that order). We deliberately do NOT sort by ts: the ESP RTC drifts too far
       to order messages reliably, whereas insertion order is exactly "as
       received/sent". This also keeps new messages strictly at the end, so the
       reconcile below only ever appends. */
    std::vector<const Msg*> all;
    all.reserve(g_msgs.size());
    for (auto& m : g_msgs) if (m.peer == g_curPeer) all.push_back(&m);
    size_t total = all.size();

    /* Only the [g_winLo, g_winHi) window is rendered. While pinned to newest the
       bottom edge follows total, so a live arrival appends into the window (the
       reconcile stays append-only); paging back bounds the window and freezes it
       until you navigate. */
    if (g_atNewest) g_winHi = total;
    if (g_winHi > total) g_winHi = total;
    if (g_winLo > g_winHi) g_winLo = g_winHi;
    updatePageButtons(total);

    std::vector<const Msg*> ms(all.begin() + g_winLo, all.begin() + g_winHi);

    /* Date anchor = recv_ts, or ts when recv_ts is unset (an optimistic just-sent
       bubble, or anything an older writer left at 0) — so a message never produces
       an empty (grey-circle) separator. prevAnchor(i) is the message above ms[i]
       (or above the window when i == 0). */
    auto anchorOf   = [](const Msg* m) -> long { return m->recv_ts > 0 ? m->recv_ts : m->ts; };
    auto prevAnchor = [&](size_t i) -> long {
        if (i > 0) return anchorOf(ms[i - 1]);
        return g_winLo > 0 ? anchorOf(all[g_winLo - 1]) : 0;
    };
    /* Add a date separator before ms[i] when the day changes. The window's first
       message (i == 0) ALWAYS gets one so the top of the scroll always carries a
       date — marked "(continued)" when the message just above the window shares
       that date (earlier same-day messages are still off-window, load-earlier). */
    auto maybeSep = [&](size_t i) {
        long a = anchorOf(ms[i]);
        if (a <= 0) return;
        if (i == 0) addDateSep(a, g_winLo > 0 && sameLocalDay(prevAnchor(0), a));
        else if (!sameLocalDay(prevAnchor(i), a)) addDateSep(a);
    };

    bool wasAtBottom = threadAtBottom();   /* pinned to newest before this update? */

    /* Reconcile against the rendered bubbles instead of clear+rebuild. If the
       existing bubbles are a prefix of the current message list, update any
       changed stage glyph in place and append the newcomers — one widget per new
       message, not a wholesale rebuild of the (possibly huge) thread. Only a
       structural change (delete/reorder) falls back to a full rebuild. */
    size_t common = std::min(ms.size(), g_bubbles.size());
    size_t matchN = 0;
    while (matchN < common && ms[matchN]->key == g_bubbles[matchN].mid) matchN++;

    if (matchN == g_bubbles.size()) {                    /* existing bubbles are a prefix */
        bool resized = false;
        for (size_t i = 0; i < g_bubbles.size(); i++)
            if (ms[i]->status != g_bubbles[i].status || ms[i]->tries != g_bubbles[i].tries) {
                fillMeta(g_bubbles[i].meta, *ms[i]);     /* status name + glyph re-render */
                layoutMeta(lv_obj_get_parent(g_bubbles[i].meta), g_bubbles[i].meta, ms[i]->status);
                g_bubbles[i].status = ms[i]->status;
                g_bubbles[i].tries  = ms[i]->tries;
                resized = true;                          /* the hug/unhug changes bubble height */
            }
        bool appended = false;
        for (size_t i = g_bubbles.size(); i < ms.size(); i++) {
            maybeSep(i);
            g_bubbles.push_back(addBubble(*ms[i], bubbleTopMargin(prevAnchor(i), anchorOf(ms[i]))));
            appended = true;
        }
        /* Re-pin the composer on ANY height change — a new bubble OR a status
         * re-render that grows/shrinks an existing one (post-delivery hug) — else
         * a shrink lifts the entry off the bottom. */
        if (appended || resized) {
            if (wasAtBottom) scrollThreadBottom(false);  /* stay pinned to newest */
            else             updateThreadDownVis();       /* scrolled up reading history — don't yank */
        }
        if (appended && wasAtBottom && lcdAwake()) markRead(g_curPeer);   /* new msg in view → keep unread at 0 */
        return;
    }

    /* Fallback: structural change or a different conversation — full rebuild. */
    lv_obj_clean(s_bubbles);
    g_nomadTargets.clear();          /* link widgets are gone with the cleaned bubbles */
    g_dateSeps.clear();              /* separator widgets went with the clean */
    g_bubbles.clear();
    for (size_t i = 0; i < ms.size(); i++) {
        maybeSep(i);
        g_bubbles.push_back(addBubble(*ms[i], bubbleTopMargin(prevAnchor(i), anchorOf(ms[i]))));
    }
    scrollThreadBottom(false);                          /* newest + compose at bottom */
}

/* ---- history overlay (older messages, browsed off the resident newest page) ---- */

void updateHistButtons(size_t /*total*/) {
    if (s_histEarlier) { if (h_winLo > 0) lv_obj_remove_flag(s_histEarlier, LV_OBJ_FLAG_HIDDEN);
                         else              lv_obj_add_flag(s_histEarlier, LV_OBJ_FLAG_HIDDEN); }
    /* "Load newer" shows when the window doesn't reach the resident boundary. No
     * count — it excludes the resident's own messages, so any number would mislead. */
    if (s_histNewer) { if (h_winHi < g_winLo) lv_obj_remove_flag(s_histNewer, LV_OBJ_FLAG_HIDDEN);
                       else                    lv_obj_add_flag(s_histNewer, LV_OBJ_FLAG_HIDDEN); }
}

/* Render the [h_winLo, h_winHi) window into the overlay. Read-only (no reconcile):
 * a page of history is a static snapshot, cleaned and rebuilt on each paging. The
 * top separator gets "(continued)" when the message just above the window shares
 * its day, exactly as the resident page does. */
void renderHistory() {
    if (!s_histBubbles || g_curPeer.empty()) return;
    std::vector<const Msg*> all;
    for (auto& m : g_msgs) if (m.peer == g_curPeer) all.push_back(&m);
    size_t total = all.size();
    if (h_winHi > g_winLo) h_winHi = g_winLo;   /* never overlap the resident page */
    if (h_winHi > total)   h_winHi = total;
    if (h_winLo > h_winHi)  h_winLo = h_winHi;

    lv_obj_clean(s_histBubbles);
    g_histDateSeps.clear();

    std::vector<const Msg*> ms(all.begin() + h_winLo, all.begin() + h_winHi);
    auto anchorOf   = [](const Msg* m) -> long { return m->recv_ts > 0 ? m->recv_ts : m->ts; };
    auto prevAnchor = [&](size_t i) -> long {
        if (i > 0) return anchorOf(ms[i - 1]);
        return h_winLo > 0 ? anchorOf(all[h_winLo - 1]) : 0;
    };
    auto maybeSep = [&](size_t i) {
        long a = anchorOf(ms[i]);
        if (a <= 0) return;
        if (i == 0) addDateSep(a, h_winLo > 0 && sameLocalDay(prevAnchor(0), a), s_histBubbles, &g_histDateSeps);
        else if (!sameLocalDay(prevAnchor(i), a)) addDateSep(a, false, s_histBubbles, &g_histDateSeps);
    };
    for (size_t i = 0; i < ms.size(); i++) {
        maybeSep(i);
        addBubble(*ms[i], bubbleTopMargin(prevAnchor(i), anchorOf(ms[i])), s_histBubbles);
    }
    updateHistButtons(total);
    lv_obj_update_layout(s_histList);
    lv_obj_scroll_to_y(s_histList, LV_COORD_MAX, LV_ANIM_OFF);   /* the join with the resident page */
}

/* Show the overlay at the page just before the resident's oldest, hiding the
 * resident (it stays built). No-op when nothing older exists. */
void openHistory() {
    if (!s_histWrap || g_winLo == 0) return;
    h_winHi = g_winLo;
    h_winLo = h_winHi > PAGE_SIZE ? h_winHi - PAGE_SIZE : 0;
    if (s_stickyDate) lv_obj_add_flag(s_stickyDate, LV_OBJ_FLAG_HIDDEN);
    if (s_msgList)    lv_obj_add_flag(s_msgList,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_histWrap, LV_OBJ_FLAG_HIDDEN);   /* show FIRST — a hidden list can't lay out */
    renderHistory();                                      /* then render + land at the bottom */
}

/* Hide the overlay and reveal the resident newest page — instant, no re-render. */
void closeHistory() {
    if (!s_histWrap || lv_obj_has_flag(s_histWrap, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(s_histWrap, LV_OBJ_FLAG_HIDDEN);
    if (s_msgList) lv_obj_remove_flag(s_msgList, LV_OBJ_FLAG_HIDDEN);
}

/* ---- list rendering (two tabs: Contacts + On the Mesh) ---- */

struct Conv { std::string peer, preview; long ts = 0; int unread = 0; long read_ts = 0; int count = 0; };

/* Conversation list built from the maintained directory (contacts.<peer>.*),
   NOT by walking messages — O(conversations), and it touches only the small
   cJSON contacts subtree, never the record store. Populated via storageForEach
   into g_convScan (function-pointer callback, so a file-scope accumulator). */
std::vector<Conv> g_convScan;
void convCb(const char* key, const char* val) {
    /* key = "s.lxmf.id.N.contacts.<peer>.<field>" */
    const char* rest = strstr(key, ".contacts.");
    if (!rest) return;
    rest += sizeof(".contacts.") - 1;
    const char* dot = strchr(rest, '.');
    if (!dot) return;
    std::string peer(rest, dot - rest);
    const char* field = dot + 1;
    if (strchr(field, '.')) return;   /* leaf field only */
    /* Leaves of one peer arrive contiguously (per-record store / sorted walk),
     * so only the last accumulator can match — O(1) per leaf, not O(N). */
    Conv* c = (!g_convScan.empty() && g_convScan.back().peer == peer)
                  ? &g_convScan.back() : nullptr;
    if (!c) { Conv nc; nc.peer = peer; g_convScan.push_back(std::move(nc)); c = &g_convScan.back(); }
    if      (!strcmp(field, "count"))   c->count   = val ? atoi(val) : 0;
    else if (!strcmp(field, "last_ts")) c->ts      = val ? atol(val) : 0;
    else if (!strcmp(field, "unread"))  c->unread  = val ? atoi(val) : 0;
    else if (!strcmp(field, "read_ts")) c->read_ts = val ? atol(val) : 0;
    else if (!strcmp(field, "preview")) c->preview = val ? val : "";
}

/* Compact "time since" badge shown at the right of every row: how long ago we
 * last heard this dest announce. Chat-app style — s / m(inutes) / h / d / w / y.
 * Empty string (drawn as nothing) when we've never heard it. */
/* Monotonic seconds since boot — the domain announce stamps live in (lxmf.cpp
 * writes them from esp_timer, not wall time, so an offline device still ages
 * them correctly). Age math must diff against this, NOT time(nullptr): mixing a
 * wall-clock now with a since-boot stamp yields a ~epoch-sized age, which made
 * the on-mesh expiry filter reject every announce (empty "On the Mesh" tab). */
long nowMonoS() { return (long)(esp_timer_get_time() / 1000000); }

std::string relAge(long secs) {
    if (secs < 0) secs = 0;
    char b[12];
    if      (secs < 60)          snprintf(b, sizeof b, "%lds", secs);
    else if (secs < 3600)        snprintf(b, sizeof b, "%ldm", secs / 60);
    else if (secs < 86400)       snprintf(b, sizeof b, "%ldh", secs / 3600);
    else if (secs < 7*86400L)    snprintf(b, sizeof b, "%ldd", secs / 86400L);
    else if (secs < 365*86400L)  snprintf(b, sizeof b, "%ldw", secs / (7*86400L));
    else                         snprintf(b, sizeof b, "%ldy", secs / (365*86400L));
    return b;
}

/* Last-heard-announce timestamp for a dest (0 = never). g_anns is kept sorted by
 * .last descending, so the first hit is the most recent. */
long lastAnnounce(const std::string& peer) {
    auto it = g_annIx.find(peer);
    return it != g_annIx.end() ? g_anns[it->second].last : 0;
}

/* ---- contact info page (mirrors the web ContactCard) ----
 * A full-size page on the app layer showing peer name, the destination hash
 * grouped in fours for eye comparison, the last-heard line, and the
 * delete-conversation flow behind an explicit confirm. The screen it covers is
 * hidden while it's up (so keyboard focus can't reach covered widgets) and
 * restored by the back chevron — thread or list, whichever opened it. */

std::string groupHash(const std::string& h) {
    std::string out;
    out.reserve(h.size() + h.size() / 4);
    for (size_t i = 0; i < h.size(); i++) {
        if (i && i % 4 == 0) out += ' ';
        out += h[i];
    }
    return out;
}

void closeInfo() {
    if (s_info) lv_obj_delete(s_info);   /* s_confirm is a child — dies with it */
    s_info = nullptr;
    s_confirm = nullptr;
    g_infoPeer.clear();
}

void closeConfirm() {
    if (s_confirm) lv_obj_delete(s_confirm);
    s_confirm = nullptr;
}

void infoBack() {
    bool fromThread = g_infoFromThread;
    closeInfo();
    if (fromThread && s_thread && !g_curPeer.empty()) {
        lv_obj_remove_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
        rebuildThread();          /* pick up anything that arrived while covered */
        composeReflectUp();
        deferFocus(s_compose);
    } else {
        showContacts();           /* refreshes announces + rebuilds the rows */
    }
}

void onConfirmDelete(lv_event_t*) {
    if (g_id >= 0 && !g_infoPeer.empty()) {
        /* Whole-conversation delete: lxmf wipes the msgs + contact subtree; the
         * storage change rebuilds the list without this peer. */
        char k[48];
        snprintf(k, sizeof k, "lxmf.id.%d.cmd.delete", g_id);
        storageSet(k, g_infoPeer.c_str());
    }
    closeInfo();
    showContacts();               /* the conversation is gone from either origin */
}

lv_obj_t* confirmButton(lv_obj_t* parent, const char* text, lv_color_t bg) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_t* l = mkLabel(b, text, lv_color_white());
    lv_obj_center(l);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

void showDeleteConfirm(lv_event_t*) {
    if (!s_info || s_confirm) return;

    /* Full-page scrim absorbing taps outside the box. */
    s_confirm = lv_obj_create(s_info);
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_FLOATING);   /* opt out of the page's column flow */
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_50, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_confirm);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(80));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = mkLabel(box, "Are you sure? This cannot be undone!", lv_color_white());
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));

    lv_obj_t* btns = lv_obj_create(box);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* del = confirmButton(btns, "Delete", lv_color_hex(0x5a2a2a));
    lv_obj_add_event_cb(del, onConfirmDelete, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cnc = confirmButton(btns, "Cancel", lv_color_hex(0x2a313a));
    lv_obj_add_event_cb(cnc, [](lv_event_t*) { closeConfirm(); }, LV_EVENT_CLICKED, nullptr);
    deferFocus(cnc);   /* safe default under the cursor */
}

void showInfo(const std::string& peer) {
    refreshFont();
    g_infoFromThread = s_thread && !lv_obj_has_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
    if (s_thread)   lv_obj_add_flag(s_thread,   LV_OBJ_FLAG_HIDDEN);
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    if (s_info) closeInfo();      /* rebuilt fresh per open — content is static */
    g_infoPeer = peer;

    s_info = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_info);
    lv_obj_set_size(s_info, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_info, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_info, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);

    /* Header: back chevron + peer name (same bar as the thread's). */
    lv_obj_t* hdr = lv_obj_create(s_info);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { infoBack(); }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), back);

    lv_obj_t* title = mkLabel(hdr, peerName(peer), lv_color_white());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 28, 0);

    /* Body: scrollable column. */
    lv_obj_t* body = lv_obj_create(s_info);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_style_pad_ver(body, 6, 0);
    lv_obj_set_style_pad_row(body, 4, 0);

    mkLabel(body, "Destination hash", lv_color_hex(0x8a93a0));
    lv_obj_t* hash = mkLabel(body, groupHash(peer), lv_color_hex(0xc8d8c8));
    lv_label_set_long_mode(hash, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hash, lv_pct(100));

    long la = lastAnnounce(peer);
    if (la > 0)
        mkLabel(body, "Heard on the mesh " + relAge(nowMonoS() - la) + " ago",
                lv_color_hex(0x8a93a0));

    lv_obj_t* del = lv_button_create(body);
    lv_obj_remove_style_all(del);
    lv_obj_set_width(del, lv_pct(100));
    lv_obj_set_height(del, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x5a2a2a), 0);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(del, 4, 0);
    lv_obj_set_style_pad_ver(del, 4, 0);
    lv_obj_set_style_margin_top(del, 10, 0);
    lv_obj_t* dl = mkLabel(del, "Delete conversation", lv_color_white());
    lv_obj_center(dl);
    lv_obj_add_event_cb(del, showDeleteConfirm, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), del);

    deferFocus(back);
}

/* Each clickable list row carries its own peer string as event user-data (freed
 * with the widget), so a row can be reused / moved during a reconcile without a
 * shared index vector going stale. */
void onRowDeletePeer(lv_event_t* e) {
    delete static_cast<std::string*>(lv_event_get_user_data(e));
}
void bindPeer(lv_obj_t* o, lv_event_cb_t cb, const std::string& peer) {
    auto* p = new std::string(peer);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(o, onRowDeletePeer, LV_EVENT_DELETE, p);
}

/* ---- per-message detail page (long-press a bubble) ---- */

/* A grey field label + a wrapped white value beneath it, in the detail body. */
void detailRow(lv_obj_t* body, const char* k, const std::string& v) {
    mkLabel(body, k, lv_color_hex(0x8a93a0));
    lv_obj_t* val = mkLabel(body, v.empty() ? "\xE2\x80\x94" : v, lv_color_hex(0xe0e0e0));  /* — */
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(val, lv_pct(100));
}

void closeMsgDetail() {
    if (s_msgDetail) lv_obj_delete(s_msgDetail);
    s_msgDetail = nullptr;
}

void msgDetailBack() {
    closeMsgDetail();
    if (s_thread && !g_curPeer.empty()) {
        lv_obj_remove_flag(s_thread, LV_OBJ_FLAG_HIDDEN);
        rebuildThread();
        composeReflectUp();
        deferFocus(s_compose);
    }
}

void showMsgDetail(const std::string& mkey) {
    if (g_id < 0 || g_curPeer.empty()) return;
    refreshFont();
    if (s_msgDetail) closeMsgDetail();
    if (s_thread) lv_obj_add_flag(s_thread, LV_OBJ_FLAG_HIDDEN);

    /* Point-read the record's leaves + the msgmeta join (keyed by message_id). */
    std::string base = g_msgsPrefix + "." + g_curPeer + "." + mkey;
    auto rd = [&](const char* f) { return storageGetStr((base + "." + f).c_str(), ""); };
    bool        in      = rd("dir") == "in";
    int         status  = storageGetInt((base + ".status").c_str(), 0);
    int         tries   = storageGetInt((base + ".tries").c_str(), 0);
    long        ts      = atol(rd("ts").c_str());
    std::string title   = rd("title");
    std::string content = rd("content");
    std::string method  = rd("method");
    bool        readf   = storageGetInt((base + ".read").c_str(), 0) != 0;
    std::string reply   = rd("reply_to");
    std::string mid     = rd("message_id");

    std::string mm = "lxmf.msgmeta." + mid;
    std::string iface = mid.empty() ? "" : storageGetStr((mm + ".iface").c_str(), "");
    int         hops  = mid.empty() ? -1 : storageGetInt((mm + ".hops").c_str(), -1);
    std::string fhop  = mid.empty() ? "" : storageGetStr((mm + ".first_hop").c_str(), "");
    std::string rssi  = mid.empty() ? "" : storageGetStr((mm + ".rssi").c_str(), "");
    std::string snr   = mid.empty() ? "" : storageGetStr((mm + ".snr").c_str(), "");
    bool haveMeta = !iface.empty() || hops >= 0 || !fhop.empty();

    s_msgDetail = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_msgDetail);
    lv_obj_set_size(s_msgDetail, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_msgDetail, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_msgDetail, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_msgDetail, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* hdr = lv_obj_create(s_msgDetail);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { msgDetailBack(); }, LV_EVENT_CLICKED, nullptr);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), back);
    lv_obj_t* htitle = mkLabel(hdr, in ? "Incoming message" : "Outgoing message", lv_color_white());
    lv_obj_align(htitle, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t* body = lv_obj_create(s_msgDetail);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_style_pad_ver(body, 6, 0);
    lv_obj_set_style_pad_row(body, 3, 0);

    detailRow(body, "Status", lxmfStatusName((uint8_t)status));
    if (haveMeta) {
        detailRow(body, "Interface", iface);
        detailRow(body, "Hops", hops >= 0 ? std::to_string(hops) : std::string());
        bool direct = fhop.empty() || fhop.find_first_not_of('0') == std::string::npos;
        detailRow(body, "First hop", direct ? "direct (no transit node)" : groupHash(fhop));
        if (!rssi.empty()) detailRow(body, "RSSI", rssi + " dBm");
        if (!snr.empty())  detailRow(body, "SNR", snr + " dB");
    } else {
        detailRow(body, "Routing", "not recorded (DIRECT/Resource, or pre-dating)");
    }
    if (ts > 0) {
        char tb[64] = "";
        time_t tt = ts; struct tm tmv{}; localtime_r(&tt, &tmv);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tmv);
        detailRow(body, "Time", tb);
    }
    if (!in) {
        detailRow(body, "Method", method.empty() ? "auto" : method);
        detailRow(body, "Attempts", tries == LXMF_TRIES_GAVEUP ? "gave up" : std::to_string(tries));
    } else {
        detailRow(body, "Read", readf ? "yes" : "no");
    }
    detailRow(body, "Message ID", mid.empty() ? "" : groupHash(mid));
    if (!reply.empty() && reply.find_first_not_of('0') != std::string::npos)
        detailRow(body, "In reply to", groupHash(reply));
    if (!title.empty()) detailRow(body, "Title", title);
    detailRow(body, "Content", content);

    deferFocus(back);
}

void onMsgLongPress(lv_event_t* e) {
    auto* key = static_cast<std::string*>(lv_event_get_user_data(e));
    if (key) showMsgDetail(*key);
}
void bindMsgDetail(lv_obj_t* o, const std::string& key) {
    auto* p = new std::string(key);
    lv_obj_add_event_cb(o, onMsgLongPress, LV_EVENT_LONG_PRESSED, p);
    lv_obj_add_event_cb(o, onRowDeletePeer, LV_EVENT_DELETE, p);   /* frees p */
}

/* Contact row circled-i tap: open the info page for that row's peer. */
void onContactInfo(lv_event_t* e) {
    showInfo(*static_cast<std::string*>(lv_event_get_user_data(e)));
}

/* Populate a row host (styled by the caller) with the two-line body + a
 * right-aligned age badge: [ name / one-line subtitle ]········[ age ].
 * `host` is turned into a flex row; the text stacks in a growing left column so
 * a long subtitle ellipsizes rather than shoving the badge off-screen. */
void fillPeerContent(lv_obj_t* host, const std::string& title, const std::string& sub,
                     const std::string& age, lv_color_t titleColor) {
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* col = lv_obj_create(host);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 0);                 /* grow from 0 so the badge keeps its space */
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    /* CRITICAL: a plain lv_obj is CLICKABLE by default, and this column covers
     * nearly the whole row — it was winning the hit-test over the row button, so
     * taps landed here (no handler, no bubbling) and silently died; only the few
     * uncovered padding pixels ever reached the row. Labels aren't clickable, so
     * lists that put labels straight on the button never hit this. Make the
     * column transparent to input so presses fall through to the row. */
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    mkLabel(col, title, titleColor);

    /* LONG_DOT ellipsizes only within the label's own size, so bound both:
     * full column width + exactly one line tall → a long subtitle is clipped to
     * a single "…"-terminated line instead of wrapping. */
    lv_obj_t* pv = mkLabel(col, sub, lv_color_hex(0x8a93a0));
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pv, lv_pct(100));
    lv_obj_set_height(pv, lv_font_get_line_height(kFont));

    if (!age.empty()) {
        lv_obj_t* a = mkLabel(host, age, lv_color_hex(0x6a7280));
        lv_obj_set_style_pad_left(a, 4, 0);
    }
}

/* A plain tappable row (On-the-Mesh + the "message this address" affordance).
 * The peer travels on the widget (bindPeer) → onContactClick → openThread. */
lv_obj_t* buildPeerRow(lv_obj_t* list, const RowSpec& s) {
    lv_obj_t* row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    bindPeer(row, onContactClick, s.peer);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    fillPeerContent(row, s.title, s.sub, s.age, s.titleColor);
    return row;
}

/* ---- contact row: a plain tappable body + a fixed circled-i ----
 * The body is an ordinary button (opens the thread) — structurally identical to
 * the mesh rows, which tap reliably; the earlier swipe-to-delete foreground was
 * the one thing unique to this list and made taps miss, so it's gone. A small
 * circled-i sits at the right edge (no background) and opens the contact info
 * page, which holds the delete-conversation flow behind an explicit confirm. */
lv_obj_t* buildContactRow(lv_obj_t* list, const RowSpec& s) {
    int lineH = lv_font_get_line_height(kFont);

    /* Tappable body — the same plain button the mesh tab uses (peer on the
     * widget via bindPeer → onContactClick → openThread). */
    lv_obj_t* row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    bindPeer(row, onContactClick, s.peer);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    fillPeerContent(row, s.title, s.sub, s.age, s.titleColor);   /* row → flex: [body][age] */

    /* Unread badge: a blue pill with the count, between the age and the info
     * button. Blue background per design; absent when there's nothing unread. */
    if (s.unread > 0) {
        lv_obj_t* badge = lv_obj_create(row);
        lv_obj_remove_style_all(badge);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2563a0), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_set_style_pad_ver(badge, 1, 0);
        lv_obj_set_width(badge, LV_SIZE_CONTENT);
        lv_obj_set_height(badge, LV_SIZE_CONTENT);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        int u = s.unread < 0 ? 0 : (s.unread > 999 ? 999 : s.unread);
        char cnt[8]; snprintf(cnt, sizeof cnt, "%d", u);
        lv_obj_center(mkLabel(badge, cnt, lv_color_white()));
    }

    /* Fixed circled-i at the right edge (grey, no background) → info page.
     * LVGL's symbol set has no info glyph, so it's a bordered-circle label. */
    lv_obj_t* info = lv_button_create(row);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, lineH + 14, 2 * lineH);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ic = mkLabel(info, "i", lv_color_hex(0x8a93a0));
    lv_obj_set_size(ic, lineH + 2, lineH + 2);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(ic, 1, 0);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 1, 0);
    lv_obj_set_style_border_color(ic, lv_color_hex(0x8a93a0), 0);
    lv_obj_center(ic);
    bindPeer(info, onContactInfo, s.peer);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), info);
    return row;
}

lv_obj_t* grpLabel(lv_obj_t* list, const char* t) {
    lv_obj_t* l = mkLabel(list, t, lv_color_hex(0x6a7280));
    lv_obj_set_style_pad_top(l, 3, 0);
    return l;
}

/* Realize one row spec into a widget appended to `list`. */
lv_obj_t* buildRow(lv_obj_t* list, const RowSpec& s) {
    switch (s.kind) {
        case RK_CONTACT: return buildContactRow(list, s);
        case RK_PEER:    return buildPeerRow(list, s);
        case RK_GROUP:   return grpLabel(list, s.title.c_str());
        case RK_EMPTY:   default: return mkLabel(list, s.title, lv_color_hex(0x8a93a0));
    }
}

/* Pack every live row into sequential child slots after the search box (child 0),
 * so a reused-but-reordered row lands at its new position without a rebuild. */
void reorderRows(lv_obj_t* list, std::vector<ListRow>& model) {
    int32_t idx = 1;
    for (auto& r : model) if (r.obj) lv_obj_move_to_index(r.obj, idx++);
}

void chunkTimerCb(lv_timer_t* t) {
    auto* job = static_cast<ChunkJob*>(lv_timer_get_user_data(t));
    int made = 0;
    bool pending = false;
    for (size_t i = 0; i < job->target.size(); i++) {
        if ((*job->model)[i].obj) continue;
        if (made < LIST_CHUNK) { (*job->model)[i].obj = buildRow(job->list, job->target[i]); made++; }
        else { pending = true; break; }
    }
    reorderRows(job->list, *job->model);
    if (pending) return;                         /* more next tick — the loop renders meanwhile */
    if (job->keepScroll && job->scrollY > 0) {
        lv_obj_update_layout(job->list);
        lv_obj_scroll_to_y(job->list, job->scrollY, LV_ANIM_OFF);
    }
    *job->slot = nullptr;
    lv_timer_delete(job->timer);
    delete job;
}

void cancelChunk(ChunkJob*& slot) {
    if (!slot) return;
    if (slot->timer) lv_timer_delete(slot->timer);
    delete slot;
    slot = nullptr;
}

/* Reconcile `model` (live rows) to `target` (desired rows): reuse unchanged keys,
 * recreate changed ones, build new ones, delete vanished ones, then reorder. A big
 * batch of fresh builds (first open / identity switch / cleared search) is created
 * LIST_CHUNK at a time off a timer so the LCD loop renders + services input between
 * chunks; incremental updates (a new message, a stage change) touch only their one
 * row and finish synchronously. */
void applyRows(lv_obj_t* list, std::vector<ListRow>& model,
               std::vector<RowSpec> target, bool keepScroll, ChunkJob*& slot) {
    cancelChunk(slot);   /* supersede any in-flight populate for this list */

    std::unordered_map<std::string, size_t> have;
    for (size_t i = 0; i < model.size(); i++)
        if (model[i].obj) have.emplace(model[i].key, i);   /* only rows that actually exist */
    std::vector<char> used(model.size(), 0);

    std::vector<ListRow> out;
    out.reserve(target.size());
    int creates = 0;
    for (auto& t : target) {
        lv_obj_t* obj = nullptr;
        auto it = have.find(t.key);
        if (it != have.end() && !used[it->second]) {
            used[it->second] = 1;
            ListRow& ex = model[it->second];
            if (ex.sig == t.sig) obj = ex.obj;                /* reuse untouched */
            else { lv_obj_delete(ex.obj); creates++; }         /* changed → recreate */
        } else {
            creates++;                                         /* new key → build */
        }
        out.push_back({ t.key, t.sig, obj });
    }
    for (size_t i = 0; i < model.size(); i++)
        if (!used[i] && model[i].obj) lv_obj_delete(model[i].obj);   /* vanished */

    model.swap(out);

    int32_t sy = keepScroll ? lv_obj_get_scroll_y(list) : 0;

    if (creates <= LIST_CHUNK) {
        for (size_t i = 0; i < model.size(); i++)
            if (!model[i].obj) model[i].obj = buildRow(list, target[i]);
        reorderRows(list, model);
        if (keepScroll && sy > 0) { lv_obj_update_layout(list); lv_obj_scroll_to_y(list, sy, LV_ANIM_OFF); }
        return;
    }

    /* Large populate: build the first chunk now, defer the rest to a repeating
     * timer that the LCD loop drains between renders. */
    int made = 0;
    for (size_t i = 0; i < model.size() && made < LIST_CHUNK; i++)
        if (!model[i].obj) { model[i].obj = buildRow(list, target[i]); made++; }
    reorderRows(list, model);

    slot = new ChunkJob{ list, &model, std::move(target), keepScroll, sy, nullptr, &slot };
    slot->timer = lv_timer_create(chunkTimerCb, 15, slot);
}

/* Decoration rows (New / footer / empty-state) have no peer, so they key off a
 * 0x01-prefixed sentinel — a control byte a 32-hex peer key can never contain. The
 * "\x01." separator keeps the byte out of the following escape (a bare "\x01e…"
 * would fold the 'e' into the hex escape). */
RowSpec groupSpec(const std::string& t) {
    RowSpec s; s.kind = RK_GROUP; s.key = "\x01.g." + t; s.sig = t; s.title = t; return s;
}
RowSpec emptySpec(const std::string& t) {
    RowSpec s; s.kind = RK_EMPTY; s.key = "\x01.e." + t; s.sig = t; s.title = t; return s;
}

/* Build both tabs' target row sets from the in-RAM stores and reconcile each list
 * to it. The reconcile touches only the rows that changed, so the common live
 * update (one new message / one announce) is one widget, not a wholesale rebuild;
 * a full populate is chunked (see applyRows). Each list keeps its scroll. */
void rebuildList(bool keepScroll) {
    if (!s_listC || !s_listM) return;
    refreshFont();
    /* The mesh column reads g_anns; reload the (up to thousands) announce catalogue
     * only when it actually changed or has never been walked — a Contacts-only
     * change (a new message) no longer drags the whole catalogue through a sort.
     * refreshTimerCb clears g_refreshAnns after the rebuild it schedules. */
    if (!g_annsLoaded || g_refreshAnns) { refreshAnnounces(); g_annsLoaded = true; }

    std::vector<RowSpec> tC, tM;

    if (g_id < 0) {
        /* g_id < 0 spans two very different situations. A configured slot exists
         * in persistent storage the instant we open, but its mailbox isn't up
         * until rnsd starts and the delivery dest connects — seconds after a
         * reset. Only say "create one" when there is genuinely nothing to bring
         * up; otherwise reassure that it's coming. */
        std::vector<int> en, dis;
        configuredIds(en, dis);
        const char* msg =
            !en.empty()  ? "Waiting for initialization…"                :
            !dis.empty() ? "Identity disabled.\nEnable it in Settings." :
                           "No active identity.\nCreate one in Settings.";
        tC.push_back(emptySpec(msg));
        applyRows(s_listC, g_rowsC, std::move(tC), keepScroll, g_chunkC);
        applyRows(s_listM, g_rowsM, std::move(tM), keepScroll, g_chunkM);   /* clears the mesh list */
        return;
    }

    long now = nowMonoS();   /* announce stamps are monotonic since-boot, not wall time */

    /* ---- Contacts tab: conversations, newest-comms first, each with a
     * last-heard-announce badge. ---- */
    std::string nC = lower(trim(g_qContacts));
    /* Read the conversation list from the maintained directory — never the
       message store. Only peers with at least one message (count>0) are
       conversations; the rest are announce-only contacts. */
    g_convScan.clear();
    char cprefix[64];
    snprintf(cprefix, sizeof cprefix, "s.lxmf.id.%d.contacts", g_id);
    storageForEach(cprefix, convCb);
    std::vector<Conv> convs;
    for (auto& c : g_convScan) if (c.count > 0) convs.push_back(c);
    std::sort(convs.begin(), convs.end(), [](const Conv& a, const Conv& b) { return a.ts > b.ts; });

    int cRows = 0;

    /* "New": a bare 32-hex query for a peer we don't already have a thread with. */
    if (isHex32(nC)) {
        bool known = false;
        for (auto& c : convs) if (c.peer == nC) { known = true; break; }
        if (!known) {
            tC.push_back(groupSpec("New"));
            RowSpec s; s.kind = RK_PEER; s.key = "\x01.new"; s.peer = nC;
            s.title = "Message this address"; s.sub = nC; s.titleColor = lv_color_white();
            s.sig = "N|" + nC;
            tC.push_back(std::move(s));
            cRows++;
        }
    }

    for (auto& c : convs) {
        std::string nm = peerName(c.peer);
        if (!qmatch(nC, nm, c.peer)) continue;
        long la = lastAnnounce(c.peer);
        RowSpec s; s.kind = RK_CONTACT; s.key = c.peer; s.peer = c.peer;
        s.title = nm; s.sub = printable(c.preview, true);
        s.age = la > 0 ? relAge(now - la) : std::string();
        s.titleColor = lv_color_white(); s.unread = c.unread;
        s.sig = "C|" + nm + "|" + s.sub + "|" + std::to_string(c.unread);   /* age excluded on purpose */
        tC.push_back(std::move(s));
        cRows++;
    }
    if (cRows == 0)
        tC.push_back(emptySpec(nC.empty() ? "No conversations yet.\nSearch the mesh tab." : "No matches."));

    /* ---- On the Mesh: every dest heard within the expiry window, newest first,
     * badged by recency. No dedup with Contacts — a peer can appear in both. ---- */
    std::string nM = lower(trim(g_qMesh));
    int expire = storageGetInt("s.lxmf.on_mesh_expire", 3600);   /* 0 = show all */
    int shown = 0, total = 0;
    for (auto& an : g_anns) {
        if (expire > 0 && an.last > 0 && (now - an.last) > expire) continue;   /* too old */
        std::string nm = an.name.empty() ? peerName(an.hash) : an.name;
        if (!qmatch(nM, nm, an.hash)) continue;
        total++;
        if (shown >= MESH_ROW_CAP) continue;
        RowSpec s; s.kind = RK_PEER; s.key = an.hash; s.peer = an.hash;
        s.title = nm; s.sub = an.hash;
        s.age = an.last > 0 ? relAge(now - an.last) : std::string();
        s.titleColor = lv_color_white();
        s.sig = "P|" + nm;                          /* age/last excluded: a re-announce reorders, no churn */
        tM.push_back(std::move(s));
        shown++;
    }
    if (total > shown) {
        char foot[48];
        snprintf(foot, sizeof foot, "+%d more — search to narrow", total - shown);
        tM.push_back(groupSpec(foot));
    }
    if (shown == 0)
        tM.push_back(emptySpec(nM.empty() ? "Nothing heard recently." : "No matches."));

    applyRows(s_listC, g_rowsC, std::move(tC), keepScroll, g_chunkC);
    applyRows(s_listM, g_rowsM, std::move(tM), keepScroll, g_chunkM);
}

/* ---- tabs ---- */

void styleTab(lv_obj_t* b, bool active) {
    if (!b) return;
    lv_obj_set_style_bg_color(b, active ? lv_color_hex(0x2563a0) : lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
}

void setActiveTab(int n, bool keepScroll) {
    g_activeTab = n;
    bool c = (n == 0);
    if (s_tabContacts) { if (c) lv_obj_remove_flag(s_tabContacts, LV_OBJ_FLAG_HIDDEN);
                         else    lv_obj_add_flag  (s_tabContacts, LV_OBJ_FLAG_HIDDEN); }
    if (s_tabMesh)     { if (c) lv_obj_add_flag   (s_tabMesh,     LV_OBJ_FLAG_HIDDEN);
                         else    lv_obj_remove_flag(s_tabMesh,     LV_OBJ_FLAG_HIDDEN); }
    styleTab(s_tabBtnC, c);
    styleTab(s_tabBtnM, !c);
    lv_obj_t* shown  = c ? s_listC   : s_listM;
    lv_obj_t* search = c ? s_searchC : s_searchM;
    /* Switching tabs / fresh open starts at the top, so focusing the search box
     * there is harmless. Returning from a conversation (keepScroll) must stay put:
     * pin the scroll across the focus so the search box doesn't drag it up. */
    if (!keepScroll) {
        if (shown) lv_obj_scroll_to_y(shown, 0, LV_ANIM_OFF);
        deferFocus(search);                          /* type-to-search on the shown tab */
    } else {
        deferFocusPinScroll(search, shown);          /* keyboard ready, list stays put */
    }
    /* Announce churn is ignored while the mesh column is hidden (see
       onStorageChange). If any arrived meanwhile and it's now the visible tab,
       fold them in with one coalesced rebuild. */
    if (!c && g_refreshAnns) { g_listDirty = true; scheduleRefresh(); }
}

void onTabClick(lv_event_t* e) {
    setActiveTab((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- search boxes (one per tab) ---- */

void searchTimerCb(lv_timer_t*) {
    g_searchTimer = nullptr;   /* one-shot: LVGL deletes it after this returns */
    rebuildList();
}

void onSearchChanged(lv_event_t* e) {
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* t = lv_textarea_get_text(ta);
    (ta == s_searchM ? g_qMesh : g_qContacts) = t ? t : "";
    /* Postpone the rebuild to SEARCH_DEBOUNCE_MS after the last keystroke so a
     * fast typist isn't fighting a per-key rebuild (see SEARCH_DEBOUNCE_MS). */
    if (g_searchTimer) {
        lv_timer_reset(g_searchTimer);
    } else {
        g_searchTimer = lv_timer_create(searchTimerCb, SEARCH_DEBOUNCE_MS, nullptr);
        lv_timer_set_repeat_count(g_searchTimer, 1);
    }
}

void onSearchEnter(lv_event_t* e) {
    /* Enter flushes any pending debounce, then opens the thread if the query is
     * a bare 32-hex address (start a conversation with an exact dest). */
    if (g_searchTimer) {
        lv_timer_delete(g_searchTimer);
        g_searchTimer = nullptr;
        rebuildList();
    }
    lv_obj_t* ta = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    std::string needle = lower(trim(ta == s_searchM ? g_qMesh : g_qContacts));
    if (isHex32(needle)) openThread(needle);
}

/* One tab page: a single vertical scroll list whose first child is the search
 * box, so the search scrolls away with the rows instead of staying pinned. The
 * page and the list are the same object; rebuildList clears only the rows
 * (children >= 1) and leaves the search — and its text/focus — in place. */
lv_obj_t* buildTabPage(lv_obj_t*& page, lv_obj_t*& search, const char* placeholder) {
    page = lv_obj_create(s_contacts);           /* scrollable (default) — this IS the list */
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 2, 0);

    search = lv_textarea_create(page);          /* child 0 — scrolls with the rows */
    lv_textarea_set_one_line(search, true);
    lv_textarea_set_placeholder_text(search, placeholder);
    lv_obj_set_style_text_font(search, kFont, 0);
    lv_obj_set_style_pad_ver(search, 2, 0);
    lv_obj_set_style_pad_hor(search, 6, 0);
    lv_obj_set_width(search, lv_pct(100));
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), search);
    lv_obj_add_event_cb(search, onSearchChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(search, onSearchEnter,   LV_EVENT_READY,         nullptr);

    return page;   /* rows are appended after `search` */
}

lv_obj_t* mkTabButton(lv_obj_t* bar, const char* text, int idx) {
    lv_obj_t* b = lv_button_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_ver(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* l = mkLabel(b, text, lv_color_white());
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onTabClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

/* Record USER scrolls on the mesh list so the live announce-refresh backs off
 * while you're browsing (see refreshTimerCb's MESH_SCROLL_QUIET_MS gate). A user
 * drag is dispatched while an input device is being processed; the rebuild's own
 * scroll-restore runs from a timer with no active indev, so it's ignored here —
 * otherwise every rebuild would re-arm the quiet window and starve itself. */
void onMeshScroll(lv_event_t*) {
    if (lv_indev_active()) g_lastMeshScroll = millis();
}

void buildContactsScreen() {
    s_contacts = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_contacts);
    lv_obj_set_size(s_contacts, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_contacts, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_contacts, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_contacts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_contacts, 4, 0);
    lv_obj_set_style_pad_ver(s_contacts, 1, 0);
    lv_obj_set_style_pad_row(s_contacts, 2, 0);

    /* Tab bar: two equal buttons. */
    lv_obj_t* bar = lv_obj_create(s_contacts);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_tabBtnC = mkTabButton(bar, "Contacts",    0);
    s_tabBtnM = mkTabButton(bar, "On the Mesh", 1);

    s_listC = buildTabPage(s_tabContacts, s_searchC, "Search contacts or 32-hex");
    s_listM = buildTabPage(s_tabMesh,     s_searchM, "Search the mesh");
    lv_obj_add_event_cb(s_listM, onMeshScroll, LV_EVENT_SCROLL, nullptr);   /* throttle gate */

    setActiveTab(g_activeTab);   /* Contacts by default; also styles the tab bar */
}

/* ---- identity picker ---- */

void onIdPick(lv_event_t* e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    selectId(n);
    showContacts();
    maybeOpenPending();       /* resume a nomad-tapped open under the chosen identity */
}

void showIdPicker(const std::vector<int>& ids) {
    if (!s_idpick) {
        s_idpick = lv_obj_create(s_layer);
        lv_obj_remove_style_all(s_idpick);
        lv_obj_set_size(s_idpick, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_idpick, lv_color_hex(0x10141a), 0);
        lv_obj_set_style_bg_opa(s_idpick, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(s_idpick, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_hor(s_idpick, 6, 0);
        lv_obj_set_style_pad_ver(s_idpick, 4, 0);
        lv_obj_set_style_pad_row(s_idpick, 4, 0);
    }
    lv_obj_clean(s_idpick);
    if (s_contacts) lv_obj_add_flag(s_contacts, LV_OBJ_FLAG_HIDDEN);
    if (s_thread)   lv_obj_add_flag(s_thread,   LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(false);

    mkLabel(s_idpick, "Select identity", lv_color_hex(0x8a93a0));

    lv_obj_t* first = nullptr;
    for (int n : ids) {
        lv_obj_t* b = lv_button_create(s_idpick);
        lv_obj_remove_style_all(b);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x20262e), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(b, 1, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, onIdPick, LV_EVENT_CLICKED, (void*)(intptr_t)n);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
        if (!first) first = b;

        mkLabel(b, idLabel(n), lv_color_white());
        char dk[40];
        snprintf(dk, sizeof dk, "lxmf.id.%d.dest_hash", n);
        std::string dh = storageGetStr(dk, "");
        mkLabel(b, dh.empty() ? "(announcing…)" : (dh.substr(0, 16) + "…"), lv_color_hex(0x8a93a0));
    }
    lv_obj_remove_flag(s_idpick, LV_OBJ_FLAG_HIDDEN);
    deferFocus(first);
}

/* Route to the right first screen for the current identity set: picker (>1),
 * straight into the list (exactly 1), or the list's guidance (none). Routes on
 * *loaded* identities (dest hash known), not connected ones, so stored history
 * shows the moment the identity loads — seconds before its mailbox comes up.
 * Called on open and whenever the identity set changes while still unpicked. */
void routeByIdentity() {
    std::vector<int> ids;
    loadedIds(ids);
    if (ids.size() > 1) {
        showIdPicker(ids);
    } else if (ids.size() == 1) {
        selectId(ids[0]);
        showContacts();
    } else {
        g_id = -1;
        g_msgsPrefix.clear();
        g_msgs.clear();
        showContacts();
    }
}

/* ---- live updates: storage change -> refresh whatever's shown ---- */

/* Coalesce list rebuilds: the on-the-mesh announce stream can fire many times a
 * second, and each rebuild walks the announce catalogue + clears the list (which
 * resets the scroll). Schedule at most one rebuild per window; rebuildList keeps
 * the scroll position across it so a live update never snaps you to the top. */
/* Coalesced view refresh. A single message's queued→sending→sent→delivered
 * walk, a markRead sweep, or the announce firehose each fires many change
 * notifications in a burst; running a full refreshMsgs (O(msgs²)) + rebuild
 * INLINE on every one — as the thread view used to, uncoalesced — melts the
 * LCD task. Fold the burst into one debounced pass that runs only the walks
 * the pending changes actually dirtied. (The optimistic send bubble is drawn
 * directly in the send handler, so this delay never affects your own send.) */
void refreshTimerCb(lv_timer_t*) {
    g_refreshPending = false;
    if (g_id < 0) { g_refreshMsgs = g_refreshAnns = g_listDirty = false; return; }
    /* Flags are cleared by the branch that CONSUMES them, not up front: a change
     * that arrives while a conversation is open updates the thread (clearing
     * g_refreshMsgs) but must keep g_refreshAnns / g_listDirty so the list still
     * rebuilds when you return to it. */
    if (s_thread && !lv_obj_has_flag(s_thread, LV_OBJ_FLAG_HIDDEN)) {
        if (g_refreshMsgs) { refreshMsgs(); rebuildThread(); g_refreshMsgs = false; }
        composeReflectUp();        /* enable/label compose the instant `up` flips */
        applyThreadIcons();        /* follow link open/close/teardown; re-source icons after a zoom reset */
    } else if (s_contacts && !lv_obj_has_flag(s_contacts, LV_OBJ_FLAG_HIDDEN)) {
        /* Only rebuild when something actually changed — a plain return from a
         * conversation must not churn (or move the scroll). The list reads the
         * contacts directory + g_anns; it never needs the g_msgs walk. */
        if (g_listDirty) {
            /* While the On-the-Mesh column is the visible tab, its data is the
               announce firehose. Throttle its live rebuild: at most once per
               MESH_MIN_REBUILD_MS, and not at all while the list has been scrolled
               within the last MESH_SCROLL_QUIET_MS. If gated, re-arm for the
               remaining quiet time and keep the dirty flags so the update still
               lands once things settle. The Contacts tab isn't announce-driven
               (see onStorageChange), so its changes stay on the fast 200 ms path. */
            bool meshVisible = s_tabMesh && !lv_obj_has_flag(s_tabMesh, LV_OBJ_FLAG_HIDDEN);
            if (meshVisible) {
                uint32_t now = millis();
                uint32_t wait = 0;
                if (now - g_lastMeshBuild  < MESH_MIN_REBUILD_MS)
                    wait = MESH_MIN_REBUILD_MS  - (now - g_lastMeshBuild);
                if (now - g_lastMeshScroll < MESH_SCROLL_QUIET_MS)
                    wait = std::max(wait, MESH_SCROLL_QUIET_MS - (now - g_lastMeshScroll));
                if (wait > 0) { scheduleRefreshIn(wait); return; }
                g_lastMeshBuild = now;
            }
            rebuildList(true);   /* refreshes g_anns (mesh column) itself */
            g_refreshMsgs = g_refreshAnns = g_listDirty = false;
            g_listBuiltId = g_id;
        }
    }
}
void scheduleRefreshIn(uint32_t ms) {
    if (g_refreshPending) return;
    g_refreshPending = true;
    lv_timer_t* t = lv_timer_create(refreshTimerCb, ms, nullptr);
    lv_timer_set_repeat_count(t, 1);
}
void scheduleRefresh() { scheduleRefreshIn(200); }

void onStorageChange(const char* key, const char*) {
    if (!s_layer) return;
    /* Not yet committed to a slot: re-route only when the identity set itself
     * may have changed (ignore the announce firehose so the picker doesn't
     * thrash / steal focus on every heard announce). */
    if (g_id < 0) {
        if (key && (strncmp(key, "lxmf.id", 7) == 0 || strncmp(key, "s.lxmf.id", 9) == 0))
            routeByIdentity();
        return;
    }
    /* Flag which walk the change dirtied and coalesce; refreshTimerCb does the
       expensive work once per window. Announce churn touches only the mesh
       column, never the msg walk or the open thread. */
    if (key && strncmp(key, "lxmf.msgmeta", 12) == 0) {
        /* Routing telemetry for the open thread (the LoRa pill's iface). Refresh
           the msg walk so rebuildThread re-renders pills; the contact list is
           untouched by msgmeta, so don't dirty it. */
        g_refreshMsgs = true;
    } else if (key && strncmp(key, "lxmf.announces", 14) == 0) {
        g_refreshAnns = true;
        /* Announce-only change: it feeds the On-the-Mesh column and nothing else.
           A rebuild walks + sorts the whole (up to thousands) announce catalogue
           and re-creates every row; on a busy mesh announces fire several times a
           second, so dirtying the list on each one pegs the lcd task while you're
           just sitting on the Contacts tab — whose rows come from the contacts
           directory, not announces. Dirty the list only while the mesh column is
           the visible tab; setActiveTab folds accumulated announces back in when
           you switch to it. */
        if (s_contacts && !lv_obj_has_flag(s_contacts, LV_OBJ_FLAG_HIDDEN)
            && s_tabMesh && !lv_obj_has_flag(s_tabMesh, LV_OBJ_FLAG_HIDDEN))
            g_listDirty = true;
    } else {
        g_refreshMsgs = true;
        g_listDirty = true;   /* a contact row (preview/unread) may have changed */
    }
    scheduleRefresh();
}

/* Screen woke: if the open thread is scrolled to the newest, the messages that
 * arrived while asleep are now in view — clear their unread. */
void onStandbyChange(const char* /*key*/, const char* val) {
    if (!s_layer) return;
    if (val && atoi(val) == 0 && threadReading()) markRead(g_curPeer);
}

/* Layer eviction frees every widget; null our handles so a storage change
 * arriving after close (the subscription outlives the layer) early-returns
 * instead of touching freed objects. The next open rebuilds from scratch. */
void onLayerDelete(lv_event_t*) {
    /* App-layer objects only — s_newIdTa / s_importTa live in the Settings
     * program's layer and null themselves via their own DELETE handlers. */
    s_layer = nullptr; s_idpick = nullptr; s_contacts = nullptr;
    s_tabBtnC = nullptr; s_tabBtnM = nullptr; s_tabContacts = nullptr; s_tabMesh = nullptr;
    s_searchC = nullptr; s_searchM = nullptr; s_listC = nullptr; s_listM = nullptr;
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr;
    s_earlierBtn = nullptr; s_newerBtn = nullptr; s_newerLbl = nullptr; s_loading = nullptr;
    s_stickyDate = nullptr; s_stickyLbl = nullptr;
    s_histWrap = nullptr; s_histList = nullptr; s_histBubbles = nullptr;
    s_histEarlier = nullptr; s_histNewer = nullptr; s_histNewerLbl = nullptr;
    g_histDateSeps.clear();
    s_send = nullptr; s_sendIcon = nullptr; s_rc = nullptr; s_comp = nullptr; s_composePill = nullptr; s_collapsePill = nullptr;
    g_bubbles.clear();   /* bubble widgets went with the layer — drop dangling refs */
    g_dateSeps.clear();  /* separator widgets went with the layer too */
    /* Pending timers would touch freed widgets — drop them. */
    if (g_threadLoadTimer) { lv_timer_delete(g_threadLoadTimer); g_threadLoadTimer = nullptr; }
    if (g_stickyFadeTimer) { lv_timer_delete(g_stickyFadeTimer); g_stickyFadeTimer = nullptr; }
    if (g_iconSettle)      { lv_timer_delete(g_iconSettle);      g_iconSettle      = nullptr; }
    g_needMsgLoad = false;
    /* A pending chunked populate would build rows into freed widgets — drop it,
     * then forget the list-row models (their widgets went with the layer). */
    cancelChunk(g_chunkC); cancelChunk(g_chunkM);
    g_rowsC.clear(); g_rowsM.clear();
    s_compose = nullptr; s_threadName = nullptr; s_threadDown = nullptr; s_threadLink = nullptr;
    s_info = nullptr; s_msgDetail = nullptr; s_confirm = nullptr; g_infoPeer.clear();
    g_focusTarget = nullptr;
    g_refreshPending = false; g_refreshMsgs = false; g_refreshAnns = false;
    /* A queued search rebuild would touch freed widgets — drop it. */
    if (g_searchTimer) { lv_timer_delete(g_searchTimer); g_searchTimer = nullptr; }
}

/* ---- entry point (lcd task, on first open of a fresh layer) ---- */

void lxmfApp(void* arg) {
    refreshFont();   /* vector UI font live before any label / printable() */
    s_layer = static_cast<lv_obj_t*>(arg);
    s_idpick = nullptr; s_contacts = nullptr;
    s_tabBtnC = nullptr; s_tabBtnM = nullptr; s_tabContacts = nullptr; s_tabMesh = nullptr;
    s_searchC = nullptr; s_searchM = nullptr; s_listC = nullptr; s_listM = nullptr;
    s_thread = nullptr; s_msgList = nullptr; s_bubbles = nullptr; s_compose = nullptr;
    s_earlierBtn = nullptr; s_newerBtn = nullptr; s_newerLbl = nullptr; s_loading = nullptr;
    s_stickyDate = nullptr; s_stickyLbl = nullptr;
    s_histWrap = nullptr; s_histList = nullptr; s_histBubbles = nullptr;
    s_histEarlier = nullptr; s_histNewer = nullptr; s_histNewerLbl = nullptr;
    s_send = nullptr; s_sendIcon = nullptr; s_rc = nullptr; s_comp = nullptr; s_composePill = nullptr; s_collapsePill = nullptr;
    g_bubbles.clear(); g_dateSeps.clear(); g_histDateSeps.clear();
    g_threadLoadTimer = nullptr; g_stickyFadeTimer = nullptr; g_iconSettle = nullptr;   /* prior onLayerDelete freed them */
    g_needMsgLoad = false;
    g_winLo = g_winHi = 0; g_atNewest = true; g_anchorMid.clear();
    s_threadName = nullptr; s_threadDown = nullptr; s_threadLink = nullptr;
    s_info = nullptr; s_msgDetail = nullptr; s_confirm = nullptr; g_infoPeer.clear(); g_infoFromThread = false;
    g_curPeer.clear(); g_qContacts.clear(); g_qMesh.clear();
    g_activeTab = 0;   /* Contacts tab selected by default on each fresh open */
    g_id = -1; g_msgsPrefix.clear(); g_msgs.clear();
    g_refreshPending = false; g_refreshMsgs = false; g_refreshAnns = false;
    g_rowsC.clear(); g_rowsM.clear();          /* fresh empty layer → empty row models */
    g_chunkC = nullptr; g_chunkM = nullptr;    /* prior layer's onLayerDelete already freed them */
    g_annsLoaded = false;                      /* reload the announce catalogue on first build */
    g_searchTimer = nullptr;   /* prior layer's onLayerDelete already freed it */
    /* The list is drawn into a brand-new (empty) layer on every open, so the
     * "already built for this identity" short-circuit must start unset — else a
     * reopen after the app is stopped/evicted sees g_listBuiltId still matching
     * the identity, skips the initial populate, and shows an empty list. */
    g_listBuiltId = -2; g_listDirty = false;

    lv_obj_add_event_cb(s_layer, onLayerDelete, LV_EVENT_DELETE, nullptr);

    buildContactsScreen();      /* built once; hidden/shown by the router */
    routeByIdentity();          /* picker (>1) else straight into the list */

    if (!g_subscribed) {
        storageSubscribeChanges("s.lxmf.id",      onStorageChange);   /* msgs + contacts */
        storageSubscribeChanges("lxmf.id",        onStorageChange);   /* identity up/dest edge */
        storageSubscribeChanges("lxmf.announces", onStorageChange);   /* on-the-mesh column */
        storageSubscribeChanges("lxmf.msgmeta",   onStorageChange);   /* per-message routing (LoRa pill) */
        storageSubscribeChanges("sys.standby",    onStandbyChange);   /* wake → clear unread if reading */
        g_subscribed = true;
    }
}

/* ---- Settings → Reticulum → LXMF (admin pane, mirrors the web LxmfPanel) ---- */

void lxmfSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "LXMF");
    lcdSettingSlider (p, "Re-announce interval (s)", "s.lxmf.announce_interval_s", 0, 21600);
    lcdSettingCaption(p, "0 = announce on demand only.");
    lcdSettingSlider (p, "Announce catalogue cap", "s.lxmf.max_announces", 256, 8192);
    lcdSettingSlider (p, "On-the-mesh horizon (s)", "s.lxmf.on_mesh_expire", 0, 86400);
    lcdSettingCaption(p, "Hide mesh peers unheard for longer; 0 = show all.");
    lcdSettingSlider (p, "Advertised stamp cost", "s.lxmf.stamp_cost", 0, 18);
    lcdSettingCaption(p, "PoW cost (bits) senders pay; 0 = none.");
    lcdSettingSwitch (p, "Generate stamps", "s.lxmf.generate_stamps");
    lcdSettingCaption(p, "Pay a peer's advertised PoW cost when sending.");
    lcdSettingSwitch (p, "Require stamps", "s.lxmf.enforce_stamps");
    lcdSettingCaption(p, "Drop inbound messages without a valid stamp.");

    lcdSettingSection(p, "Identities");
    /* One block per existing slot; literal dest/enabled keys (the helpers keep
     * the key by pointer), the slot index drives name/status/Destroy. */
    if (storageExists("s.lxmf.id.0.label")) lxmfIdentityBlock(p, 0, "lxmf.id.0.dest_hash", "s.lxmf.id.0.enabled");
    if (storageExists("s.lxmf.id.1.label")) lxmfIdentityBlock(p, 1, "lxmf.id.1.dest_hash", "s.lxmf.id.1.enabled");
    if (storageExists("s.lxmf.id.2.label")) lxmfIdentityBlock(p, 2, "lxmf.id.2.dest_hash", "s.lxmf.id.2.enabled");
    if (storageExists("s.lxmf.id.3.label")) lxmfIdentityBlock(p, 3, "lxmf.id.3.dest_hash", "s.lxmf.id.3.enabled");

    lcdSettingSection(p, "Add identity");
    if (lcdHasKeyboard()) {
        /* Hardware keyboard: a name field + an explicit Create button. Enter in
         * the field commits too, but pressing it is non-obvious, so the button
         * is the discoverable affordance. */
        lv_obj_t* row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        s_newIdTa = lv_textarea_create(row);
        lv_textarea_set_one_line(s_newIdTa, true);
        lv_textarea_set_placeholder_text(s_newIdTa, "Name");
        lv_obj_set_style_text_font(s_newIdTa, kFont, 0);
        lv_obj_set_flex_grow(s_newIdTa, 1);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_newIdTa);
        lv_obj_add_event_cb(s_newIdTa, onAddIdentity, LV_EVENT_READY, nullptr);   /* Enter commits */
        lv_obj_add_event_cb(s_newIdTa, [](lv_event_t*){ s_newIdTa = nullptr; },   /* avoid a dangle on rebuild */
                            LV_EVENT_DELETE, nullptr);

        lv_obj_t* add = lv_button_create(row);
        lv_obj_set_style_pad_ver(add, 2, 0);
        lv_obj_set_style_pad_hor(add, 8, 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), add);
        lv_obj_t* al = lv_label_create(add);
        lv_obj_set_style_text_font(al, kFont, 0);
        lv_label_set_text(al, "Create");
        lv_obj_center(al);
        lv_obj_add_event_cb(add, onAddIdentity, LV_EVENT_CLICKED, nullptr);

        /* Second row: a 128-hex private key field + an Import button. */
        lv_obj_t* irow = lv_obj_create(p);
        lv_obj_remove_style_all(irow);
        lv_obj_set_width(irow, lv_pct(100));
        lv_obj_set_height(irow, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(irow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(irow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(irow, 6, 0);
        lv_obj_remove_flag(irow, LV_OBJ_FLAG_SCROLLABLE);

        s_importTa = lv_textarea_create(irow);
        lv_textarea_set_one_line(s_importTa, true);
        lv_textarea_set_placeholder_text(s_importTa, "128-hex private key");
        lv_obj_set_style_text_font(s_importTa, kFont, 0);
        lv_obj_set_flex_grow(s_importTa, 1);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_importTa);
        lv_obj_add_event_cb(s_importTa, onImportIdentity, LV_EVENT_READY, nullptr);   /* Enter commits */
        lv_obj_add_event_cb(s_importTa, [](lv_event_t*){ s_importTa = nullptr; },     /* avoid a dangle on rebuild */
                            LV_EVENT_DELETE, nullptr);

        lv_obj_t* imp = lv_button_create(irow);
        lv_obj_set_style_pad_ver(imp, 2, 0);
        lv_obj_set_style_pad_hor(imp, 8, 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), imp);
        lv_obj_t* il = lv_label_create(imp);
        lv_obj_set_style_text_font(il, kFont, 0);
        lv_label_set_text(il, "Import");
        lv_obj_center(il);
        lv_obj_add_event_cb(imp, onImportIdentity, LV_EVENT_CLICKED, nullptr);
    } else {
        /* Touch-only: tapping a field opens the full-screen on-screen keyboard,
         * which carries its own OK (commit) button. */
        lcdSettingText(p, "New (name)",           "lxmf.cmd.identity_new");
        lcdSettingText(p, "Import (128-hex key)", "lxmf.cmd.identity_import");
    }
}

}  // namespace

/* LxmfApp — the LXMessenger launcher program as an LcdApp (and thus a Service).
 * onCreate builds the three screens; cleanup on eviction is handled by the
 * layer's own onLayerDelete (it nulls every handle so a late storage change
 * early-returns), so no onClose is needed. Declared in lxmf_app.h (global, so
 * the generated services: trampoline can `new` it); defined here where the
 * file-static UI state lives. */
LxmfApp::LxmfApp() : LcdApp({ .name = "LXMF", .iconBasename = "lxmf" }) {}

void LxmfApp::onCreate(lv_obj_t* root) { lxmfApp(root); }

/* LxmfApp::appInit — the boot-task half of bring-up, run once by LcdApp::onInit()
 * right after it hops the launcher-tile install onto the lcd task. This whole
 * file lives under conditional/spangap-lcd/, compiled only when the lcd straddle
 * is staged, so no #if is needed — no lcd, no LxmfApp, no services: registration. */
void LxmfApp::appInit() {
    lcdRegisterSettings("Mesh Network/LXMF", "LXMF Messages", lxmfSettingsPane, 2);

    /* The on-device nomad browser writes a tapped contact's dest hash to
     * lxmf.url_lcd. Subscribe ON the lcd task (via lcdRun) so the callback is
     * delivered there and may touch LVGL directly — and register here at init,
     * not in lxmfApp, so the trigger works even if LXMF was never opened. */
    lcdRun([](void*) { storageSubscribeChanges("lxmf.url_lcd", onLcdOpenUrl); });
}
