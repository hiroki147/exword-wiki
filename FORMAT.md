# パック形式 (XWK1)

文字コードは cp932(Shift_JIS)。数値はすべてリトルエンディアン(実機 SH-4 はビッグエンディアンなので、
読み出しは1バイトずつ組み立てる)。ファイル名は 8.3: WIKI.IDX / WIKI.D00-D99 / WIKI.I00-I99 / WIKI.FNT。

## WIKI.IDX

ヘッダ 32 バイト:

    0  "XWK1"     4  version(u16)=1    6  encoding(u8) 0=utf-8 1=cp932 2=euc-jp    7  flags(u8)
    8  n_articles(u32)    12 n_keys(u32)    16 n_dfiles(u16)    18 n_ifiles(u16)
    20 key_size(u16)=32   22 title_size(u16)=48   24..31 予約

続けて:

1. キー表: n_keys 個 × (key_size + 4) バイト。key = 検索キー(NUL 埋め)、続く u32 = 記事ID。
   キーでバイト順(符号なし)に整列済み。検索キーはよみのひらがな(カタカナは変換済み)か、
   英数字題名の小文字。前方一致検索は下限二分探索で行う。
2. 記事表: n_articles 個 × (28 + title_size) バイト。ID = 0 始まりの位置。
   u16 dfile, u16 ifile, u32 offset, u32 clen, u32 rlen, u32 ioff, u32 ilen, u16 iw, u16 ih, title[title_size]

## WIKI.Dnn(本文)

記事ごとに raw deflate(zlib ヘッダ無し)で圧縮した本文を連結。offset/clen で切り出し、展開後は rlen バイト。

本文は \n 区切りのテキストで、制御文字(0x01-0x04)だけが特別:

    0x01 <レベル数字> 見出し行の先頭
    0x02 <10進の記事ID> 0x03 <表示文字> 0x04     リンク

## WIKI.Inn(画像)

記事ごとに JPEG(baseline、非プログレッシブ)を連結。ioff/ilen で切り出す。ilen=0 は画像なし。

## WIKI.FNT(フォント)

    0  "XFN1"   4 fw(u8)=16   5 fh(u8)=16   6 hw(u8)=8   7 予約   8 rows(u16)=84   10 cells(u16)=94   12..15 予約
    16 半角: 256 文字 × ((hw+7)/8 × fh) バイト。添字は cp932 の1バイトコード
    続き 全角: 84 × 94 文字 × ((fw+7)/8 × fh) バイト。添字は JIS 区点 (区-1)*94+(点-1)

ビットマップは1行ごとに MSB が左端。Shift_JIS の2バイトは次の式で区点にする(core.c の sjis_to_rc):
    c1>=0xE0 なら c1-=0x40; c2>=0x80 なら c2-=1;
    c2>=0x9E: 区=(c1-0x70)*2-0x20, 点=c2-0x7D-0x20   /   それ以外: 区=(c1-0x70)*2-1-0x20, 点=c2-0x1F-0x20
