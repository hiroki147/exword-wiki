#!/usr/bin/env python3
"""wikipack - build an offline Wikipedia pack for a homebrew EX-word viewer.

サブコマンド:
  demo    ネット不要のデモ用キャッシュを作る(ビューア開発・動作確認用)
  fetch   Wikipedia API から記事・リンク・サムネイルをキャッシュに保存
  build   キャッシュから <名前>.IDX / .Dxx / .Ixx を生成(名前は --name、既定 WIKI)
  zim     Kiwix の .zim から直接パックを生成(全記事向け。画像なし)
  nfo     辞書の表示名(.NFO)だけを作る(辞書本体を作り直さなくてよい)
  font    TTF から日本語ビットマップフォント WIKI.FNT を生成(ビューア必須)
  lookup  (参照実装) かな前方一致で検索
  show    (参照実装) 記事を表示し、画像を書き出す
  verify  (参照実装) パック全体の整合性チェック

パックの形式は FORMAT.md を参照。C 側ビューアはこの lookup/show と同じ手順で読めばよい。
"""
import argparse
import glob
import hashlib
import io
import json
import os
import re
import struct
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
import zlib

# ---------------------------------------------------------------- format ---
MAGIC = b"XWK1"
VERSION = 1
ENC_IDS = {"utf-8": 0, "cp932": 1, "euc-jp": 2}
ENC_NAMES = {v: k for k, v in ENC_IDS.items()}
HEADER = struct.Struct("<4sHBBIIHHHH8x")   # 32 bytes
ARTICLE = struct.Struct("<HHIIIIIHH")      # 28 bytes (+ title_size)
KEY_SIZE = 32       # NUL終端込み。かな約15文字(cp932)
TITLE_SIZE = 48     # NUL終端込み
M_HEAD, M_LINK_S, M_LINK_M, M_LINK_E = "\x01", "\x02", "\x03", "\x04"

DEFAULT_DROP = ["脚注", "注釈", "出典", "関連項目", "外部リンク", "参考文献",
                "参照", "脚注・出典", "注釈・出典", "脚注・参考文献", "関連文献"]


# ---------------------------------------------------------------- common ---
def sha(s):
    return hashlib.sha1(s.encode("utf-8")).hexdigest()[:16]


def to_hira(s):
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in s)


def norm_key(s):
    """検索キー: NFKC -> カタカナをひらがなへ -> かな/英数字(小文字)のみ残す。"""
    s = to_hira(unicodedata.normalize("NFKC", s).lower())
    return re.sub(r"[^ぁ-ゖー0-9a-z]", "", s)


def enc_trunc(s, enc, limit):
    """enc でエンコードして limit バイト以内に文字境界で切る。"""
    while True:
        b = s.encode(enc, errors="replace")
        if len(b) <= limit:
            return b
        s = s[:-1]


_KANA_SEG = re.compile(r"[ぁ-ゖァ-ヶー・ 　]{2,40}")
_PAREN = re.compile(r"[（(]([^）)]{1,80})[）)]")
_KKS = None


def lead_yomi(text):
    """導入文の最初の括弧の中から、かなだけの部分(「、」「;」で区切った1つ)を読みとして取る。
    例: 「ネコ（猫、ねこ、Felis catus）」 -> ねこ"""
    for m in _PAREN.finditer(text[:150]):
        for seg in re.split(r"[、，,；;]", m.group(1)):
            seg = seg.strip()
            if _KANA_SEG.fullmatch(seg):
                return seg
    return None


def guess_yomi(rec, use_kakasi=True):
    global _KKS
    if rec.get("yomi"):
        return rec["yomi"], "given"
    y = lead_yomi(rec.get("text", ""))
    if y:
        return y, "lead"
    if use_kakasi:
        try:
            if _KKS is None:
                import pykakasi
                _KKS = pykakasi.kakasi()
            return "".join(x["hira"] for x in _KKS.convert(rec["title"])), "kakasi"
        except ImportError:
            pass
    return rec["title"], "none"


# --------------------------------------------------------- cp932 に寄せる ---
_CP932_FIX = str.maketrans({
    "\u301c": "\uff5e", "\u2016": "\u2225", "\u2212": "\uff0d", "\u00a2": "\uffe0",
    "\u00a3": "\uffe1", "\u00ac": "\uffe2", "\u2014": "\u2015", "\u00a0": " ",
    "\u200b": "", "\u200e": "", "\u200f": "", "\ufeff": "", "\u2010": "-", "\u2011": "-",
    "\u2012": "-", "\u2013": "-", "\u00d7": "\u00d7",
})
_ENC_CACHE = {}
_ALLOWED = None


def _font_chars():
    """WIKI.FNT に字形がある文字(ASCII・半角カナ・JIS X 0208 の 1〜84 区)の集合。
    cp932 では表せても、NEC 13区(①など)や IBM 拡張にはフォントに字形が無く空白になるので除く。"""
    global _ALLOWED
    if _ALLOWED is None:
        a = set(chr(c) for c in range(0x20, 0x7F)) | set("\n\t\x01\x02\x03\x04")
        a |= set(chr(c) for c in range(0xFF61, 0xFFA0))
        for row in range(1, 85):
            for cell in range(1, 95):
                j1, j2 = row + 0x20, cell + 0x20
                s1 = ((j1 - 0x21) >> 1) + 0x81
                if s1 > 0x9F:
                    s1 += 0x40
                s2 = j2 + 0x1F + (1 if j2 >= 0x60 else 0) if j1 & 1 else j2 + 0x7E
                try:  # フォント生成(font サブコマンド)と同じ基準: JIS X 0208 に実在する区点だけ
                    bytes([0xA0 + row, 0xA0 + cell]).decode("euc_jp")
                    a.add(bytes([s1, s2]).decode("cp932"))
                except UnicodeDecodeError:
                    pass
        _ALLOWED = a
    return _ALLOWED


def prep_text(s, enc):
    """cp932 用に、フォントに無い文字を近い文字へ置き換える(〜→～、é→e、①→1、㈱→(株) など)。
    置き換えられないものは ? にする。cp932 以外のときは何もしない。"""
    if enc != "cp932":
        return s
    s = s.translate(_CP932_FIX)
    allowed = _font_chars()
    bad = set(s) - allowed
    if not bad:
        return s
    m = {}
    for ch in bad:
        r = _ENC_CACHE.get(ch)
        if r is None:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))
            r = base if base and all(c in allowed for c in base) else "?"
            _ENC_CACHE[ch] = r
        m[ord(ch)] = r
    return s.translate(m)


