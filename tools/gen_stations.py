#!/usr/bin/env python3
"""Generate the Radio module's factory-preset station set.

Curated, talk/utility-first world set. Stream URLs/codecs/favicons are resolved from
radio-browser.info (canonical, CC0) so we never hand-maintain URLs; entries are filtered
to the codecs the Radio module can actually play (MP3 / AAC / HLS-on-mac, never OGG) and
to radio-browser's `lastcheckok` liveness flag. A few feeds with no radio-browser entry
(Broadcastify scanners/ATC, some ambient streams) are pinned by direct URL.

Favicons: if radio-browser has one and it decodes to a reasonable raster, we bundle it
(normalized to 256x256 PNG); otherwise the preset ships with no icon and the module
synthesizes an avatar from the station name at runtime. We never ship hand-made art.

Run from the repo root:  python3 tools/gen_stations.py   (needs network; macOS `magick`).
Re-runnable: it rewrites presets/Radio/ and the fetched icons under res/stations/.
"""
import json, os, re, subprocess, sys, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESETS = os.path.join(ROOT, "presets", "Radio")
ICONS = os.path.join(ROOT, "res", "stations")
RB = "https://de1.api.radio-browser.info"
UA = {"User-Agent": "akaudio-radio-gen/1.0"}
PLAYABLE = {"MP3", "AAC", "AAC+", "MP2"}  # codecs the module decodes (OGG/OPUS excluded)

# ---- Curated set ---------------------------------------------------------------------
# rb: resolve by radio-browser name search (picks top-voted playable + lastcheckok).
#     `must` = case-insensitive substring the matched name must contain (guards mismatch).
# url: pinned direct stream (no radio-browser entry) — e.g. Broadcastify feeds.
def rb(name, must=None): return {"kind": "rb", "query": name, "must": must or name}
def url(name, u): return {"kind": "url", "name": name, "url": u}
def bcfy(name, feed): return url(name, f"https://broadcastify.cdnstream1.com/{feed}")

CATEGORIES = {
    "News & Talk": [
        rb("BBC World Service"), rb("NPR Program Stream", "NPR"), rb("France Info"),
        rb("France Inter"), rb("France Culture"), rb("Deutschlandfunk", "Deutschlandfunk"),
        rb("ABC NewsRadio", "ABC"), rb("ABC RN", "RN"), rb("CBC Radio One", "CBC"),
        rb("RTE Radio 1", "RTÉ"), rb("RNZ National", "RNZ"), rb("VOA"),
        rb("Al Jazeera English", "Al Jazeera"), rb("WNYC"), rb("KQED"),
        rb("LBC", "LBC"), rb("Times Radio"), rb("TalkRadio", "Talk"),
        rb("NHK World Radio Japan", "NHK"), rb("Radio New Zealand", "RNZ"),
        rb("SBS", "SBS"), rb("Monocle", "Monocle"), rb("GB News Radio", "GB"),
        rb("Euronews", "Euronews"), rb("RFI English", "RFI"),
        rb("Radio Romania International", "Romania"), rb("Radio Praha", "Prague"),
        rb("Polskie Radio", "Polskie"), rb("Radio Sweden", "Sweden"),
        rb("ABC Radio National", "Radio National"),
    ],
    "Spoken & Stories": [
        rb("BBC Radio 4", "Radio 4"), rb("BBC Radio 4 Extra", "4 Extra"),
        rb("RTE Radio 1 Extra", "Extra"), rb("WALM Old Time Radio", "Old Time"),
        rb("Radio Drama", "Drama"), rb("Radio Mystery Theater", "Mystery"),
        rb("Radio Art Poetry", "Poetry"), rb("4RPH Reading Radio", "Reading"),
        rb("BBC Radio Orkney", "Orkney"), rb("historyradio", "history"),
        rb("RMC Nights Story", "Story"), rb("Radio Drama 99.1", "Drama"),
    ],
    "Nature & Ambient": [
        url("Ambi Nature Radio", "https://nature-rex.radioca.st/stream"),
        rb("MyNoise Pure Nature", "MyNoise"),
        url("Nature Radio Rain", "https://maggie.torontocast.com:2020/stream/natureradiorain"),
        rb("MyNoise Ocean Waves", "MyNoise"), rb("MyNoise Rainy Day", "MyNoise"),
        rb("MyNoise Zen Garden", "MyNoise"), rb("Sleep Radio", "Sleep"),
        rb("Birdsong Radio", "Birdsong"), rb("Forest Green Radio", "Forest"),
        rb("Ambient Sleeping Pill", "Ambient"), rb("White Noise Radio", "Noise"),
        rb("Phaune radio", "Phaune"), rb("Nature Radio Sleep", "Nature"),
        rb("24/7 Nature Radio", "Nature"), rb("24/7 Seawaves Radio", "Seawaves"),
        rb("24/7 Country Walks Radio", "Country Walks"), rb("Radio Relax Nature", "Relax"),
        rb("intra Nature Radio", "Nature"),
    ],
    "Space & Science": [
        url("Blue Mars", "http://streams.echoesofbluemars.org:8000/bluemars"),
        url("Cryosleep", "http://streams.echoesofbluemars.org:8000/cryosleep"),
        url("Voices From Within", "http://streams.echoesofbluemars.org:8000/voicesfromwithin"),
        rb("SomaFM Mission Control", "Mission"), rb("SomaFM Deep Space One", "Deep Space"),
        rb("SomaFM Space Station Soma", "Space Station"), rb("SomaFM Synphaera", "Synphaera"),
        rb("SomaFM Vaporwaves", "Vaporwaves"),
    ],
    "Scanners": [
        bcfy("Fairfax County (VA)", "33658"), bcfy("Rivercom (Chelan-Douglas)", "29475"),
        bcfy("Madison County", "1441"), bcfy("Tillamook County", "30651"),
        bcfy("New Haven (CT)", "20380"),
    ],
    "ATC": [
        bcfy("Anchorage Center (ARTCC)", "31716"), bcfy("Aeroparque Tower (SABE)", "45890"),
    ],
}

