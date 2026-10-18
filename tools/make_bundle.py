#!/usr/bin/env python3
"""Package a release from firmware/bin/ and z2m/.

dist/release/   flat release assets, uploaded by .github/workflows/release.yml:
  z2m_ota_index.json             OTA index with absolute release URLs. Point z2m at
                                 .../releases/latest/download/z2m_ota_index.json and every later
                                 release shows up as an ordinary "update available"
  zy-m100-zrd.mjs                converter: stock definition + OTA, and the converted device
  *.zigbee                       the two OTA images
  zy-m100-zrd-z2m-v<ver>.zip     offline bundle: the dist/z2m/ tree below
  SHA256SUMS

dist/z2m/       drop-in tree for the z2m data directory (offline install, local index)

Both indexes pin each image by SHA-512 and restrict it to exactly one device type. The first-stage
image matters most: its manufacturer code / image type (0x1141 / 0xD3A3) are shared by every stock
Tuya MCU module, so without the manufacturerName filter z2m would offer it to *any* Tuya TS0601
in the network, and those devices would stop working.

usage: make_bundle.py [--repo owner/name] [--tag vX.Y.Z]
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, 'dist')
CONVERTERS = ('zy-m100-zrd.mjs',)


def header(path):
    d = open(path, 'rb').read(56)
    manuf, itype, ver = struct.unpack_from('<HHI', d, 10)
    return manuf, itype, ver


def index_entry(img, url):
    manuf, itype, ver = header(img)
    entry = {
        'url': url,
        'sha512': hashlib.sha512(open(img, 'rb').read()).hexdigest(),
        'fileVersion': ver,
        'manufacturerCode': manuf,
        'imageType': itype,
        'fileSize': os.path.getsize(img),
    }
    if (manuf, itype) == (0x1141, 0xD3A3):
        # stock Tuya module -> custom firmware: this exact device only
        entry.update({'modelId': 'TS0601', 'manufacturerName': ['_TZE204_7gclukjs'], 'maxFileVersion': 0xFF})
    else:
        entry.update({'modelId': 'ZY-M100-ZRD_1'})
    return entry


def write_json(path, obj):
    with open(path, 'w') as fh:
        json.dump(obj, fh, indent=2)
        fh.write('\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default='jkaberg/M100_24Ghz_radar')
    ap.add_argument('--tag', help='release tag, default v<firmware version>')
    a = ap.parse_args()

    # one firmware version per release, packaged with the stock ids and with ours
    from_tuya = sorted(glob.glob(os.path.join(ROOT, 'firmware', 'bin', '1141-d3a3-*-from-tuya.zigbee')))
    custom = sorted(glob.glob(os.path.join(ROOT, 'firmware', 'bin', '6565-5244-*.zigbee')))
    if len(from_tuya) != 1 or len(custom) != 1:
        raise SystemExit('expected exactly one image of each kind in firmware/bin (run firmware/build.sh)')
    images = from_tuya + custom
    version = re.search(r'_V(\d+)\.(\d+)\.(\d+)', os.path.basename(custom[0])).groups()
    tag = a.tag or 'v%d.%d.%d' % tuple(int(x) for x in version)

    shutil.rmtree(DIST, ignore_errors=True)
    z2m = os.path.join(DIST, 'z2m')
    rel = os.path.join(DIST, 'release')
    for d in (os.path.join(z2m, 'ota'), os.path.join(z2m, 'external_converters'), rel):
        os.makedirs(d)

    for f in CONVERTERS:
        shutil.copy(os.path.join(ROOT, 'z2m', f), os.path.join(z2m, 'external_converters', f))
        shutil.copy(os.path.join(ROOT, 'z2m', f), os.path.join(rel, f))

    local_index, remote_index = [], []
    base = f'https://github.com/{a.repo}/releases/download/{tag}/'
    for img in images:
        name = os.path.basename(img)
        shutil.copy(img, os.path.join(z2m, 'ota', name))
        shutil.copy(img, os.path.join(rel, name))
        local_index.append(index_entry(img, 'ota/' + name))     # relative to the z2m data directory
        remote_index.append(index_entry(img, base + name))
        manuf, itype, ver = header(img)
        print(f'{name}: manuf 0x{manuf:04x} type 0x{itype:04x} version 0x{ver:08x}')

    write_json(os.path.join(z2m, 'ota', 'local_ota_index.json'), local_index)
    write_json(os.path.join(rel, 'z2m_ota_index.json'), remote_index)

    zname = f'zy-m100-zrd-z2m-{tag}.zip'
    with zipfile.ZipFile(os.path.join(rel, zname), 'w', zipfile.ZIP_DEFLATED) as z:
        for d, _, files in os.walk(z2m):
            for f in sorted(files):
                full = os.path.join(d, f)
                z.write(full, os.path.relpath(full, z2m))

    with open(os.path.join(rel, 'SHA256SUMS'), 'w') as fh:
        for f in sorted(os.listdir(rel)):
            if f != 'SHA256SUMS':
                fh.write(f'{hashlib.sha256(open(os.path.join(rel, f), "rb").read()).hexdigest()}  {f}\n')

    print(f'release {tag} for {a.repo}: {sorted(os.listdir(rel))}')


if __name__ == '__main__':
    main()