# ----------------------------------------------------------------- fetch ---
def api_get(lang, ua, params, retries=4):
    params = dict(params, format="json", formatversion="2")
    url = "https://%s.wikipedia.org/w/api.php?%s" % (lang, urllib.parse.urlencode(params))
    req = urllib.request.Request(url, headers={"User-Agent": ua})
    for i in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception:
            if i == retries - 1:
                raise
            time.sleep(2 * (i + 1))


def query_all(lang, ua, params, delay):
    pages, norm, redir, cont = {}, {}, {}, {}
    while True:
        d = api_get(lang, ua, dict(params, **cont))
        q = d.get("query", {})
        for n in q.get("normalized", []):
            norm[n["from"]] = n["to"]
        for n in q.get("redirects", []):
            redir[n["from"]] = n["to"]
        for p in q.get("pages", []):
            cur = pages.setdefault(p.get("pageid", p["title"]), {"title": p["title"]})
            for k, v in p.items():
                if k == "links":
                    cur.setdefault("links", []).extend(v)
                elif k != "title":
                    cur[k] = v
        if "continue" not in d:
            return pages, norm, redir
        cont = d["continue"]
        time.sleep(delay)


def parse_title_line(line, default_lang):
    """1行を (題名, よみ, 言語) にする。
    題名の代わりに記事の URL も書ける(例: https://ja.wikipedia.org/wiki/C%E8%A8%80%E8%AA%9E)。
    URL なら言語と題名を URL から取るので、曖昧さや表記ゆれで別の記事になることがない。"""
    body, _, y = line.partition("\t")
    body, yomi, lang = body.strip(), (y.strip() or None), default_lang
    if re.match(r"https?://", body, re.I):
        u = urllib.parse.urlparse(body)
        m = re.match(r"([a-z\-]+)\.(?:m\.)?wikipedia\.org$", u.netloc, re.I)
        if not m:
            raise ValueError("Wikipedia の URL ではありません: " + body)
        lang = m.group(1).lower()
        if u.path.startswith("/wiki/"):
            title = urllib.parse.unquote(u.path[len("/wiki/"):])
        else:
            title = (urllib.parse.parse_qs(u.query).get("title") or [""])[0]
        title = title.replace("_", " ").strip()
        if not title:
            raise ValueError("URL から記事名を取り出せません: " + body)
    else:
        title = body.replace("_", " ").strip()
    return title, yomi, lang


def read_titles(path, default_lang="ja"):
    out = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                out.append(parse_title_line(line, default_lang))
            except ValueError as e:
                print("  %d 行目をスキップ: %s" % (n, e))
    return out


def cache_key(title, lang, default_lang):
    """キャッシュのファイル名の元。既定言語は従来どおり題名だけ(取得済みキャッシュを活かす)。"""
    return sha(title if lang == default_lang else lang + ":" + title)


def cmd_fetch(a):
    art_dir = os.path.join(a.cache, "articles")
    img_dir = os.path.join(a.cache, "img")
    os.makedirs(art_dir, exist_ok=True)
    os.makedirs(img_dir, exist_ok=True)
    entries, seen = [], set()
    for t, y, lg in read_titles(a.titles, a.lang):  # 同じ記事の重複は1回だけ
        if (lg, t) not in seen:
            seen.add((lg, t))
            entries.append((t, y, lg))
    todo = [(t, y, lg) for t, y, lg in entries
            if a.refresh or not os.path.exists(os.path.join(art_dir, cache_key(t, lg, a.lang) + ".json"))]
    print("取得対象: %d 件 (キャッシュ済みはスキップ)" % len(todo))
    bsize = 20 if a.lead_only else 1
    fetched = []
    for lang in sorted({lg for _, _, lg in todo}):
        items = [(t, y) for t, y, lg in todo if lg == lang]
        for i in range(0, len(items), bsize):
            batch = items[i:i + bsize]
            params = {"action": "query", "prop": "extracts|pageimages", "redirects": 1,
                      "titles": "|".join(t for t, _ in batch), "explaintext": 1,
                      "piprop": "thumbnail", "pithumbsize": a.thumb}
            if a.lead_only:
                params.update(exintro=1, exlimit="max")
            else:
                params.update(exlimit=1)
            pages, norm, redir = query_all(lang, a.user_agent, params, a.delay)
            by_title = {p["title"]: p for p in pages.values()}
            for t, y in batch:
                ft = redir.get(norm.get(t, t), norm.get(t, t))
                p = by_title.get(ft)
                if not p or p.get("missing") or not p.get("extract"):
                    print("  見つからない/本文なし:", t)
                    continue
                if p["title"] != t:  # 指定した題名と、実際に取れた記事が違うときは必ず知らせる
                    print("  注意: 「%s」 -> 「%s」(リダイレクト/正規化)" % (t, p["title"]))
                rec = {"requested": t, "lang": lang, "title": p["title"], "yomi": y,
                       "text": p["extract"], "links": [], "thumb_file": None}
                th = p.get("thumbnail")
                if th and th.get("source") and not a.no_images:
                    fn = os.path.join(img_dir, cache_key(p["title"], lang, a.lang) + ".bin")
                    try:
                        if not os.path.exists(fn):
                            req = urllib.request.Request(th["source"], headers={"User-Agent": a.user_agent})
                            with urllib.request.urlopen(req, timeout=30) as r, open(fn, "wb") as f:
                                f.write(r.read())
                            time.sleep(a.delay)
                        rec["thumb_file"] = os.path.relpath(fn, a.cache)
                    except Exception as e:
                        print("  画像取得失敗:", p["title"], e)
                with open(os.path.join(art_dir, cache_key(t, lang, a.lang) + ".json"), "w", encoding="utf-8") as f:
                    json.dump(rec, f, ensure_ascii=False)
                fetched.append(rec)
            print("  [%s] %d/%d" % (lang, min(i + bsize, len(items)), len(items)))
            time.sleep(a.delay)

    # 記事間リンク(本文中リンクの代わりに「その記事から張られているリンク先タイトル」を取る)
    print("リンク情報を取得中…")
    for lang in sorted({r["lang"] for r in fetched}):
        recs = [r for r in fetched if r["lang"] == lang]
        for i in range(0, len(recs), 10):
            batch = recs[i:i + 10]
            pages, _, _ = query_all(lang, a.user_agent,
                                    {"action": "query", "prop": "links", "pllimit": "max",
                                     "plnamespace": 0, "redirects": 1,
                                     "titles": "|".join(r["title"] for r in batch)}, a.delay)
            by_title = {p["title"]: p for p in pages.values()}
            for rec in batch:
                rec["links"] = [l["title"] for l in by_title.get(rec["title"], {}).get("links", [])]
                with open(os.path.join(art_dir, cache_key(rec["requested"], lang, a.lang) + ".json"),
                          "w", encoding="utf-8") as f:
                    json.dump(rec, f, ensure_ascii=False)
            time.sleep(a.delay)
    print("完了。次は: wikipack.py build %s OUT" % a.cache)


