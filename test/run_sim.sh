#!/bin/sh
# PC 上でビューアを動かして画面を PPM に書き出す(実機なしの動作確認)。
# 使い方: sh test/run_sim.sh <パックのディレクトリ> "<キー列>"   例: sh test/run_sim.sh demo-pack "ne,ENTER,RIGHT,ENTER"
set -e
cd "$(dirname "$0")/.."
sed 's/^int main(void)/int real_main(void)/' src/main.c > test/main_sim.c
sed -i 's#"core.h"#"../src/core.h"#; s#"img.h"#"../src/img.h"#; s#"strings.h"#"../src/strings.h"#; s#"libc/memmgr.h"#<libc/memmgr.h>#' test/main_sim.c
gcc -O1 -w -Itest/mock -include string.h -o /tmp/exwiki_sim test/sim_main.c test/main_sim.c src/core.c src/img.c src/picojpeg.c src/tinflate.c
mkdir -p /tmp/exwiki_frames && rm -f /tmp/exwiki_frames/*
/tmp/exwiki_sim "$1" /tmp/exwiki_frames "$2"
echo "画面: /tmp/exwiki_frames/f00.ppm ..."
