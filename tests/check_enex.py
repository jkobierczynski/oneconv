#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
"""Validate ENEX files against the rules Evernote publishes for ENEX/ENML."""
import sys, re, base64, hashlib, glob, os
import xml.etree.ElementTree as ET

ALLOWED = set("""a abbr acronym address area b bdo big blockquote br caption center cite code col colgroup dd del dfn div
dl dt em font h1 h2 h3 h4 h5 h6 hr i img ins kbd li map ol p pre q s samp small span strike strong sub sup table tbody td
tfoot th thead title tr tt u ul var xmp en-note en-media en-todo en-crypt""".split())
BAD_ATTR = {"id", "class", "accesskey", "data", "dynsrc", "tabindex"}
NOTE_ORDER = ["title", "content", "created", "updated", "tag", "note-attributes", "resource"]
RES_ORDER = ["data", "mime", "width", "height", "duration", "recognition", "resource-attributes", "alternate-data"]
TIME = re.compile(r"^\d{8}T\d{6}Z$")

def check_order(el, order, where, errs):
    idx = -1
    for c in el:
        if c.tag not in order: errs.append(f"{where}: unexpected <{c.tag}>"); continue
        i = order.index(c.tag)
        if i < idx: errs.append(f"{where}: <{c.tag}> out of order")
        idx = max(idx, i)

def check(path):
    errs = []; notes = res = media = todos = 0
    root = ET.parse(path).getroot()
    if root.tag != "en-export": errs.append("root is not en-export")
    for n in root:
        notes += 1
        title = n.findtext("title") or ""
        where = f"note '{title[:40]}'"
        check_order(n, NOTE_ORDER, where, errs)
        if not title.strip() or len(title) > 255 or title != title.strip() or "\n" in title: errs.append(f"{where}: bad title")
        for t in ("created", "updated"):
            v = n.findtext(t)
            if v is not None and not TIME.match(v): errs.append(f"{where}: bad {t} {v}")
        tags = [t.text for t in n.findall("tag")]
        if len(set(x.lower() for x in tags)) != len(tags) or any("," in x or len(x) > 100 or not x.strip() for x in tags): errs.append(f"{where}: bad tags {tags}")
        hashes = {}
        for r in n.findall("resource"):
            res += 1
            check_order(r, RES_ORDER, where + " resource", errs)
            d = r.find("data")
            if d.get("encoding") != "base64": errs.append(f"{where}: resource encoding")
            raw = base64.b64decode(d.text)
            hashes[hashlib.md5(raw).hexdigest()] = r.findtext("mime")
            if not r.findtext("mime"): errs.append(f"{where}: resource without mime")
        content = n.findtext("content")
        if not content.startswith('<?xml version="1.0" encoding="UTF-8"'): errs.append(f"{where}: content header")
        if "<!DOCTYPE en-note SYSTEM \"http://xml.evernote.com/pub/enml2.dtd\">" not in content: errs.append(f"{where}: doctype")
        body = content[content.index("<en-note"):]
        try: doc = ET.fromstring(body)
        except ET.ParseError as e: errs.append(f"{where}: ENML not well-formed: {e}"); continue
        used = set()
        for el in doc.iter():
            if el.tag not in ALLOWED: errs.append(f"{where}: element <{el.tag}> not allowed in ENML")
            for a, v in el.attrib.items():
                if a in BAD_ATTR or a.startswith("on"): errs.append(f"{where}: attribute {a} on <{el.tag}>")
                if a in ("href", "src") and not re.match(r"^(https?|ftp|mailto|tel|file):", v): errs.append(f"{where}: url {v[:60]}")
            if el.tag == "en-media":
                media += 1; h = el.get("hash"); used.add(h)
                if h not in hashes: errs.append(f"{where}: en-media hash {h} has no resource")
                elif hashes[h] != el.get("type"): errs.append(f"{where}: en-media type mismatch")
            if el.tag == "en-todo":
                todos += 1
                if el.get("checked") not in ("true", "false", None): errs.append(f"{where}: en-todo checked")
        for h in hashes:
            if h not in used: errs.append(f"{where}: resource {h} is never referenced")
    return notes, res, media, todos, errs

paths = []
for arg in sys.argv[1:]:
    if os.path.isdir(arg):
        for d, _, files in sorted(os.walk(arg)):
            paths += [os.path.join(d, f) for f in sorted(files) if f.lower().endswith(".enex")]
    else:
        paths.append(arg)
if not paths:
    sys.exit("usage: check_enex.py <file.enex or directory>...")

tot = [0, 0, 0, 0]; bad = 0
for path in paths:
    n, r, m, t, errs = check(path)
    tot = [a + b for a, b in zip(tot, (n, r, m, t))]
    if errs:
        bad += 1; print(os.path.basename(path)); [print("   ", e) for e in errs[:8]]
print(f"{len(paths)} files, {tot[0]} notes, {tot[1]} resources, {tot[2]} en-media, {tot[3]} en-todo; files with problems: {bad}")
sys.exit(1 if bad else 0)