# ----------------------------------------------------------------- build ---
_HEAD_RE = re.compile(r"^(={2,6})\s*(.+?)\s*\1\s*$")


def process_body(text, drop, max_chars):
    text = re.sub(r"[\x01-\x04]", "", text.replace("\r", ""))
    out, skip, skip_lv, total, blank = [], False, 0, 0, False
    for line in text.split("\n"):
        line = line.rstrip()
        m = _HEAD_RE.match(line)
        if m:
            lv, title = len(m.group(1)), m.group(2)
            if skip and lv > skip_lv:
                continue
            skip = title in drop
            skip_lv = lv
            if skip:
                continue
            out.append("%s%d%s" % (M_HEAD, min(lv, 4), title))
            blank = False
            continue
        if skip:
            continue
        if not line.strip():
            if out and not blank:
                out.append("")
                blank = True
            continue
        blank = False
        out.append(line)
        total += len(line)
        if total >= max_chars:
            break
    while out and out[-1] == "":
        out.pop()
    return out


def add_links(lines, own_title, link_titles, title_to_id, max_links):
    spans = []  # (line, start, end, id)
    seen_ids = set()
    for t in link_titles:
        tid = title_to_id.get(t)
        if tid is None or t == own_title or tid in seen_ids:
            continue
        needle = re.sub(r"\s*[（(].*?[）)]\s*$", "", t)
        if len(needle) < 2:
            continue
        for li, ln in enumerate(lines):
            if ln.startswith(M_HEAD):
                continue
            p = ln.find(needle)
            if p >= 0:
                spans.append((li, p, p + len(needle), tid))
                seen_ids.add(tid)
                break
    spans.sort(key=lambda s: (s[0], s[1], -(s[2] - s[1])))
    keep, last = [], (-1, -1)
    for s in spans:
        if s[0] == last[0] and s[1] < last[1]:
            continue
        keep.append(s)
        last = (s[0], s[2])
    keep = keep[:max_links]
    for li, st, en, tid in sorted(keep, key=lambda s: (s[0], -s[1])):
        ln = lines[li]
        lines[li] = "%s%s%d%s%s%s%s" % (ln[:st], M_LINK_S, tid, M_LINK_M, ln[st:en], M_LINK_E, ln[en:])
    return len(keep)


def make_jpeg(path, w, h, quality, gray):
    from PIL import Image
    im = Image.open(path)
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGB", im.size, (255, 255, 255))
        bg.paste(im, mask=im.split()[-1])
        im = bg
    im = im.convert("L" if gray else "RGB")
    im.thumbnail((w, h), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=False, progressive=False)
    return buf.getvalue(), im.size


def pack_name(s):
    """辞書のファイル名(拡張子なし)。8文字までの英数字。大文字にそろえる。"""
    n = s.upper()
    if not re.fullmatch(r"[A-Z0-9_]{1,8}", n):
        sys.exit("--name は英数字(と _)8文字までにしてください: " + s)
    return n


def write_nfo(out, name, title, enc):
    """辞書の表示名(ビューアの選択画面に出る)。<名前>.NFO の1行目に Shift_JIS で書く。"""
    t = prep_text(title or name, enc).replace("\r", " ").replace("\n", " ")
    b = enc_trunc(t, "cp932" if enc == "cp932" else enc, 34)
    with open(os.path.join(out, name + ".NFO"), "wb") as f:
        f.write(b + b"\r\n")


class Chunker:
    def __init__(self, outdir, letter, limit, prefix="WIKI"):
        self.outdir, self.letter, self.limit, self.prefix = outdir, letter, limit, prefix
        self.no, self.size, self.f, self.files = -1, 0, None, []

    def add(self, blob):
        if self.f is None or (self.size > 0 and self.size + len(blob) > self.limit):
            self._next()
        off = self.size
        self.f.write(blob)
        self.size += len(blob)
        return self.no, off

    def _next(self):
        if self.f:
            self.f.close()
        self.no += 1
        if self.no > 99:
            sys.exit("ファイルが100個を超えました。--chunk-mb を大きくするか記事数を減らしてください")
        name = "%s.%s%02d" % (self.prefix, self.letter, self.no)
        self.f = open(os.path.join(self.outdir, name), "wb")
        self.files.append(name)
        self.size = 0

    def close(self):
        if self.f:
            self.f.close()


