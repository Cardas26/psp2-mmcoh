#!/usr/bin/env python3

import argparse
import hashlib
import os
import re
import shutil
import struct
import sys
import zipfile
import zlib

LIBAPPLICATION_SHA256 = "3bd2246ca60d57e2fb144a0db226fae33664837cdee98d73abd9a35375eb5b87"
LIBS = ("libApplication.so", "libfmodex.so", "libfmodevent.so")
MANIFEST = "prepare_data.manifest"

VIDEO_RENAMES = {
    "Videos/C0S3_Magic Portal.mp4": "Videos/C0S3_Magic_Portal.mp4",
    "Videos/C0S4_Anwen Hidden.mp4": "Videos/C0S4_Anwen_Hidden.mp4",
    "Videos/C1S1_Stop Fight.mp4": "Videos/C1S1_Stop_Fight.mp4",
    "Videos/C4S4_Aidan_The_Demon.mp4": "Videos/C4S4_Aidan_the_demon.mp4",
    "Videos/Upsell_Video.mp4": "Videos/Upsell_video.mp4",
    "Videos/C2S1_Godric_Back.mp4": "Videos/C2S1_Godric_back.mp4",
    "Videos/C5S1_Silver_Cities.mp4": "Videos/C5S1_Silver_cities.mp4",
}


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def extract_member(zf, info, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with zf.open(info) as src, open(dest, "wb") as dst:
        shutil.copyfileobj(src, dst, 1 << 20)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def split_lines(text):
    return re.findall(r"[^\n]*\n|[^\n]+", text)


def sed_delete_lines(path, pattern):
    data = read_bytes(path)
    lines = data.split(b"\n")
    kept = [l for l in lines if not re.search(pattern, l)]
    if len(kept) != len(lines):
        write_bytes(path, b"\n".join(kept))
    return len(lines) - len(kept)


def sed_insert_after(path, anchor, line):
    data = read_bytes(path)
    if anchor not in data:
        return False
    write_bytes(path, data.replace(anchor, anchor + b"\n" + line, 1))
    return True


def fix_1(dlc):
    layouts = os.path.join(dlc, "res/menus/layouts")
    names = ("advpause.layout advpause_SD.layout advpause_iOS.layout "
             "setting_option.layout setting_option_SD.layout "
             "mppauseonline.layout mppauseonline_SD.layout titlemenu.layout "
             "titlemenu_SD.layout titlemenu_Amazon.layout titlemenu_iOS.layout "
             "help_option.layout help_option_SD.layout").split()
    for n in names:
        p = os.path.join(layouts, n)
        if os.path.isfile(p):
            data = read_bytes(p)
            new = data.replace(b'Value="DejaVuSans-10"', b'Value="basefont"')
            if new != data:
                write_bytes(p, new)


def fix_2_3(dlc):
    p = os.path.join(dlc, "res/menus/layouts/advpause_SD.layout")
    text = read_text(p)
    if 'Name="advpause/infos"' not in text:
        m = re.search(r"    </Window>\n</GUILayout>\s*\Z", text)
        if not m:
            fail("fix 2: unexpected ending in " + p)
        text = (text[:m.start()]
                + '        <Window Type="taharezlook/LineText" Name="advpause/infos" >\n'
                + '            <Property Name="FrameEnabled" Value="False" />\n'
                + '            <Property Name="HorzFormatting" Value="RightAligned" />\n'
                + '            <Property Name="UnifiedAreaRect" Value="{{0.912,0},{0.95,0},{0.914,0},{0.954,0}}" />\n'
                + '            <Property Name="BackgroundEnabled" Value="False" />\n'
                + '        </Window>\n'
                + text[m.start():])
        write_text(p, text)
    if 'Name="advpause/save__auto__text"' not in text:
        m = re.search(r'(        <Window Type="lbut" Name="advpause/save" >\n(?:.*\n)*?        </Window>\n)', text)
        if not m:
            fail("fix 3: advpause/save not found in " + p)
        block = m.group(1)
        block = (block[:-len("        </Window>\n")]
                 + '            <Window Type="taharezlook/LineText" Name="advpause/save__auto__text" >\n'
                 + '                <Property Name="FrameEnabled" Value="False" />\n'
                 + '                <Property Name="BackgroundEnabled" Value="False" />\n'
                 + '                <Property Name="Font" Value="basefont" />\n'
                 + '                <Property Name="UnifiedAreaRect" Value="{{0,0},{0,0},{0.01,0},{0.01,0}}" />\n'
                 + '            </Window>\n'
                 + "        </Window>\n")
        write_text(p, text[:m.start()] + block + text[m.end():])


def fix_4(dlc):
    p = os.path.join(dlc, "res/menus/imagesets/mppauseonline/mppauseonline.imageset")
    if b'Name="stat/infos_backg"' not in read_bytes(p):
        sed_insert_after(p, b'<Image Height="606" Name="stats_frame" Width="914" XPos="2" YPos="2"/>',
                         b'\t<Image Height="1" Name="stat/infos_backg" Width="1" XPos="2" YPos="2"/>')


def fix_5_6_9(dlc):
    menus = os.path.join(dlc, "res/menus")
    sed_delete_lines(os.path.join(menus, "layouts/battle_intro_SD.layout"),
                     rb'<Property Name="FontScale" Value="2" />')
    sed_delete_lines(os.path.join(menus, "looknfeel/summary.looknfeel"),
                     rb'<Property name="FontScale" value="[0-9.]*" />')
    sed_delete_lines(os.path.join(menus, "layouts/heroSelectMenu_1vs1_SD.layout"),
                     rb'<Property Name="FontScale" Value="0\.(8|68)" />')


def fix_7(dlc):
    menus = os.path.join(dlc, "res/menus")
    font = os.path.join(menus, "fonts/DejaVuSans-10.font")
    if not os.path.isfile(font):
        write_bytes(font, b'<Font Name="DejaVuSans-10" Filename="basefont.imageset" Type="Pixmap" '
                          b'NativeHorzRes="1920" NativeVertRes="1080" AutoScaled="true" />\n')
    scheme = os.path.join(menus, "schemes/taharezlook.scheme")
    if b'Name="DejaVuSans-10"' not in read_bytes(scheme):
        sed_insert_after(scheme, b'<Font Name="basefont" Filename="basefont.font" />',
                         b'\t<Font Name="DejaVuSans-10" Filename="DejaVuSans-10.font" />')


def fix_8(dlc):
    p = os.path.join(dlc, "res/menus/schemes/artifacts_adventure.scheme")
    if b'Name="widget_artifacts"' not in read_bytes(p):
        sed_insert_after(p, b'<Imageset Name="widget_artifacts_adventure" Filename="widget_artifacts_adventure.imageset" />',
                         b'    <Imageset Name="widget_artifacts" Filename="widget_artifacts.imageset" />')


def fix_10(dlc):
    menus = os.path.join(dlc, "res/menus")
    lnf_dir = os.path.join(menus, "looknfeel")
    valid = set()
    for n in sorted(os.listdir(lnf_dir)):
        if not n.endswith(".looknfeel"):
            continue
        cur = None
        for line in split_lines(read_text(os.path.join(lnf_dir, n))):
            m = re.search(r'<WidgetLook name="([^"]*)"', line)
            if m:
                cur = m.group(1)
            if cur and re.search(r'<PropertyDefinition[^>]*[Nn]ame="FontScale"', line):
                valid.add(cur)
    type_map = {}
    scheme_dir = os.path.join(menus, "schemes")
    for n in sorted(os.listdir(scheme_dir)):
        if n.endswith(".scheme"):
            for m in re.finditer(r'<FalagardMapping[^>]*WindowType="([^"]*)"[^>]*LookNFeel="([^"]*)"',
                                 read_text(os.path.join(scheme_dir, n))):
                type_map[m.group(1)] = m.group(2)

    def sweep(directory, ext, type_re, prop_re):
        for n in sorted(os.listdir(directory)):
            if not n.endswith(ext):
                continue
            p = os.path.join(directory, n)
            lines = split_lines(read_text(p))
            cur, drop = None, set()
            for i, line in enumerate(lines):
                m = re.search(type_re, line)
                if m:
                    cur = m.group(1)
                if re.search(prop_re, line) and type_map.get(cur, cur) not in valid:
                    drop.add(i)
            if drop:
                write_text(p, "".join(l for i, l in enumerate(lines) if i not in drop))

    sweep(os.path.join(menus, "layouts"), ".layout", r'<Window Type="([^"]*)"', r'FontScale.*Property|Property.*FontScale')
    sweep(lnf_dir, ".looknfeel", r'<Child type\s*="([^"]*)"', r'<Property name="FontScale" value=')


def fix_12(dlc):
    sed_delete_lines(os.path.join(dlc, "res/menus/layouts/credits.layout"),
                     rb'<Property Name="HorzFlip" Value="" />')


def fix_13(dlc):
    p = os.path.join(dlc, "defaultT.png")
    if os.path.isfile(p):
        return
    w = h = 16
    raw = b"".join(b"\x00" + b"\x00\x00\x00\x00" * w for _ in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))
    write_bytes(p, b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9))
                + chunk(b"IEND", b""))


