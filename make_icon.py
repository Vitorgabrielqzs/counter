"""Gera Counter.ico: quadrado arredondado escuro com "+1" branco em negrito (Segoe UI Variable).
Uso: python make_icon.py Counter.ico previa.png  (requer Pillow: pip install pillow)"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\SegUIVar.ttf"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
OUT = sys.argv[1]
PREVIEW = sys.argv[2]


def font(px):
    f = ImageFont.truetype(FONT, px)
    f.set_variation_by_name(b"Bold Display")
    return f


def draw(size):
    # Desenha 8x maior e reduz: bordas suaves sem perder nitidez
    s = 8
    W = size * s
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Fundo: quadrado arredondado, cinza-grafite descendo para quase preto, fio claro na borda
    margin = round(W * 0.06)
    radius = round(W * 0.22)
    box = [margin, margin, W - margin - 1, W - margin - 1]
    grad = Image.new("RGBA", (W, W))
    gd = ImageDraw.Draw(grad)
    for y in range(W):
        t = y / W
        c = round(0x3A * (1 - t) + 0x14 * t)
        gd.line([(0, y), (W, y)], fill=(c, c, c + 2, 255))
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)
    img.paste(grad, (0, 0), mask)
    d.rounded_rectangle(box, radius, outline=(255, 255, 255, 46), width=max(s, round(W * 0.012)))

    # "+1" branco, centralizado pela área pintada
    text = "+1"
    px = round(W * 0.52)
    f = font(px)
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    x = (W - (r - l)) / 2 - l
    y = (W - (b - t)) / 2 - t
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255))

    return img.resize((size, size), Image.LANCZOS)


images = [draw(n) for n in SIZES]
images[-1].save(OUT, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])

# Prévia: todos os tamanhos lado a lado, em fundo escuro e claro
w = sum(SIZES) + 10 * len(SIZES) + 10
prev = Image.new("RGBA", (w, 2 * 276), (32, 32, 32, 255))
prev.paste((243, 243, 243, 255), (0, 276, w, 552))
x = 10
for n, im in zip(SIZES, images):
    prev.alpha_composite(im, (x, 10))
    prev.alpha_composite(im, (x, 286))
    x += n + 10
prev.save(PREVIEW)
