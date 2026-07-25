#!/usr/bin/env python3
"""
sgdb_recover — inspect and recover spangap storage_db (.db.gz) record files.

storage_db files are gzip-wrapped packed-record stores (see
spangap-core/esp-idf/include/storage_db.h). This tool decodes them off-device so
a conversation or contact directory written by an older/dev firmware can be read
and, if the live loader would reject it, rewritten into the current on-disk
layout to push back.

On-disk image (uncompressed, little-endian):

    [16 B file header] 'SGDB' | u16 fmt | u16 schema_id | u16 schema_ver |
                       u16 hdr_size | u32 record_count
    if fmt>=2: [u16 desc_len][descriptor]        (self-describing field table)
    records:   [u32 rec_len][u8 flags][.. fixed header, hdr_size bytes total ..]
               [u16 klen][key][ per text field: u16 len + bytes ]

    descriptor: u16 field_count, then per field:
                u8 kind | u16 off | u16 width | u8 name_len | name

    kinds: 1=u8 2=u32 3=fixstr(width) 4=text 5=data(width raw bytes)

format_ver 1 files carry NO descriptor: the only layout signal is
(schema_id, hdr_size). Text fields, however, sit after the fixed header + key
regardless of how the fixed fields are arranged, so title/content and contact
names are recoverable from the header's hdr_size + the field-count for that
schema family alone — which is what makes a dev-format file readable here even
when the exact fixed layout is unknown.

Usage:
    sgdb_recover.py dump   FILE...            # decode + print every record
    sgdb_recover.py header FILE...            # just the file header line
    sgdb_recover.py recover FILE OUT.db.gz    # rewrite into the current layout

`recover` targets the current committed schemas (msgs id=1 hdr=140,
contacts id=2 hdr=143). It preserves every text field verbatim and copies the
fixed fields it can positively locate; fields absent from the source default to
zero, exactly as the on-device auto-migrator would fill them.
"""
import gzip
import struct
import sys

U8, U32, FIXSTR, TEXT, DATA = 1, 2, 3, 4, 5
KIND_NAME = {U8: "u8", U32: "u32", FIXSTR: "fixstr", TEXT: "text", DATA: "data"}
BUILTIN = 8


# ---- schema builder (mirrors sdb_schema in storage_db.cpp) -------------------

class Schema:
    def __init__(self, schema_id, schema_ver):
        self.schema_id = schema_id
        self.schema_ver = schema_ver
        self.hdr = BUILTIN
        self.fields = []          # (name, kind, off, width)

    def u8(self, n):     self.fields.append((n, U8, self.hdr, 1)); self.hdr += 1; return self
    def u32(self, n):
        if self.hdr & 3: self.hdr = (self.hdr + 3) & ~3
        self.fields.append((n, U32, self.hdr, 4)); self.hdr += 4; return self
    def fixstr(self, n, w): self.fields.append((n, FIXSTR, self.hdr, w)); self.hdr += w; return self
    def data(self, n, w):   self.fields.append((n, DATA, self.hdr, w)); self.hdr += w; return self
    def text(self, n):      self.fields.append((n, TEXT, len(self.text_fields()), 0)); return self

    def text_fields(self):  return [f for f in self.fields if f[1] == TEXT]
    def find(self, name):   return next((f for f in self.fields if f[0] == name), None)


def msg_current():
    s = Schema(1, 4)
    (s.u8("tries").u8("status").u32("recv_ts").fixstr("dir", 4).fixstr("method", 16)
      .u32("ts").u32("delivered_ts").data("message_id", 32).data("reply_to", 32)
      .data("rlpg_tid", 32).text("title").text("content"))
    return s


def contact_current():
    s = Schema(2, 2)
    (s.u32("count").u32("last_ts").u32("unread").u32("read_ts").u32("last_seen")
      .u8("trust").data("hash", 16).data("pubkey", 64).data("rlpg", 16)
      .data("rlpg_svc", 16).u8("rlpg_active").u8("caps")
      .text("display_name").text("nick").text("preview"))
    return s


# Every id=2 layout from v2a (hdr 109) onward shares this exact fixed prefix — the
# RLPG fields were appended — so these offsets decode any contacts file hdr>=109.
CONTACT_PREFIX = contact_current()
# id=1: delivered_ts was inserted before message_id at hdr 140, so only the
# hdr-140 layout matches the current schema's fixed offsets; older msg files
# (hdr 104) share the prefix up to `ts`.
MSG_CURRENT = msg_current()


# ---- decode ------------------------------------------------------------------

def read_image(path):
    with open(path, "rb") as f:
        raw = f.read()
    try:
        return gzip.decompress(raw)
    except OSError as e:
        sys.exit(f"{path}: not a valid gzip file ({e})")