def cmd_build(a):
    enc = a.encoding
    drop = set(a.drop_sections.split(",")) if a.drop_sections else set()
    recs = []
    for fn in sorted(glob.glob(os.path.join(a.cache, "articles", "*.json"))):
        with open(fn, encoding="utf-8") as f:
            recs.append(json.load(f))
    seen, uniq = set(), []
    for r in recs:
        if r["title"] not in seen:
            seen.add(r["title"])
            uniq.append(r)
    recs = uniq
    if not recs:
        sys.exit("キャッシュに記事がありません")

    stat_y = {"given": 0, "lead": 0, "kakasi": 0, "none": 0}
    for r in recs:
        y, how = guess_yomi(r)
        stat_y[how] += 1
        key = norm_key(y) or norm_key(r["title"]) or "~"
        r["_key"] = enc_trunc(key, enc, KEY_SIZE - 1)
        r["_title"] = enc_trunc(prep_text(r["title"], enc), enc, TITLE_SIZE - 1)
    recs.sort(key=lambda r: (r["_key"], r["title"]))
    title_to_id = {r["title"]: i for i, r in enumerate(recs)}

    os.makedirs(a.out, exist_ok=True)
    name = pack_name(a.name)
    dch = Chunker(a.out, "D", int(a.chunk_mb * 1024 * 1024), name)
    ich = Chunker(a.out, "I", int(a.chunk_mb * 1024 * 1024), name)
    table, n_links, n_imgs, raw_total, comp_total, img_total = [], 0, 0, 0, 0, 0
    bad_chars = {}
    for i, r in enumerate(recs):
        lines = process_body(r["text"], drop, a.max_chars)
        n_links += add_links(lines, r["title"], r.get("links", []), title_to_id, a.max_links)
        body = prep_text("\n".join(lines), enc)
        if enc != "utf-8":
            for c in set(body):
                if c not in bad_chars:
                    try:
                        c.encode(enc)
                        bad_chars[c] = False
                    except UnicodeEncodeError:
                        bad_chars[c] = True
        raw = body.encode(enc, errors="replace")
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        comp = co.compress(raw) + co.flush()
        dno, doff = dch.add(comp)
        raw_total += len(raw)
        comp_total += len(comp)
        ino, ioff, ilen, iw, ih = 0, 0, 0, 0, 0
        tf = r.get("thumb_file")
        if tf and not a.no_images and os.path.exists(os.path.join(a.cache, tf)):
            try:
                jpg, (iw, ih) = make_jpeg(os.path.join(a.cache, tf), a.img_w, a.img_h, a.img_quality, a.gray)
                ino, ioff = ich.add(jpg)
                ilen = len(jpg)
                n_imgs += 1
                img_total += ilen
            except Exception as e:
                print("画像変換失敗:", r["title"], e)
        table.append(ARTICLE.pack(dno, ino, doff, len(comp), len(raw), ioff, ilen, iw, ih)
                     + r["_title"].ljust(TITLE_SIZE, b"\0"))
    dch.close()
    ich.close()

    write_nfo(a.out, name, a.title, enc)
    with open(os.path.join(a.out, name + ".IDX"), "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, ENC_IDS[enc], 0, len(recs), len(recs),
                            len(dch.files), len(ich.files), KEY_SIZE, TITLE_SIZE))
        for i, r in enumerate(recs):
            f.write(r["_key"].ljust(KEY_SIZE, b"\0") + struct.pack("<I", i))
        for t in table:
            f.write(t)

    nbad = sum(1 for v in bad_chars.values() if v)
    print("記事 %d 件 / リンク %d 個 / 画像 %d 枚" % (len(recs), n_links, n_imgs))
    print("本文 %s -> %s (圧縮)  画像 %s" % (human(raw_total), human(comp_total), human(img_total)))
    print("出力: %s  (D:%d個 I:%d個 + IDX)" % (a.out, len(dch.files), len(ich.files)))
    print("読みの由来: " + ", ".join("%s=%d" % kv for kv in stat_y.items()))
    if stat_y["none"]:
        print("  ! 読みが取れず題名をそのまま使った記事があります(漢字を含むとかな検索に出ません)。")
        print("    pip install pykakasi するか、タイトル一覧に「題名<TAB>よみ」と書いてください。")
    if nbad:
        print("  ! %s で表せない文字が %d 種類あり '?' に置換されます" % (enc, nbad))


def human(n):
    return "%.1f KB" % (n / 1024) if n < 1024 * 1024 else "%.2f MB" % (n / 1048576)


# ------------------------------------------------------------------- zim ---
# Kiwix の .zim(例: wikipedia_ja_all_nopic)から直接パックを作る。
# 150万記事でも動くように、記事ごとに「抽出 -> 圧縮 -> 書き込み」を流し、全文をメモリに溜めない。
#   必要: pip install libzim lxml   (読みの自動付与に pykakasi)
#   画像: ZIM の画像は取り込まない(nopic 版を想定)。

_Z_SKIP_TAGS = {"table", "style", "script", "figure", "math", "noscript", "audio", "video", "svg",
                "img", "link", "meta", "head", "nav", "form", "input", "button", "iframe", "title"}
_Z_SKIP_CLASSES = {"mw-editsection", "noprint", "navbox", "navbox-inner", "hatnote", "reference", "mw-ref",
                   "mwe-math-element", "thumb", "thumbinner", "mw-empty-elt", "metadata", "sistersitebox",
                   "navbar", "ambox", "toc", "mw-references-wrap", "reflist", "references", "infobox",
                   "gallery", "mw-cite-backlink", "shortdescription", "citation"}
_Z_LISTS = ("ul", "ol", "dl")
_Z_WS = re.compile(r"[ \t\r\n\f\v\xa0]+")
_Z_MARK = re.compile("[\x01-\x04]")
_Z_LINKMARK = re.compile("\x02\\d+\x03|\x04")

_ZG = {}   # fork で子プロセスに引き継ぐ状態


def _z_skipped(el):
    tag = el.tag
    if not isinstance(tag, str) or tag in _Z_SKIP_TAGS:
        return True
    cls = el.get("class")
    if cls and not _Z_SKIP_CLASSES.isdisjoint(cls.split()):
        return True
    return el.get("role") == "navigation"


def _z_resolve(href, g):
    if not href or href.startswith(("http:", "https:", "mailto:", "//", "#", "data:")):
        return None
    h = urllib.parse.unquote(href.split("#", 1)[0].split("?", 1)[0])
    while h.startswith("./"):
        h = h[2:]
    while h.startswith("../"):
        h = h[3:]
    h = h.lstrip("/")
    if g["legacy"] and h.startswith("A/"):
        h = h[2:]
    return g["path2id"].get(h)


def _z_inline(el, g, st, stop_lists=False):
    parts = [_Z_MARK.sub("", el.text or "")]
    for ch in el:
        tag = ch.tag
        if isinstance(tag, str):
            if _z_skipped(ch):
                pass
            elif tag == "br":
                parts.append(" ")
            elif stop_lists and tag in _Z_LISTS:
                pass
            elif tag == "a":
                text = _z_inline(ch, g, st)
                aid = _z_resolve(ch.get("href"), g) if st["link"] else None
                if (aid is not None and aid != st["self"] and aid not in st["seen"]
                        and len(st["seen"]) < g["max_links"] and text.strip()):
                    st["seen"].add(aid)
                    text = "\x02%d\x03%s\x04" % (aid, text)
                parts.append(text)
            else:
                parts.append(_z_inline(ch, g, st))
        parts.append(_Z_MARK.sub("", ch.tail or ""))
    return "".join(parts)


def _z_blocks(el, g, st):
    for ch in el:
        tag = ch.tag
        if _z_skipped(ch):
            continue
        if tag in ("h2", "h3", "h4", "h5", "h6"):
            st["link"] = False
            text = _z_inline(ch, g, st)
            st["link"] = True
            yield "h", int(tag[1]), text
        elif tag == "h1":
            continue
        elif tag in ("p", "pre", "dt", "dd"):
            yield "p", 0, _z_inline(ch, g, st)
        elif tag == "li":
            yield "li", 0, _z_inline(ch, g, st, stop_lists=True)
            for sub in ch:
                if isinstance(sub.tag, str) and sub.tag in _Z_LISTS and not _z_skipped(sub):
                    yield from _z_blocks_list(sub, g, st)
        elif tag in _Z_LISTS or tag in ("div", "section", "article", "main", "blockquote", "body", "details"):
            yield from _z_blocks(ch, g, st)


