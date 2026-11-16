"""Pulls the menu art out of the player's own ROM at first launch:
the stone-frame background, the 8x8 font and the pointing-hand cursor.
Nothing extracted here is ever committed or shipped.

usage: extract_assets.py ROM OUTDIR"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from harness import Snes, START, DOWN
from PIL import Image

FONT_TILE = 0x200          # 2bpp tile number of ASCII 0 in VRAM (password screen)


def until_visible(s, max_frames=600):
    for _ in range(max_frames // 10):    # first let the old screen fade out
        if not s.frame.getbbox():
            break
        s.run(10)
    for _ in range(max_frames // 10):
        s.run(10)
        if s.frame.getbbox():
            s.run(40)
            return
    raise RuntimeError("screen never faded in")


def transparent(img, key=(0, 0, 0)):
    img = img.convert("RGBA")
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            if px[x, y][:3] == key:
                px[x, y] = (0, 0, 0, 0)
    return img


def main(rom, out):
    os.makedirs(out, exist_ok=True)
    s = Snes(rom)
    s.run(60 * 12)                       # legal screens and Ocean logo
    s.tap(START, 180)                    # skip the intro
    s.tap(START, 90)                     # title menu
    title = s.save()

    s.layers({5}); s.run(2)              # the cursor sprite next to START GAME
    box = s.frame.getbbox()
    transparent(s.frame.crop(box)).save(f"{out}/cursor.png")

    s.load(title); s.layers({1, 2, 3, 4, 5})
    s.tap(DOWN, 30)
    s.tap(START, 30); until_visible(s)   # password screen
    s.layers({1}); s.run(2)
    s.frame.save(f"{out}/frame.png")     # stone frame with vines (BG1)

    vram = s.mem(3)
    sheet = Image.new("L", (16 * 8, 4 * 8))   # ASCII 0x20-0x5F, values 0..3
    px = sheet.load()
    for ch in range(0x20, 0x60):
        t = (FONT_TILE + ch) * 16
        b = vram[t:t + 16]
        cx, cy = (ch - 0x20) % 16 * 8, (ch - 0x20) // 16 * 8
        for y in range(8):
            for x in range(8):
                px[cx + x, cy + y] = (b[y * 2] >> (7 - x) & 1) | (b[y * 2 + 1] >> (7 - x) & 1) << 1
    sheet.save(f"{out}/font.png")
    print("extracted to", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
