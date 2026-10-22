#!/usr/bin/env python3
"""UI 文字列(日本語)を Shift_JIS の8進エスケープに変換して src/strings.h を作る。
文字列を変えたらこのスクリプトを再実行する: python3 tools/gen_strings.py"""
import os

STRINGS = {
    "TITLE_SEARCH": "Wikipedia 検索",
    "PROMPT": "よみ:",
    "MODE_KANA": "かな",
    "MODE_ABC": "ABC",
    "NOHIT": "該当なし",
    "LOADING": "読み込み中…",
    "HELP_SEARCH": "Enter:開く BS:消す SHIFT:かな/ABC Back:終了",
    "HELP_ART": "↑↓:移動 ←→:リンク Enter:開く Back:戻る F1:検索",
    "ERR_MEM": "メモリを確保できません",
    "ERR_FONT": "WIKI.FNT を読めません",
    "ERR_PACK": "WIKI.IDX を読めません(形式または文字コード)",
    "ERR_ART": "記事を読めません",
    "ERR_HINT": "どれかキーで終了",
}

out = ["/* 自動生成: tools/gen_strings.py (Shift_JIS, 8進エスケープ) */",
       "#ifndef STRINGS_H", "#define STRINGS_H", ""]
for name, text in STRINGS.items():
    b = text.encode("cp932")
    lit = "".join(chr(c) if 0x20 <= c < 0x7F and chr(c) not in '\\"?' else "\\%03o" % c for c in b)
    out.append('static const char S_%s[] = "%s";' % (name, lit))
out += ["", "#endif", ""]
path = os.path.join(os.path.dirname(__file__), "..", "src", "strings.h")
open(path, "w").write("\n".join(out))
print("wrote", os.path.normpath(path))
