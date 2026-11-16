"""Draws the F1 menu at the SNES's native 256x224 with the art taken from
the ROM, to judge the look before writing it in C."""
import sys
from PIL import Image, ImageDraw

A = sys.argv[1]
frame = Image.open(f"{A}/frame.png").convert("RGBA")
font = Image.open(f"{A}/font.png")
cursor = Image.open(f"{A}/cursor.png")

# the game's own text palettes: 1 outline, 2 shade, 3 face
WHITE = {1: (0, 0, 0), 2: (222, 222, 222), 3: (255, 255, 255)}
RED = {1: (0, 0, 0), 2: (139, 0, 0), 3: (230, 0, 0)}
DIM = {1: (0, 0, 0), 2: (90, 110, 140), 3: (120, 145, 175)}
GOLD = {1: (0, 0, 0), 2: (180, 120, 20), 3: (250, 200, 60)}
ACCENT = {"Á": "A", "É": "E", "Í": "I", "Ó": "O", "Ú": "U", "Ñ": "N"}


def text(img, x, y, s, pal):
    px = img.load()
    for ch in s.upper():
        base = ACCENT.get(ch, ch)
        code = ord(base) if 0x20 <= ord(base) < 0x60 else ord("?")
        gx, gy = (code - 0x20) % 16 * 8, (code - 0x20) // 16 * 8
        for yy in range(8):
            for xx in range(8):
                v = font.getpixel((gx + xx, gy + yy))
                if v:
                    px[x + xx, y + yy] = pal[v] + (255,)
        if ch in ACCENT:                       # accents and tilde drawn above
            d = ImageDraw.Draw(img)
            c = pal[3]
            if ch == "Ñ":
                d.line([(x + 2, y - 2), (x + 3, y - 3), (x + 4, y - 2), (x + 5, y - 3)], fill=c)
            else:
                d.line([(x + 4, y - 3), (x + 5, y - 4)], fill=c)
        x += 8
    return x


def menu(sel, items, right_title, rows):
    img = frame.copy()
    shade = Image.new("RGBA", (196, 150), (10, 20, 40, 150))   # dim the marble behind the text
    img.alpha_composite(shade, (30, 52))
    text(img, 128 - len("LA MANSIÓN") * 4, 30, "LA MANSIÓN", RED)
    for i, it in enumerate(items):
        y = 58 + i * 13
        text(img, 60, y, it, GOLD if i == sel else DIM)
    img.alpha_composite(cursor, (40, 58 + sel * 13 - (cursor.height - 8) // 2))
    text(img, 128 - len(right_title) * 4, 182, right_title, WHITE)
    return img


items = ["CONTINUAR", "IR A NIVEL", "TRUCOS", "CÓDIGOS", "MAPA", "PROGRESO", "BOSS RUSH", "GUARDAR", "OPCIONES"]
a = menu(2, items, "HALL OF STAIRS 12:47", [])

# the cheats page
b = frame.copy()
b.alpha_composite(Image.new("RGBA", (196, 150), (10, 20, 40, 150)), (30, 52))
text(b, 128 - 6 * 4, 30, "TRUCOS", RED)
opts = [("MODO DIOS", "SI"), ("VIDAS INFINITAS", "SI"), ("FEZ SIN LIMITE", "SI"),
        ("ESPADA", "NO"), ("PELOTA DE GOLF", "NO"), ("SIN SLOWDOWN", "SI")]
for i, (k, v) in enumerate(opts):
    y = 62 + i * 18
    text(b, 52, y, k, GOLD if i == 0 else WHITE)
    text(b, 204 - len(v) * 8, y, v, WHITE if v == "SI" else DIM)
b.alpha_composite(cursor, (34, 62 - (cursor.height - 8) // 2))
text(b, 128 - 13 * 4, 182, "SIN REINICIAR", DIM)

sheet = Image.new("RGBA", (256 * 2 + 8, 224), (0, 0, 0, 255))
sheet.paste(a, (0, 0)); sheet.paste(b, (264, 0))
sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
sheet.save(sys.argv[2])