# Pinned codecs for url() entries that aren't plain MP3.
URL_CODEC = {"BBC Radio 4": "HLS"}

def get(u, timeout=25):
    # Shell out to curl: the system curl has a CA bundle, Python's urllib here does not.
    r = subprocess.run(["curl", "-sSL", "--max-time", str(timeout),
                        "-A", UA["User-Agent"], u], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode("utf-8", "replace")[:120])
    return r.stdout

# Drop low-quality / off-theme / propaganda / space-*music* matches that tag pulls drag in.
BLOCK_NAMES = [
    "infowars", "vesti", "вести", "mayak", "маяк", "sputnik", "rt ", "russia today",
    "caprice", "spacesynth", "nightride", "pumpkin fm", "strange radio",
    "space music", "space rock", "indie beat", "radiostorm", "fun radio for kids",
    "radios libres", "la granja", "globo fm", "filispim", "matorral", "argayo",
    "klassik radio", "epic lounge", "zonasalsa", "hit fm",
]
def name_ok(name):
    n = name.lower().strip()
    if not n or "||" in name or len(name) > 40:  # tag-stuffed / unreadable
        return False
    if name.count(",") >= 2:
        return False
    return not any(b in n for b in BLOCK_NAMES)

def dedupe_key(name):
    # Collapse bitrate/format/stream variants: "Foo (128k AAC)", "Foo MP3", "Foo FM" -> "foo".
    n = re.sub(r"\(.*?\)", " ", name.lower())
    n = re.sub(r"\b(mp3|mp2|aac\+?|hls|fm|am|stereo|hq|hd|\d+\s*k(bps)?|\d+)\b", " ", n)
    return re.sub(r"[^a-z]", "", n)

def playable(s):
    codec = (s.get("codec") or "").upper()
    return bool(s.get("lastcheckok")) and (s.get("hls") == 1 or codec in PLAYABLE)

def rec_from(s):
    return {
        "name": (s.get("name") or "").strip(),
        "url": s.get("url_resolved") or s.get("url"),
        "codec": "HLS" if s.get("hls") == 1 else (s.get("codec") or "").upper(),
        "favicon": s.get("favicon") or "",
        "votes": s.get("votes", 0),
    }

def rb_resolve(query, must):
    try:
        u = f"{RB}/json/stations/search?limit=40&order=votes&reverse=true&hidebroken=true&name={urllib.parse.quote(query)}"
        rows = json.loads(get(u))
    except Exception as e:
        print(f"   ! rb error for {query!r}: {e}")
        return None
    pat = re.compile(r"\b" + re.escape(must) + r"\b", re.I)  # whole-word, not substring
    best = None
    for s in rows:
        if not playable(s) or not pat.search(s.get("name") or ""): continue
        if not name_ok(s.get("name") or ""): continue
        if best is None or s.get("votes", 0) > best.get("votes", 0):
            best = s
    return rec_from(best) if best else None

# Tag-based bulk pull to broaden each category. Music-ish tags are excluded so e.g. the
# Nature/Ambient pull doesn't fill up with pop stations.
BLOCK_TAGS = {"pop", "rock", "dance", "hits", "top 40", "edm", "techno", "house",
              "rap", "hip hop", "metal", "country", "k-pop", "latino", "reggaeton"}