def _z_blocks_list(el, g, st):
    yield from _z_blocks(el, g, st)


def zim_extract(html, g, self_aid):
    """HTML -> (本文(マークアップ付き), 導入文(リンク記号なし))。"""
    import lxml.html
    doc = lxml.html.document_fromstring(html)
    body = doc.find("body")
    if body is None:
        body = doc
    st = {"link": True, "seen": set(), "self": self_aid}
    lines, lead, total = [], None, 0
    skip_lv = None
    for kind, lv, text in _z_blocks(body, g, st):
        text = _Z_WS.sub(" ", text).strip()
        if kind == "h":
            plain = _Z_LINKMARK.sub("", text)
            if g["lead_only"]:
                break
            if skip_lv is not None and lv > skip_lv:
                continue
            skip_lv = lv if plain in g["drop"] else None
            if skip_lv is None and plain:
                lines.append("\x01%d%s" % (min(lv, 4), plain))
            continue
        if skip_lv is not None or not text:
            continue
        if lead is None and kind == "p":
            lead = _Z_LINKMARK.sub("", text)
        if kind == "li":
            text = "・" + text
        lines.append(text)
        total += len(text)
        if total >= g["max_chars"]:
            break
    # 末尾の見出しだけが残らないように落とす
    while lines and lines[-1].startswith("\x01"):
        lines.pop()
    return "\n".join(lines), (lead or "")


def _z_archive():
    ar = _ZG.get("ar")
    if ar is None:
        from libzim.reader import Archive
        ar = _ZG["ar"] = Archive(_ZG["g"]["zim"])
    return ar


def _z_work(span):
    g = _ZG["g"]
    ar = _z_archive()
    out = []
    for k in range(span[0], span[1]):
        eid, path, title = g["arts"][k]
        try:
            html = bytes(ar._get_entry_by_id(eid).get_item().content).decode("utf-8", "replace")
            body, lead = zim_extract(html, g, k)
        except Exception as e:  # 1記事の失敗で全体を止めない
            body, lead = "", ""
            out.append((k, title, None, "error:%s" % e, b"", 0))
            continue
        y, how = guess_yomi({"title": title, "text": lead}, g["kakasi"])
        raw = prep_text(body, g["enc"]).encode(g["enc"], errors="replace")
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        out.append((k, title, y, how, co.compress(raw) + co.flush(), len(raw)))
    return out


class PackWriter:
    """記事IDを先に決めておき、完成した順に書き込む(IDX は最後にまとめて書く)。"""

    def __init__(self, out, n, enc, chunk_bytes, name="WIKI"):
        os.makedirs(out, exist_ok=True)
        self.out, self.n, self.enc, self.name = out, n, enc, name
        self.d = Chunker(out, "D", chunk_bytes, name)
        self.i = Chunker(out, "I", chunk_bytes, name)
        self.rec = ARTICLE.size + TITLE_SIZE
        self.table = bytearray(n * self.rec)
        self.keys = []

    def put(self, aid, title, yomi, comp, rlen):
        enc = self.enc
        key = enc_trunc(norm_key(yomi or "") or norm_key(title) or "~", enc, KEY_SIZE - 1)
        tb = enc_trunc(prep_text(title, enc), enc, TITLE_SIZE - 1)
        dno, doff = self.d.add(comp)
        self.table[aid * self.rec:(aid + 1) * self.rec] = \
            ARTICLE.pack(dno, 0, doff, len(comp), rlen, 0, 0, 0, 0) + tb.ljust(TITLE_SIZE, b"\0")
        self.keys.append((key, aid))

    def finish(self):
        self.d.close()
        self.i.close()
        self.keys.sort()
        with open(os.path.join(self.out, self.name + ".IDX"), "wb") as f:
            f.write(HEADER.pack(MAGIC, VERSION, ENC_IDS[self.enc], 0, self.n, len(self.keys),
                                len(self.d.files), len(self.i.files), KEY_SIZE, TITLE_SIZE))
            for key, aid in self.keys:
                f.write(key.ljust(KEY_SIZE, b"\0") + struct.pack("<I", aid))
            f.write(bytes(self.table))


def zim_scan(zim_path, limit):
    """ZIM を走査し、記事(HTML)の一覧と、リンク解決用の パス->記事ID 表を作る。"""
    from libzim.reader import Archive
    ar = Archive(zim_path)
    legacy = not ar.has_new_namespace_scheme
    arts, redirects = [], {}
    n = ar.entry_count
    print("ZIM: 利用者向けエントリ %d 件 (%s)" % (n, "旧名前空間" if legacy else "新名前空間"))
    for eid in range(n):
        e = ar._get_entry_by_id(eid)
        path = e.path
        if legacy:
            if not path.startswith("A/"):
                continue
            path = path[2:]
        if e.is_redirect:
            t = e.get_redirect_entry().path
            redirects[path] = t[2:] if legacy and t.startswith("A/") else t
            continue
        if not e.get_item().mimetype.startswith("text/html"):
            continue
        arts.append((eid, path, e.title or path))
        if limit and len(arts) >= limit:
            break
        if eid and eid % 200000 == 0:
            print("  走査 %d/%d (記事 %d)" % (eid, n, len(arts)))
    path2id = {path: k for k, (_, path, _) in enumerate(arts)}
    nredir = 0
    for src in redirects:  # リダイレクトも最終的な記事に解決して、リンク先として使えるようにする
        t, hops = redirects[src], 0
        while t in redirects and hops < 8:
            t, hops = redirects[t], hops + 1
        if t in path2id and src not in path2id:
            path2id[src] = path2id[t]
            nredir += 1
    print("記事 %d 件 / リダイレクト %d 件 (リンク解決用)" % (len(arts), nredir))
    return ar, arts, path2id, legacy