def parse_header(img, path):
    if len(img) < 16 or img[:4] != b"SGDB":
        sys.exit(f"{path}: not an SGDB store (bad magic)")
    fmt, sid, sver, hdr = struct.unpack_from("<HHHH", img, 4)
    count = struct.unpack_from("<I", img, 12)[0]
    return fmt, sid, sver, hdr, count


def parse_descriptor(img):
    off = 16
    dlen = struct.unpack_from("<H", img, off)[0]; off += 2
    end = off + dlen
    nf = struct.unpack_from("<H", img, off)[0]; off += 2
    fields = []
    for _ in range(nf):
        kind = img[off]; o, w = struct.unpack_from("<HH", img, off + 1); nl = img[off + 5]
        off += 6
        name = img[off:off + nl].decode("utf-8", "replace"); off += nl
        fields.append((name, kind, o, w))
    return fields, end


def text_count_for(sid, hdr, descriptor_fields):
    if descriptor_fields is not None:
        return sum(1 for f in descriptor_fields if f[1] == TEXT), \
               [f[0] for f in descriptor_fields if f[1] == TEXT]
    # format_ver 1: infer from the schema family (schema_id + hdr_size).
    if sid == 1:                       # messages
        if hdr in (40, 96):            # v2/v3: title, content, thread, message_id
            return 4, ["title", "content", "thread", "message_id"]
        return 2, ["title", "content"]           # v4 family
    if sid == 2:                       # contacts
        if hdr == 29:                  # v1: hash as text
            return 4, ["hash", "display_name", "nick", "preview"]
        return 3, ["display_name", "nick", "preview"]
    return 0, []


def decode_fixed(img, rec_off, schema, file_hdr):
    """Best-effort: decode named fixed fields whose offsets are known to match.
    Skips any field that would read past the file's actual fixed header — a
    prefix-schema can be wider than an older file's hdr_size."""
    out = {}
    for name, kind, o, w in schema.fields:
        if kind == TEXT:
            continue
        if o + (w or 4) > file_hdr:
            continue
        p = rec_off + o
        if kind == U8:
            out[name] = img[p]
        elif kind == U32:
            out[name] = struct.unpack_from("<I", img, p)[0]
        elif kind == FIXSTR:
            out[name] = img[p:p + w].split(b"\x00", 1)[0].decode("utf-8", "replace")
        elif kind == DATA:
            b = img[p:p + w]
            out[name] = "" if b == b"\x00" * w else b.hex()
    return out


def walk_records(img, hdr, rec_start, ntext):
    off = rec_start
    n = len(img)
    while off < n:
        if off + hdr > n:
            break
        rlen = struct.unpack_from("<I", img, off)[0]
        if rlen < hdr or off + rlen > n:
            break
        flags = img[off + 4]
        tp = off + hdr
        klen = struct.unpack_from("<H", img, tp)[0]; tp += 2
        key = img[tp:tp + klen]; tp += klen
        texts = []
        for _ in range(ntext):
            if tp + 2 > off + rlen:
                texts.append(b""); continue
            tl = struct.unpack_from("<H", img, tp)[0]; tp += 2
            texts.append(img[tp:tp + tl]); tp += tl
        yield off, rlen, flags, key, texts
        off += rlen


def cmd_header(paths):
    for p in paths:
        img = read_image(p)
        fmt, sid, sver, hdr, count = parse_header(img, p)
        store = {1: "msgs", 2: "contacts", 3: "announces"}.get(sid, "?")
        print(f"{p}: fmt={fmt} schema_id={sid}({store}) ver={sver} hdr={hdr} records={count}")


def cmd_dump(paths):
    for p in paths:
        img = read_image(p)
        fmt, sid, sver, hdr, count = parse_header(img, p)
        descriptor = None
        rec_start = 16
        if fmt >= 2:
            descriptor, rec_start = parse_descriptor(img)
        ntext, tnames = text_count_for(sid, hdr, descriptor)
        store = {1: "msgs", 2: "contacts", 3: "announces"}.get(sid, "?")
        print(f"\n=== {p}")
        print(f"    fmt={fmt} schema_id={sid}({store}) ver={sver} hdr={hdr} "
              f"records={count} text_fields={tnames}"
              + ("  [format_ver 1: layout inferred from (id,hdr)]" if fmt < 2 else ""))
        if descriptor is not None:
            print("    descriptor: " + ", ".join(
                f"{nm}:{KIND_NAME[k]}@{o}+{w}" for nm, k, o, w in descriptor))
        fixed_schema = None
        if fmt >= 2:
            fixed_schema = Schema(sid, sver)
            fixed_schema.fields = descriptor
            fixed_schema.hdr = hdr
        elif sid == 2 and hdr >= 109:
            fixed_schema = CONTACT_PREFIX
        elif sid == 1 and hdr == 140:
            fixed_schema = MSG_CURRENT
        live = 0
        for off, rlen, flags, key, texts in walk_records(img, hdr, rec_start, ntext):
            if flags & 0x01:
                continue
            live += 1
            fx = decode_fixed(img, off, fixed_schema, hdr) if fixed_schema else {}
            printable = key and all(32 <= c < 127 for c in key)
            print(f"    - key={key.decode() if printable else key.hex()}")
            if fx:
                shown = {k: v for k, v in fx.items() if v not in (0, "")}
                print(f"      fixed: {shown}")
            for nm, tv in zip(tnames, texts):
                if tv:
                    txt = tv.decode("utf-8", "replace")
                    if len(txt) > 200:
                        txt = txt[:200] + f"…(+{len(tv) - 200}B)"
                    print(f"      {nm}: {txt!r}")
        print(f"    live records: {live}")