def write_manifest(out):
    rows = []
    for base, _, files in os.walk(out):
        for n in files:
            full = os.path.join(base, n)
            rel = os.path.relpath(full, out).replace(os.sep, "/")
            if rel != MANIFEST:
                rows.append("%d %s" % (os.path.getsize(full), rel))
    rows.sort(key=lambda r: r.split(" ", 1)[1])
    write_text(os.path.join(out, MANIFEST), "\n".join(rows) + "\n")
    return len(rows)


def prepare(apk, obb, out, force):
    if os.path.exists(out) and os.listdir(out):
        fail("%s exists and is not empty; pick a new folder" % out)
    for path in (apk, obb):
        if not zipfile.is_zipfile(path):
            fail("%s is not a zip archive" % path)
    with zipfile.ZipFile(apk) as z:
        if "lib/armeabi/libApplication.so" not in z.namelist():
            fail("%s is not the game's APK (no lib/armeabi/libApplication.so); "
                 "the APK comes first, then the OBB" % apk)
        so = z.read("lib/armeabi/libApplication.so")
        if sha256_bytes(so) != LIBAPPLICATION_SHA256:
            msg = ("this APK is not the Android v1.4 release the port supports "
                   "(libApplication.so sha256 %s)" % sha256_bytes(so))
            if not force:
                fail(msg + "; --force to continue anyway")
            print("warning: " + msg, file=sys.stderr)
        os.makedirs(out, exist_ok=True)
        for lib in LIBS:
            write_bytes(os.path.join(out, lib), z.read("lib/armeabi/" + lib))
        for info in z.infolist():
            if info.filename.startswith("assets/") and not info.is_dir():
                extract_member(z, info, os.path.join(out, info.filename))
    shutil.copyfile(apk, os.path.join(out, "ClashOfHeroes.apk"))
    print("APK: libraries, assets/ and ClashOfHeroes.apk written")

    dlc = os.path.join(out, "DLC")
    with zipfile.ZipFile(obb) as z:
        members = [i for i in z.infolist() if not i.is_dir()]
        if not any(i.filename.startswith("res/menus/") for i in members):
            fail("%s does not look like the game's OBB (no res/menus/)" % obb)
        for n, info in enumerate(members, 1):
            name = VIDEO_RENAMES.get(info.filename, info.filename)
            if any(part != part.rstrip(" .") for part in name.split("/")):
                print("skipped %r: not a valid card filename" % name)
                continue
            extract_member(z, info, os.path.join(dlc, name))
            if n % 500 == 0 or n == len(members):
                print("OBB: %d/%d files" % (n, len(members)))

    fix_1(dlc)
    fix_2_3(dlc)
    fix_4(dlc)
    fix_5_6_9(dlc)
    fix_7(dlc)
    fix_8(dlc)
    fix_10(dlc)
    fix_12(dlc)
    fix_13(dlc)
    print("data fixes applied")
    count = write_manifest(out)
    print("done: %d files in %s" % (count, out))
    print("copy the folder's contents to ux0:data/mmcoh/, then run --verify on the copy")