def cmd_zim(a):
    t0 = time.time()
    ar, arts, path2id, legacy = zim_scan(a.zim, a.limit)
    if not arts:
        sys.exit("記事が見つかりません")
    for eid, path, title in arts[:3]:
        print("  例:", title, "<-", path)
    g = {"zim": a.zim, "arts": arts, "path2id": path2id, "legacy": legacy, "enc": a.encoding,
         "lead_only": a.lead_only, "max_chars": a.max_chars, "max_links": a.max_links,
         "drop": set(a.drop_sections.split(",")) if a.drop_sections else set(),
         "kakasi": not a.no_kakasi}
    _ZG["g"] = g
    name = pack_name(a.name)
    pw = PackWriter(a.out, len(arts), a.encoding, int(a.chunk_mb * 1024 * 1024), name)
    span = 100
    spans = [(i, min(i + span, len(arts))) for i in range(0, len(arts), span)]
    stat, errs, done, raw_total, comp_total = {}, 0, 0, 0, 0

    def consume(results):
        nonlocal errs, done, raw_total, comp_total
        for k, title, y, how, comp, rlen in results:
            if how.startswith("error"):
                errs += 1
                if errs <= 5:
                    print("  記事の変換に失敗(空で保存):", title, how)
                y, how = None, "none"
            stat[how] = stat.get(how, 0) + 1
            if not comp:
                co = zlib.compressobj(9, zlib.DEFLATED, -15)
                comp = co.compress(b"") + co.flush()
            pw.put(k, title, y, comp, rlen)
            raw_total += rlen
            comp_total += len(comp)
            done += 1
        if done // 10000 != (done - len(results)) // 10000 or done == len(arts):
            el = time.time() - t0
            print("  %d/%d  (%.0f 件/秒, 経過 %.0f 秒)" % (done, len(arts), done / max(el, 1e-9), el))

    jobs = max(1, a.jobs)
    if jobs > 1:
        import multiprocessing as mp
        try:
            ctx = mp.get_context("fork")
        except ValueError:
            print("この環境では並列処理(fork)が使えないので --jobs 1 で続けます")
            jobs = 1
    if jobs > 1:
        with ctx.Pool(jobs) as pool:
            for res in pool.imap_unordered(_z_work, spans):
                consume(res)
    else:
        for sp in spans:
            consume(_z_work(sp))
    pw.finish()
    write_nfo(a.out, name, a.title, a.encoding)
    print("完了: 記事 %d 件 / 本文 %s -> %s (圧縮) / ファイル D:%d 個" %
          (len(arts), human(raw_total), human(comp_total), len(pw.d.files)))
    print("読みの由来: " + ", ".join("%s=%d" % kv for kv in sorted(stat.items())))
    if errs:
        print("  ! 変換に失敗した記事 %d 件(最初の5件を表示済み)" % errs)
    if stat.get("none"):
        print("  ! 読みが取れなかった記事があります。pip install pykakasi を入れるか、--no-kakasi を外してください。")
    print("出力: %s (%s.*)   次は日本語フォント(*.FNT)を電子辞書に入れて verify" % (a.out, name))


# ------------------------------------------------------------- reference ---
class Pack:
    """C 側ビューアが実装すべき読み出し手順の参照実装(seek+read のみ)。"""

    def __init__(self, d, name=None):
        self.d = d
        if name is None:  # フォルダ内の *.IDX が1つならそれを使う
            c = [f for f in os.listdir(d) if f.upper().endswith(".IDX")]
            if len(c) != 1:
                sys.exit("--name を指定してください(見つかった辞書: %s)" % (", ".join(sorted(c)) or "なし"))
            name = c[0][:-4]
        self.pname = name
        self.idx = open(os.path.join(d, name + ".IDX"), "rb")
        h = HEADER.unpack(self.idx.read(HEADER.size))
        (magic, ver, enc, _fl, self.n_art, self.n_key,
         self.n_d, self.n_i, self.key_size, self.title_size) = h
        assert magic == MAGIC and ver == VERSION, "bad magic/version"
        self.enc = ENC_NAMES[enc]
        self.key_off = HEADER.size
        self.key_rec = self.key_size + 4
        self.art_off = self.key_off + self.n_key * self.key_rec
        self.art_rec = ARTICLE.size + self.title_size

    def _key(self, i):
        self.idx.seek(self.key_off + i * self.key_rec)
        b = self.idx.read(self.key_rec)
        return b[:self.key_size].split(b"\0", 1)[0], struct.unpack("<I", b[self.key_size:])[0]

    def prefix(self, text, limit=20):
        q = norm_key(text).encode(self.enc, errors="replace")
        lo, hi = 0, self.n_key
        while lo < hi:                      # lower bound
            mid = (lo + hi) // 2
            if self._key(mid)[0] < q:
                lo = mid + 1
            else:
                hi = mid
        out = []
        while lo < self.n_key and len(out) < limit:
            k, aid = self._key(lo)
            if not k.startswith(q):
                break
            out.append(aid)
            lo += 1
        return out

    def rec(self, aid):
        self.idx.seek(self.art_off + aid * self.art_rec)
        b = self.idx.read(self.art_rec)
        dno, ino, off, clen, rlen, ioff, ilen, iw, ih = ARTICLE.unpack(b[:ARTICLE.size])
        title = b[ARTICLE.size:].split(b"\0", 1)[0].decode(self.enc, errors="replace")
        return dict(id=aid, title=title, dno=dno, ino=ino, off=off, clen=clen, rlen=rlen,
                    ioff=ioff, ilen=ilen, iw=iw, ih=ih)

    def body(self, aid):
        r = self.rec(aid)
        with open(os.path.join(self.d, "%s.D%02d" % (self.pname, r["dno"])), "rb") as f:
            f.seek(r["off"])
            comp = f.read(r["clen"])
        raw = zlib.decompress(comp, -15)
        assert len(raw) == r["rlen"], "rlen mismatch"
        return raw.decode(self.enc, errors="replace")

    def image(self, aid):
        r = self.rec(aid)
        if not r["ilen"]:
            return None
        with open(os.path.join(self.d, "%s.I%02d" % (self.pname, r["ino"])), "rb") as f:
            f.seek(r["ioff"])
            return f.read(r["ilen"])


_LINK_RE = re.compile("\x02(\\d+)\x03(.*?)\x04")


def render(text):
    out = []
    for ln in text.split("\n"):
        if ln.startswith(M_HEAD):
            out.append("\n■ " + ln[2:])
        else:
            out.append(_LINK_RE.sub(lambda m: "[%s→#%s]" % (m.group(2), m.group(1)), ln))
    return "\n".join(out)


def cmd_lookup(a):
    p = Pack(a.pack, a.name)
    ids = p.prefix(a.query, a.limit)
    if not ids:
        print("(該当なし)")
    for i in ids:
        r = p.rec(i)
        print("#%d  %s  %s" % (i, r["title"], "[画像 %dx%d]" % (r["iw"], r["ih"]) if r["ilen"] else ""))