def rb_tag(tags, want, seen_urls, seen_names):
    out = []
    for tag in tags:
        if len(out) >= want: break
        try:
            u = f"{RB}/json/stations/search?tagList={urllib.parse.quote(tag)}&order=votes&reverse=true&hidebroken=true&limit=150"
            rows = json.loads(get(u))
        except Exception:
            continue
        for s in rows:
            if len(out) >= want: break
            if not playable(s) or not name_ok(s.get("name") or ""): continue
            st = {t.strip().lower() for t in (s.get("tags") or "").split(",")}
            if st & BLOCK_TAGS: continue
            rec = rec_from(s)
            nn = dedupe_key(rec["name"])
            if not rec["url"] or rec["url"] in seen_urls or nn in seen_names or not nn:
                continue
            seen_urls.add(rec["url"]); seen_names.add(nn)
            out.append(rec)
    return out

# Only the News tag pull is clean enough to bulk from (real public broadcasters). The
# spoken / nature / space tags on radio-browser are sparse + noisy (local music, chillout,
# space-themed music), so those categories are curated by name instead.
TAGS = {
    "News & Talk": (["news", "public radio", "talk", "current affairs"], 34),
    "Spoken & Stories": ([], 0),
    "Nature & Ambient": ([], 0),
    "Space & Science": ([], 0),
    "Scanners": ([], 0),
    "ATC": ([], 0),
}

def slug(name):
    return re.sub(r"[^a-z0-9]+", "", name.lower())[:24] or "station"

def fetch_icon(favicon, name):
    if not favicon: return ""
    raw = f"/tmp/radioart/_fav_{slug(name)}"
    try:
        data = get(favicon, timeout=15)
        if len(data) < 100: return ""
        open(raw, "wb").write(data)
        out = os.path.join(ICONS, f"{slug(name)}.png")
        # Convert (handles ico/png/jpg/gif) -> probe size -> keep only if reasonable.
        r = subprocess.run(["magick", raw + "[0]", "-resize", "256x256", out],
                           capture_output=True)
        if r.returncode != 0 or not os.path.exists(out): return ""
        dim = subprocess.run(["magick", "identify", "-format", "%w", out],
                             capture_output=True, text=True).stdout.strip()
        if not dim or int(dim) < 256:  # upscaled from a tiny favicon -> synthesize instead
            # accept only if the *source* was decently sized
            src = subprocess.run(["magick", "identify", "-format", "%w", raw + "[0]"],
                                 capture_output=True, text=True).stdout.strip()
            if not src or int(src) < 48:
                os.remove(out); return ""
        return f"stations/{slug(name)}.png"
    except Exception:
        return ""

def main():
    os.makedirs("/tmp/radioart", exist_ok=True)
    # Wipe generated icons (keep nothing hand-made) and presets.
    if os.path.isdir(PRESETS):
        subprocess.run(["rm", "-rf", PRESETS])
    for f in os.listdir(ICONS):
        os.remove(os.path.join(ICONS, f))
    total = 0
    summary = {}
    seen_urls, seen_names = set(), set()
    def norm(n): return dedupe_key(n)
    for cat, entries in CATEGORIES.items():
        cdir = os.path.join(PRESETS, cat)
        os.makedirs(cdir, exist_ok=True)
        recs = []
        for e in entries:
            if e["kind"] == "url":
                rec = {"name": e["name"], "url": e["url"],
                       "codec": URL_CODEC.get(e["name"], "MP3"), "favicon": ""}
            else:
                rec = rb_resolve(e["query"], e["must"])
                if not rec:
                    print(f"   - skip (unresolved): {e['query']}")
                    continue
            if rec["url"] in seen_urls or norm(rec["name"]) in seen_names:
                continue
            seen_urls.add(rec["url"]); seen_names.add(norm(rec["name"]))
            recs.append(rec)
        # Broaden with tag-based pulls up to the category target.
        tags, target = TAGS.get(cat, ([], 0))
        if tags and len(recs) < target:
            recs += rb_tag(tags, target - len(recs), seen_urls, seen_names)
        # Emit.
        idx = 0
        for rec in recs:
            icon = fetch_icon(rec.get("favicon", ""), rec["name"])
            idx += 1
            preset = {"plugin": "akaudio", "model": "Radio", "version": "2.0.0",
                      "params": [], "data": {"url": rec["url"], "stationName": rec["name"],
                                             "icon": icon, "playing": True}}
            fn = f"{idx:02d}_{re.sub(r'[/:]', '-', rec['name'])}.vcvm"
            json.dump(preset, open(os.path.join(cdir, fn), "w"), indent=2)
            print(f"   + [{cat}] {rec['name']}  ({rec.get('codec','?')}, icon={'yes' if icon else 'synth'})")
        summary[cat] = idx
        total += idx
    print("\n=== summary ===")
    for c, n in summary.items():
        print(f"  {c:18} {n}")
    print(f"  {'TOTAL':18} {total}")

if __name__ == "__main__":
    main()
