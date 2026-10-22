#!/usr/bin/env python3
"""wikipack.py zim の動作確認用に、Kiwix(mwoffliner)風の小さな .zim を作る。
使い方: python3 test/make_test_zim.py test.zim   (pip install libzim が必要)"""
import sys
import urllib.parse as up
from libzim.writer import Creator, Item, StringProvider, Hint


class It(Item):
    def __init__(s, path, title, content, mime="text/html", front=True):
        super().__init__()
        s.p, s.t, s.c, s.m, s.f = path, title, content, mime, front

    def get_path(s): return s.p
    def get_title(s): return s.t
    def get_mimetype(s): return s.m
    def get_contentprovider(s): return StringProvider(s.c)
    def get_hints(s): return {Hint.FRONT_ARTICLE: s.f}


def page(title, lead, sections):
    body = ('<section data-mw-section-id="0"><p><b>%s</b>%s</p><table class="infobox"><tr><td>表の中身は出さない'
            '</td></tr></table></section>' % (title, lead))
    for i, (h, paras) in enumerate(sections, 1):
        body += '<section data-mw-section-id="%d"><h2 id="%s">%s</h2>%s</section>' % (i, h, h, "".join(paras))
    return '<!DOCTYPE html><html><head><meta charset="utf-8"><title>%s</title></head><body>%s</body></html>' % (title, body)


def a(t, text):
    return '<a rel="mw:WikiLink" href="./%s" title="%s">%s</a>' % (up.quote(t), t, text)


arts = {
    "ネコ": page("ネコ", "（猫、ねこ、Felis catus）は、小型の" + a("哺乳類", "哺乳類") +
                 'である。<sup class="mw-ref reference"><a href="#cite_note-1">[1]</a></sup> 波線〜と ① é。',
                 [("特徴", ["<p>夜によく見える。" + a("イヌ", "犬") + "とは違う。</p><ul><li>項目A<ul><li>入れ子B</li></ul></li><li>項目C</li></ul>"]),
                  ("脚注", ['<ol class="mw-references"><li>消えるはずの脚注</li></ol>'])]),
    "イヌ": page("イヌ", "（犬、いぬ）は、" + a("哺乳類", "哺乳類") + "である。リダイレクト経由：" + a("ねこ", "ねこ"),
                 [("特徴", ["<p>鼻がよく利く。</p>"])]),
    "哺乳類": page("哺乳類", "（ほにゅうるい）は、乳で子を育てる動物。", [("例", ["<p>" + a("ネコ", "ネコ") + "や" + a("イヌ", "イヌ") + "。</p>"])]),
}
for i in range(300):
    t = "テスト記事%03d" % i
    arts[t] = page(t, "（てすときじ%03d）は、試験用。" % i + a("ネコ", "ネコ") + a("テスト記事%03d" % ((i + 1) % 300), "次"),
                   [("節", ["<p>本文%d</p>" % i])])

out = sys.argv[1] if len(sys.argv) > 1 else "test.zim"
with Creator(out).config_indexing(False, "ja").set_mainpath("ネコ") as c:
    for t, h in arts.items():
        c.add_item(It(t, t, h))
    c.add_item(It("style.css", "style", "body{}", "text/css", False))
    c.add_redirection("ねこ", "ねこ", "ネコ", {Hint.FRONT_ARTICLE: True})
    c.add_metadata("Title", "test ja")
    c.add_metadata("Language", "jpn")
print("作成:", out)