def cmd_show(a):
    p = Pack(a.pack, a.name)
    r = p.rec(a.id)
    print("=== %s (#%d) ===" % (r["title"], a.id))
    print(render(p.body(a.id)))
    img = p.image(a.id)
    if img and a.save_image:
        with open(a.save_image, "wb") as f:
            f.write(img)
        print("\n画像を保存: %s (%dx%d, %d bytes)" % (a.save_image, r["iw"], r["ih"], len(img)))


def cmd_verify(a):
    p = Pack(a.pack, a.name)
    errs = 0
    prev = b""
    for i in range(p.n_key):
        k, aid = p._key(i)
        if k < prev:
            print("キーが整列していない:", i)
            errs += 1
        prev = k
        if aid >= p.n_art:
            print("不正な記事ID:", i)
            errs += 1
    ids = range(p.n_art)
    if p.n_art > a.sample:  # 大きなパックは無作為に一部だけ全文チェックする
        import random
        ids = random.Random(1).sample(range(p.n_art), a.sample)
    nlink = 0
    for i in ids:
        try:
            body = p.body(i)
            for m in _LINK_RE.finditer(body):
                nlink += 1
                if int(m.group(1)) >= p.n_art:
                    print("リンク先IDが範囲外:", i, m.group(0))
                    errs += 1
            img = p.image(i)
            if img and img[:2] != b"\xff\xd8":
                print("JPEGではない:", i)
                errs += 1
        except Exception as e:
            print("記事 #%d 読み出し失敗: %s" % (i, e))
            errs += 1
    print("検証: 記事 %d (本文チェック %d) / キー %d / リンク %d / エラー %d" %
          (p.n_art, len(ids), p.n_key, nlink, errs))
    sys.exit(1 if errs else 0)


# ------------------------------------------------------------------ font ---
FONT_HEADER = struct.Struct("<4sBBBBHH4x")   # 16 bytes: magic, fw, fh, hw, pad, rows, cells


def cmd_nfo(a):
    name = pack_name(a.name)
    os.makedirs(a.out, exist_ok=True)
    write_nfo(a.out, name, a.title, "cp932")
    path = os.path.join(a.out, name + ".NFO")
    print("作成:", path, "->", open(path, "rb").read().decode("cp932").strip())
    print("電子辞書へ: setpath を送り先(mem:/// か sd:///)にして  send %s.NFO" % name)