def verify(root):
    mpath = os.path.join(root, MANIFEST)
    if not os.path.isfile(mpath):
        fail("no %s in %s - is this the copied mmcoh folder?" % (MANIFEST, root))
    bad = 0
    rows = read_text(mpath).splitlines()
    for row in rows:
        size, rel = row.split(" ", 1)
        p = os.path.join(root, *rel.split("/"))
        if not os.path.isfile(p):
            print("missing: " + rel)
            bad += 1
        elif os.path.getsize(p) != int(size):
            print("wrong size: %s (%d, expected %s)" % (rel, os.path.getsize(p), size))
            bad += 1
    if bad:
        fail("%d of %d files are missing or damaged; copy them again" % (bad, len(rows)))
    print("all %d files present with the right sizes" % len(rows))


def main():
    ap = argparse.ArgumentParser(description="Build ux0:data/mmcoh/ from the Android v1.4 APK and OBB.")
    ap.add_argument("--verify", metavar="MMCOH_DIR", help="check a copied folder against its manifest")
    ap.add_argument("--force", action="store_true", help="continue with an unrecognised APK")
    ap.add_argument("apk", nargs="?")
    ap.add_argument("obb", nargs="?")
    ap.add_argument("out", nargs="?")
    a = ap.parse_args()
    if a.verify:
        verify(a.verify)
    elif a.apk and a.obb and a.out:
        prepare(a.apk, a.obb, a.out, a.force)
    else:
        ap.print_usage()
        sys.exit(2)


if __name__ == "__main__":
    main()