# ---- recover -----------------------------------------------------------------

def build_disk_image(schema, records):
    """records: list of (key_bytes, {fixed_name: value}, {text_name: bytes})."""
    body = bytearray()
    for key, fixed, texts in records:
        hdrbuf = bytearray(schema.hdr)
        for name, kind, o, w in schema.fields:
            if kind == TEXT:
                continue
            v = fixed.get(name)
            if v in (None, "", 0):
                continue
            if kind == U8:
                hdrbuf[o] = int(v) & 0xFF
            elif kind == U32:
                struct.pack_into("<I", hdrbuf, o, int(v) & 0xFFFFFFFF)
            elif kind == FIXSTR:
                b = v.encode() if isinstance(v, str) else v
                hdrbuf[o:o + min(len(b), w - 1)] = b[:w - 1]
            elif kind == DATA:
                b = bytes.fromhex(v) if isinstance(v, str) else v
                hdrbuf[o:o + w] = b[:w].ljust(w, b"\x00")
        rec = bytearray(hdrbuf)
        rec += struct.pack("<H", len(key)) + key
        for name, _, _, _ in schema.text_fields():
            tv = texts.get(name, b"")
            if isinstance(tv, str):
                tv = tv.encode()
            rec += struct.pack("<H", len(tv)) + tv
        struct.pack_into("<I", rec, 0, len(rec))
        body += rec

    desc = bytearray(struct.pack("<H", len(schema.fields)))
    for name, kind, o, w in schema.fields:
        nb = name.encode()
        desc += struct.pack("<BHHB", kind, o, w, len(nb)) + nb

    img = bytearray(16)
    img[0:4] = b"SGDB"
    struct.pack_into("<HHHHI", img, 4, 2, schema.schema_id, schema.schema_ver,
                     schema.hdr, len(records))
    img += struct.pack("<H", len(desc)) + desc + body
    return bytes(img)


def cmd_recover(src, out):
    img = read_image(src)
    fmt, sid, sver, hdr, count = parse_header(img, src)
    descriptor = None
    rec_start = 16
    if fmt >= 2:
        descriptor, rec_start = parse_descriptor(img)
    ntext, tnames = text_count_for(sid, hdr, descriptor)

    if sid == 1:
        target = msg_current()
        src_fixed = None
        if fmt >= 2:
            src_fixed = Schema(sid, sver); src_fixed.fields = descriptor; src_fixed.hdr = hdr
        elif hdr == 140:
            src_fixed = MSG_CURRENT
    elif sid == 2:
        target = contact_current()
        src_fixed = None
        if fmt >= 2:
            src_fixed = Schema(sid, sver); src_fixed.fields = descriptor; src_fixed.hdr = hdr
        elif hdr >= 109:
            src_fixed = CONTACT_PREFIX
    else:
        sys.exit(f"recover: schema_id {sid} not supported (only msgs/contacts)")

    target_names = {f[0] for f in target.fields}
    records = []
    for off, rlen, flags, key, texts in walk_records(img, hdr, rec_start, ntext):
        if flags & 0x01:
            continue
        fixed = {}
        if src_fixed:
            for k, v in decode_fixed(img, off, src_fixed, hdr).items():
                if k in target_names:
                    fixed[k] = v
        tmap = {nm: tv for nm, tv in zip(tnames, texts) if nm in target_names}
        records.append((key, fixed, tmap))

    disk = build_disk_image(target, records)
    with open(out, "wb") as f:
        f.write(gzip.compress(disk, mtime=0))
    print(f"recovered {len(records)} record(s) from {src} "
          f"(fmt{fmt} hdr{hdr}) -> {out} (current layout, fmt2 hdr{target.hdr})")


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "header":
        cmd_header(sys.argv[2:])
    elif cmd == "dump":
        cmd_dump(sys.argv[2:])
    elif cmd == "recover":
        if len(sys.argv) != 4:
            sys.exit("usage: sgdb_recover.py recover FILE OUT.db.gz")
        cmd_recover(sys.argv[2], sys.argv[3])
    else:
        sys.exit(f"unknown command {cmd!r}")


if __name__ == "__main__":
    main()
