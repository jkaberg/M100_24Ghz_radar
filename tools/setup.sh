#!/bin/sh
# Fetch everything that isn't ours to ship into .deps/ and check it by sha256:
#   .deps/tc32/            Telink tc32-gcc toolchain
#   .deps/tl_zigbee_sdk/   Telink Zigbee SDK v3.7.2.0 (unmodified)
#   .deps/dumps/           GD32 firmware of the radar MCU (Andrik45719/ZY-M100) and one stock
#                          ZT3L flash dump (slacky1965/tuya_thermostat_zrd), used by the tests
#   .deps/TlsrPgm.py       pvvx's SWS programmer, only for wired recovery
#   .venv/                 capstone + unicorn for tools/check_frames_gd32.py
set -e
cd "$(dirname "$0")/.."
D=.deps
mkdir -p $D/dl $D/dumps

fetch() { # url file sha256
    if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --status; then
        echo "fetch $1"
        curl -fsSL -o "$2" "$1"
    fi
    echo "$3  $2" | sha256sum -c --quiet
}

fetch https://shyboy.oss-cn-shenzhen.aliyuncs.com/readonly/tc32_gcc_v2.0.tar.bz2 \
      $D/dl/tc32_gcc_v2.0.tar.bz2 33b854be3e3db3dba4b4dacdda2cd4ea1c94dfd4d562864a095956de7991b430
[ -d $D/tc32 ] || tar xjf $D/dl/tc32_gcc_v2.0.tar.bz2 -C $D

# GitHub tag archive; if GitHub ever re-packs it the checksum changes - compare the tree instead
fetch https://github.com/telink-semi/telink_zigbee_sdk/archive/refs/tags/V3.7.2.0.zip \
      $D/dl/V3.7.2.0.zip 77ca35173fc9d6c4fc8a6f6a97bd601d66c7a607a5c89e3012c138736ae0c049
if [ ! -d $D/tl_zigbee_sdk ]; then
    python3 -I -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" $D/dl/V3.7.2.0.zip $D/dl
    mv $D/dl/telink_zigbee_sdk-3.7.2.0/tl_zigbee_sdk $D/tl_zigbee_sdk
    rm -rf $D/dl/telink_zigbee_sdk-3.7.2.0
fi

A=https://raw.githubusercontent.com/Andrik45719/ZY-M100/03770f2873119606d88cecb19a2432fca7c2817e
fetch $A/ZY-M100-24GV2_TZE204_7gclukjs.bin $D/dumps/ZY-M100-24GV2_TZE204_7gclukjs.bin \
      ea3e1b327e1370a2a2be7201a01611a78bb54f37efb0c9cac01547ee0eba6743
fetch $A/ZY-M100-24GV2_TZE204_7gclukjs-TargetDistance_disable.bin \
      $D/dumps/ZY-M100-24GV2_TZE204_7gclukjs-TargetDistance_disable.bin \
      fd4833cd351e40e08c202e72a80196c4f241b25bab0b7133f39325dc4befd131
fetch $A/ZY-M100-24GV3_TZE204_ya4ft0w4.bin $D/dumps/ZY-M100-24GV3_TZE204_ya4ft0w4.bin \
      0823b58360741c8c36565016219832cf3e8fe8a03c3e09c815cccaab2a9c6600

S=https://raw.githubusercontent.com/slacky1965/tuya_thermostat_zrd/9d1a784a9ee464888961aba39217dee8581078a2
fetch $S/bin/tuya_original/tuya_thermostat_orig_aoclfnxz.bin $D/dumps/stock_zt3l_aoclfnxz.bin \
      86cdb7ce99231fb129989b45a5e814eff76af241777cb7a21023f6c3fa736d99

[ -f $D/TlsrPgm.py ] || curl -fsSL -o $D/TlsrPgm.py https://raw.githubusercontent.com/pvvx/TLSRPGM/refs/heads/main/TlsrPgm.py

if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
    .venv/bin/pip install -q capstone unicorn
fi

echo "deps ready in $D"
