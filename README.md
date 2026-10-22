# exword-wiki: EX-word DATAPLUS 6 用 オフライン Wikipedia

PC で Wikipedia の記事を固めた辞書(パック。<名前>.IDX / .Dnn / .Inn / .NFO)を作り、電子辞書側のビューアアプリ(XWIKI)で読む。
通信機能は使わない。画像(各記事の先頭画像)も表示できる。

重要: ビューアは **実機でまだ一度も動かしていない**。ここまでの確認は次の3点。
- SH-4 向けに実際のツールチェインでビルドできる(コード約42KB / 静的RAM約7KB。上限は128KB / 32KB)
- PC 上のシミュレータで、検索・記事表示・画像・リンク移動・戻るが動く(画面は PPM で書き出して目視)
- パック作成ツールとビューアの読み出し処理が、デモデータで一致する

## 中身

    wikipack.py     PC 側ツール(zim / fetch / build / font / demo / lookup / show / verify)
    src/            ビューア(main.c=画面と操作, core.c=パック読み出し・ローマ字・折り返し)
    prebuilt/       ビルド済みアプリ(ja/XWIKI, cn/XWIKI)
    demo-pack/      デモ用データ一式(辞書2つ: WIKI と TEST、日本語フォント WIKI.FNT)。文章は自作で Wikipedia の本文ではない
    test/           PC シミュレータ(sh test/run_sim.sh demo-pack "ne,ENTER")
    FORMAT.md       パック形式の仕様

## 1. データを作る(PC)

    pip install pillow pykakasi

日本語フォント(WIKI.FNT)。TTF は手持ちのものでよい(例は Noto Sans CJK):

    python3 wikipack.py font WIKI.FNT \
      --ttf /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc --index 0 \
      --ascii-ttf /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf

載せたい記事を titles.txt に1行1件で書く。題名でも、記事の URL でもよい(混在可、# から始まる行はコメント):

    ネコ
    https://ja.wikipedia.org/wiki/C%E8%A8%80%E8%AA%9E
    https://ja.wikipedia.org/wiki/北海道
    https://en.wikipedia.org/wiki/Cat	ねこ

- 題名だけだと「C言語」のつもりが別の記事(「C」など)になることがある。確実に指定したい記事は
  ブラウザのアドレスバーの URL をそのまま貼るのがよい(言語も URL から決まる。スマホ版 URL も可)。
- 末尾に「<TAB>よみ」を付けると、検索用の読みを自分で指定できる。
- fetch は、指定した題名と実際に取れた記事が違うとき(リダイレクトなど)に
  「注意: 「A」 -> 「B」」と表示する。意図と違う記事になっていないか、この表示を確認してほしい。
取得してパックにする:

    python3 wikipack.py fetch titles.txt cache --lead-only \
      --user-agent "exword-wiki/0.1 (連絡先メール)"
    python3 wikipack.py build cache out --encoding cp932 --name WIKI --title "自分用Wikipedia"
    cp WIKI.FNT out/
    python3 wikipack.py verify out

- fetch の `--user-agent` は Wikimedia の規約上必須(連絡先を入れる)。
- `--lead-only` は導入部だけ。外すと全文(1記事ずつ取るので遅い)。
- 画像は「フリーライセンスの先頭画像」だけ。`--no-images` で無し、`--gray` で軽量化。
- `--name` は辞書のファイル名(英数字8文字まで。既定 WIKI)、`--title` は辞書の選択画面に出す名前(日本語可)。
  複数の辞書を入れるときは辞書ごとに `--name` を変える(ファイル名が重ならないように)。
- ビューアは文字コード cp932 専用。cp932 で表せない文字は ? になる。
- fetch は私の環境から Wikipedia に接続できず、実際の API では未テスト(デモデータでは確認済み)。
  エラーが出たらメッセージをそのまま教えてほしい。

ネット無しで試すなら:

    python3 wikipack.py demo cache && python3 wikipack.py build cache out && cp WIKI.FNT out/

(demo-pack/ にはビルド済みの辞書が2つ入っている。両方を送ると「辞書の選択」画面を試せる)

## 全記事を入れる(.zim から)

Kiwix が配布している Wikipedia の ZIM ファイル(日本語版の「テキストのみ(nopic)」版。
`wikipedia_ja_all_nopic_日付.zim` のような名前。Kiwix のライブラリで入手)から直接パックを作れる。
ネット接続は不要で、API を何万回も叩かずに済む。

    pip install libzim lxml pykakasi
    python3 wikipack.py zim wikipedia_ja_all_nopic.zim out_test --limit 2000 --jobs 4   # まず試運転
    python3 wikipack.py zim wikipedia_ja_all_nopic.zim out --jobs 8 --chunk-mb 64        # 本番
    cp WIKI.FNT out/ && python3 wikipack.py verify out

- 記事ごとに「HTML を読む → 本文と記事間リンクを取り出す → 圧縮 → 書き込み」と流すので、全文をメモリに溜めない。
  `--jobs` で並列にできる(CPU コア数くらい。メモリが足りなければ減らす)。
- 捨てるもの: 表(情報ボックス含む)、脚注・出典・関連項目・外部リンク・参考文献の節、画像。
  リンクは「同じ ZIM 内の別記事」へのものだけ残り、リダイレクト(例: ねこ → ネコ)も解決される。
- `--limit N` は先頭の N 記事だけ作る。**容量と時間の見積もりに使う**:
  出力サイズ ÷ N × 全記事数(日本語版は約150万)、経過時間も同様。
- `--lead-only` で導入部だけに絞ると、サイズも時間も大幅に減る。`--max-chars`(既定2万文字)で1記事の長さを制限。
- `--no-kakasi` は読みの自動推定(pykakasi)を省いて速くするが、導入文に読みが無い記事は検索に出なくなる。
- cp932 のうちフォントに字形がある文字だけを使う。〜→～、é→e、①→1、㈱→(株) のように置き換え、
  置き換えられない文字(絵文字、旧字の一部など)は ? になる。
- 本文ファイルは最大100個(D00-D99)なので、`--chunk-mb` × 100 が全体の大きさ以上になるようにする。
  索引(WIKI.IDX)は1記事あたり約112バイト(150万件で約170MB)。

未確認(私の環境に本物の ZIM がなく、実際の Wikipedia の ZIM では試していない):

1. ZIM の HTML の細かい構造(表の外の本文が取りこぼされないか、記事内リンクの書き方など)。
   小さな試験用 ZIM(`python3 test/make_test_zim.py test.zim`)で、本文・見出し・入れ子の箇条書き・
   脚注の除去・リダイレクト・リンク解決・複数ファイルへの分割・並列処理が動くことは確認した。
   本物では `--limit 2000` の出力を `lookup` / `show` で見て、おかしい所があれば教えてほしい。
2. 全件(約150万)での所要時間とメモリ使用量。
3. 電子辞書側の空き容量、SD カードの対応容量、巨大なファイルを `send` できるか、アプリが読めるか。
   まず小さいパック(数千件)で実機に入れ、段階的に増やすのが安全。

## 2. アプリを入れる(libexword)

ビルド済みの `prebuilt/ja/XWIKI` をそのまま使える。自分でビルドする場合は
devkitSH4 を入れて `make`(または GitHub Actions)。出力は build/ja/XWIKI。

    mkdir -p ~/.local/share/exword/ja && cp -r prebuilt/ja/XWIKI ~/.local/share/exword/ja/

電子辞書を「ライブラリー」→「通信」で接続待ち受けにして PC につなぎ(SD カードを使うなら、差した状態で)、libexword で:

    connect
    dict reset <名前>        # 追加コンテンツが全部消える。初回だけ。表示された key を控える
    dict auth <名前> <key>
    dict install XWIKI
    send WIKI.FNT            # 日本語フォント。1つあればよい(どの保存場所でも可)
    send WIKI.IDX
    send WIKI.NFO            # 辞書の表示名(無くても動く)
    send WIKI.D00            # D01, D02... があれば全部
    send WIKI.I00            # I01... があれば全部

### 保存場所(内蔵メモリ / SD カード)を選ぶ

libexword の `setpath` で送り先の保存場所を切り替える(ソースで確認した書式):

    setpath mem:///          # 内蔵メモリのルート
    setpath sd:///           # SD カードのルート
    capacity                 # 今の保存場所の容量を表示
    list                     # 今の保存場所のファイル一覧

- 送り先を `setpath` で決めてから `send` する。辞書データは内蔵・SD のどちらに置いてもよい。
- `dict install`(アプリ本体)も、今いる保存場所に対して働く。アプリは内蔵に入れたまま、データだけ SD に置く使い方でよい。
- ビューアは起動時に内蔵と SD の両方から `*.IDX` を探して一覧にする。同じ名前の辞書が両方にあれば別々に並ぶ。

## 辞書が複数あるとき(選択画面)

- 見つかった辞書が1つなら、そのまま検索画面が開く。
- 2つ以上なら「辞書の選択」画面が出る。`[内蔵]` / `[SD]` の印と表示名、ファイル名が並ぶ。
  ↑↓で選び、Enter で開く。Back で終了。
- 検索画面で Back を押すと、この選択画面に戻る(辞書が2つ以上あるとき)。
- 表示名は `<名前>.NFO`(1行目)。無ければファイル名がそのまま出る。
  送信済みの辞書に後から名前を付けるだけなら、辞書を作り直さず NFO だけ作って送る:

      python3 wikipack.py nfo . --name WIKI --title "日本語Wikipedia"
      (電子辞書で setpath を送り先にして)  send WIKI.NFO
- 日本語フォント(`*.FNT`)は、最初に見つかったものを全辞書で共用する。

## 3. 使い方

検索画面: ローマ字でよみを入力(かな変換される)。↑↓で候補、Enter で開く。
何も入力しない状態では全記事が五十音順に並ぶ。

    英字キー   入力(例 neko → ねこ)      記号キー   長音(ー)
    BackSpace  1文字消す                   SHIFT      かな / ABC 切り替え(英字の題名用)
    Back       終了

記事画面:

    ↑↓ 1行スクロール   PageUp/Down 1画面   ←→ リンクの選択
    Enter 選択中のリンクを開く   Back 前の記事へ(履歴が空なら検索へ)   F1 検索へ

リンクは、記事本文中に、同じパックに入っている別記事の題名が出てくる所に自動で付く。

## 起動して真っ白のままのとき

アプリは起動に失敗すると、英数字だけで原因を表示する(日本語フォントが読めない状況でも読めるように)。
画面に出る項目と見方:

    step:        どこで失敗したか(no *.FNT font file / no *.IDX dictionary found / alloc ...)
    drv / id:    起動したドライブ(0=本体, 1=SD)とアプリID
    font_read:   フォント(*.FNT)から読めたバイト数(-1 = 開けていない)。必要量は font_need(256784 など)
    open attempts:  開こうとしたパスと sys_open の戻り値(-1 は失敗)。どのパスも -1 なら送信先が違う

この表示を写真か書き写しで教えてもらえれば、置き場所やファイル名の扱いを合わせられる。
(大文字のファイル名で開けなければ小文字名も自動で試す。読み込みは 16KB ずつに分けている)

起動すると最初に "EXWIKI build 5" と、1〜4 の進行表示(4 は見つかった辞書の数)が出る。これが出なければ古いビルドが動いている
(アプリを入れ直す: `dict remove XWIKI` のあと `dict install XWIKI`)。
途中で止まった場合は、最後に表示された番号を教えてほしい。
記事を開くときは画面下に "loading..." が出る。これが出たまま動かなければ、記事の展開か画像の表示で止まっている。

## 4. 実機で確かめてほしい点(未確認)

1. send のファイル名と置き場所: Gnuboy EX の説明では send したファイルはドライブのルートに置かれる。
   ビューアは内蔵・SD それぞれの、ルート / `<APPID>\_USER\` / `<APPID>\` から `*.IDX` を探す。
   起動時に「no *.IDX dictionary found」と出たら、ここを疑う(画面に探した場所と見つかった数が出る)。
   SD は `\\crd0\`、内蔵は `\\drv0\` という名前で探しているが、SD 側の名前は実機で未確認。
2. send が WIKI.D00 のような拡張子・複数ファイル・数MBの大きさを受け付けるか。
   (Gnuboy の ROM は拡張子 .gbc に変える必要があった、という記述がある)
3. 内蔵メモリの空き容量(`capacity` で確認できる)。足りなければ SD(`setpath sd:///`)。
4. キー配置: SHIFT / 記号 / F1 / PageUp・Down の反応。
5. 大きなパックでの検索・表示の速さ(検索は二分探索で、1回あたり十数回のファイル読み込み)。
6. 画面描画の速さ(1画面ぶんの再描画に数百ms かかる可能性)。

## ライセンス・出典

- ひな形と libdataplus 周り: brain-hackers/exword-template (GPL-2.0、COPYING 参照)。本プロジェクトも GPL-2.0
- src/tinflate.c, tinf.h: tinf (zlib License、src/LICENSE.tinf)
- src/picojpeg.c, picojpeg.h: picojpeg (パブリックドメイン)。圧縮展開の未定義動作1か所を修正
- demo-pack/WIKI.FNT: Noto Sans CJK JP(SIL OFL)と DejaVu Sans Mono から生成
- Wikipedia の本文・画像は各記事のライセンス(CC BY-SA 等)に従い、再配布時は出典表記が必要