def cmd_font(a):
    """JIS X 0208 全角(行1-84) + 半角256文字のビットマップフォントを書き出す。
    全角は JIS 区点順(index=(区-1)*94+(点-1))、半角は cp932 の1バイトコード順。
    ビューアは Shift_JIS(cp932) 本文を区点に変換して引く(変換表が要らない)。"""
    from PIL import Image, ImageDraw, ImageFont
    fw = fh = a.size
    hw = fw // 2
    base = round(fh * a.baseline)
    cjk = ImageFont.truetype(a.ttf, a.size, index=a.index)
    asc_path = a.ascii_ttf or a.ttf
    asc = ImageFont.truetype(asc_path, a.ascii_size or (a.size - 3 if a.ascii_ttf else a.size - 1))

    def render(font, ch, w, b):
        im = Image.new("1", (w, fh), 0)
        d = ImageDraw.Draw(im)
        d.fontmode = "1"
        d.text((0, b), ch, font=font, fill=1, anchor="ls")
        return im.tobytes()

    half = bytearray()
    for c in range(256):
        if 0x20 <= c <= 0x7E:
            half += render(asc, chr(c), hw, base)
        elif 0xA1 <= c <= 0xDF:
            half += render(cjk, bytes([c]).decode("cp932"), hw, base)
        else:
            half += bytes(((hw + 7) // 8) * fh)
    full = bytearray()
    blank = bytes(((fw + 7) // 8) * fh)
    drawn = 0
    for row in range(1, 85):
        for cell in range(1, 95):
            try:
                ch = bytes([0xA0 + row, 0xA0 + cell]).decode("euc_jp")
            except UnicodeDecodeError:
                full += blank
                continue
            full += render(cjk, ch, fw, base)
            drawn += 1
    with open(a.out, "wb") as f:
        f.write(FONT_HEADER.pack(b"XFN1", fw, fh, hw, 0, 84, 94))
        f.write(half)
        f.write(full)
    print("%s: %dx%d 全角%d字 + 半角256, %s" % (a.out, fw, fh, drawn, human(16 + len(half) + len(full))))


# ------------------------------------------------------------------ demo ---
DEMO = [
    ("動物", None, "動物（どうぶつ）は、自分で動き回り、ほかの生き物などを食べて生きる生き物の仲間である。\n"
     "== 分類 ==\nおおまかに、背骨のあるものとないものに分けられる。背骨のある動物には哺乳類が含まれる。\n"
     "== 脚注 ==\nこの節は取り除かれるべき部分のテストである。", ["哺乳類", "ネコ", "イヌ"], (200, 120, 40)),
    ("哺乳類", None, "哺乳類（ほにゅうるい）は、子を乳で育てる動物の仲間である。\n== 例 ==\nネコやイヌは哺乳類である。",
     ["動物", "ネコ", "イヌ"], (60, 140, 200)),
    ("ネコ", None, "ネコは、人と長く暮らしてきた小型の哺乳類である。\n== 特徴 ==\n夜によく見える目を持つ。動物の中でも身のこなしが軽い。",
     ["哺乳類", "動物"], (180, 80, 160)),
    ("イヌ", "いぬ", "イヌは、人と古くから暮らしてきた哺乳類である。\n== 特徴 ==\n鼻がよく利く。動物の中でも群れで暮らす性質が強い。",
     ["哺乳類", "動物"], (80, 170, 90)),
    ("北海道", None, "北海道（ほっかいどう）は、日本の最も北にある大きな島とその周辺の地域である。\n== 都市 ==\n中心となる都市は札幌市である。",
     ["札幌市"], (90, 120, 220)),
    ("札幌市", None, "札幌市（さっぽろし）は、北海道の中心となる都市である。\n== 概要 ==\n冬の雪で知られる。",
     ["北海道"], (230, 230, 240)),
]


def cmd_demo(a):
    from PIL import Image, ImageDraw
    os.makedirs(os.path.join(a.cache, "articles"), exist_ok=True)
    os.makedirs(os.path.join(a.cache, "img"), exist_ok=True)
    for title, yomi, text, links, color in DEMO:
        im = Image.new("RGB", (320, 200), color)
        d = ImageDraw.Draw(im)
        for k in range(0, 200, 20):
            d.line([(0, k), (320, 199 - k)], fill=(255, 255, 255), width=2)
        d.rectangle([100, 60, 220, 140], outline=(0, 0, 0), width=4)
        fn = os.path.join(a.cache, "img", sha(title) + ".bin")
        im.save(fn, "PNG")
        rec = {"requested": title, "title": title, "yomi": yomi, "text": text, "links": links,
               "thumb_file": os.path.relpath(fn, a.cache)}
        with open(os.path.join(a.cache, "articles", sha(title) + ".json"), "w", encoding="utf-8") as f:
            json.dump(rec, f, ensure_ascii=False)
    print("デモ用キャッシュを作成:", a.cache, "(%d 件。文章はデモ用の自作で Wikipedia の本文ではありません)" % len(DEMO))


# ------------------------------------------------------------------ main ---
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    s = sp.add_parser("demo")
    s.add_argument("cache")
    s.set_defaults(f=cmd_demo)

    s = sp.add_parser("fetch")
    s.add_argument("titles", help="記事の一覧(1行1件。題名か記事のURL。「題名<TAB>よみ」も可。#はコメント)")
    s.add_argument("cache", help="キャッシュ用ディレクトリ")
    s.add_argument("--user-agent", required=True,
                   help='Wikimedia の規約上必須。例: "my-exword-wiki/0.1 (you@example.com)"')
    s.add_argument("--lang", default="ja", help="題名だけの行の言語(URLの行は URL の言語を使う)")
    s.add_argument("--lead-only", action="store_true", help="導入部だけ取得(速い・小さい)")
    s.add_argument("--thumb", type=int, default=320, help="サムネイル幅px")
    s.add_argument("--delay", type=float, default=0.5, help="リクエスト間隔(秒)")
    s.add_argument("--refresh", action="store_true", help="取得済みも取り直す")
    s.add_argument("--no-images", action="store_true", help="サムネイルを取らない(速い)")
    s.set_defaults(f=cmd_fetch)

    s = sp.add_parser("build")
    s.add_argument("cache")
    s.add_argument("out")
    s.add_argument("--encoding", choices=list(ENC_IDS), default="cp932",
                   help="パック内の文字コード(ビューアの描画関数に合わせる。要確認)")
    s.add_argument("--max-chars", type=int, default=20000, help="1記事の本文上限(文字)")
    s.add_argument("--max-links", type=int, default=80)
    s.add_argument("--drop-sections", default=",".join(DEFAULT_DROP))
    s.add_argument("--chunk-mb", type=float, default=4.0, help="1ファイルの目安サイズ")
    s.add_argument("--no-images", action="store_true")
    s.add_argument("--name", default="WIKI", help="辞書のファイル名(8文字までの英数字)。複数の辞書を入れ分けるとき変える")
    s.add_argument("--title", help="選択画面に出す辞書の名前(日本語可)")
    s.add_argument("--img-w", type=int, default=240)
    s.add_argument("--img-h", type=int, default=160)
    s.add_argument("--img-quality", type=int, default=60)
    s.add_argument("--gray", action="store_true", help="画像をグレースケールにして軽くする")
    s.set_defaults(f=cmd_build)

    s = sp.add_parser("nfo")
    s.add_argument("out", help="出力先フォルダ(例: . )")
    s.add_argument("--name", default="WIKI", help="辞書のファイル名(送信済みの NAME.IDX の NAME と同じにする)")
    s.add_argument("--title", required=True, help="選択画面に出す名前(日本語可)")
    s.set_defaults(f=cmd_nfo)

    s = sp.add_parser("font")
    s.add_argument("out", help="出力ファイル(WIKI.FNT)")
    s.add_argument("--ttf", required=True, help="日本語TTF/OTF/TTC")
    s.add_argument("--index", type=int, default=0, help="TTCの場合のフォント番号")
    s.add_argument("--size", type=int, default=16, help="全角のピクセル数(半角は半分)")
    s.add_argument("--ascii-ttf", help="半角英数用フォント(等幅推奨)。省略時は --ttf")
    s.add_argument("--ascii-size", type=int)
    s.add_argument("--baseline", type=float, default=0.88, help="ベースライン位置(高さに対する割合)")
    s.set_defaults(f=cmd_font)

    s = sp.add_parser("zim")
    s.add_argument("zim", help="Kiwix の .zim ファイル(例: wikipedia_ja_all_nopic_*.zim)")
    s.add_argument("out")
    s.add_argument("--encoding", choices=list(ENC_IDS), default="cp932")
    s.add_argument("--lead-only", action="store_true", help="導入部だけ(小さく・速い)")
    s.add_argument("--max-chars", type=int, default=20000, help="1記事の本文上限(文字)")
    s.add_argument("--max-links", type=int, default=80)
    s.add_argument("--drop-sections", default=",".join(DEFAULT_DROP))
    s.add_argument("--chunk-mb", type=float, default=64.0, help="本文ファイル1つの目安サイズ(最大100個)")
    s.add_argument("--jobs", type=int, default=1, help="並列数(CPUコア数くらい)")
    s.add_argument("--limit", type=int, default=0, help="先頭から N 記事だけ(試運転・容量見積もり用)")
    s.add_argument("--no-kakasi", action="store_true", help="pykakasi による読みの推定をしない(速い)")
    s.add_argument("--name", default="WIKI", help="辞書のファイル名(8文字までの英数字)。複数の辞書を入れ分けるとき変える")
    s.add_argument("--title", help="選択画面に出す辞書の名前(日本語可)")
    s.set_defaults(f=cmd_zim)

    s = sp.add_parser("lookup")
    s.add_argument("pack")
    s.add_argument("--name", help="辞書のファイル名(フォルダに *.IDX が1つだけなら省略可)")
    s.add_argument("query")
    s.add_argument("--limit", type=int, default=20)
    s.set_defaults(f=cmd_lookup)

    s = sp.add_parser("show")
    s.add_argument("pack")
    s.add_argument("--name", help="辞書のファイル名(フォルダに *.IDX が1つだけなら省略可)")
    s.add_argument("id", type=int)
    s.add_argument("--save-image", metavar="FILE.jpg")
    s.set_defaults(f=cmd_show)

    s = sp.add_parser("verify")
    s.add_argument("pack")
    s.add_argument("--name", help="辞書のファイル名(フォルダに *.IDX が1つだけなら省略可)")
    s.add_argument("--sample", type=int, default=20000, help="本文を全文チェックする記事数の上限(無作為)")
    s.set_defaults(f=cmd_verify)

    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
