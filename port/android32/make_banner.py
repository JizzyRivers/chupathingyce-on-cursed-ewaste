"""make_banner.py: the Android TV launcher's banner (320x180 dp, drawn at xhdpi, 640x360): the launcher icon's
helmet on its red, with the app's name (a television's launcher shows only the banner).

    python port/android32/make_banner.py        (needs Pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

RES = Path(__file__).resolve().parent / "app" / "app" / "src" / "main" / "res"
RED = (0xA3, 0x20, 0x1C)
FONT = "C:/Windows/Fonts/segoeuib.ttf"


def main():
    width, height = 640, 360
    image = Image.new("RGB", (width, height), RED)
    # a darker band along the bottom edge, so the name reads on any red
    shade = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(shade)
    for y in range(height):
        alpha = int(110 * (y / height) ** 2)
        draw.line([(0, y), (width, y)], fill=(0, 0, 0, alpha))
    image = Image.alpha_composite(image.convert("RGBA"), shade)
    helmet = Image.open(RES / "mipmap-xxxhdpi" / "ic_launcher_foreground.png").convert("RGBA")
    # (the adaptive icon's foreground has a safe margin: cropped to the helmet itself)
    helmet = helmet.crop(helmet.getbbox())
    scale = 210 / max(helmet.size)
    helmet = helmet.resize((round(helmet.width * scale), round(helmet.height * scale)), Image.LANCZOS)
    image.alpha_composite(helmet, (36, (height - helmet.height) // 2))
    draw = ImageDraw.Draw(image)
    left, room = 262, width - 262 - 28

    def fitted(text, size):
        # (the largest size at which the text fits the room beside the helmet)
        while True:
            font = ImageFont.truetype(FONT, size)
            if draw.textlength(text, font=font) <= room or size <= 12:
                return font
            size -= 1

    big = fitted("Chupathingy", 70)
    small = fitted("32-bit Android", 38)
    draw.text((left, 112), "Chupathingy", font=big, fill=(255, 255, 255))
    draw.text((left + 3, 200), "32-bit Android", font=small, fill=(255, 224, 214))
    out = RES / "drawable-xhdpi" / "banner.png"
    image.convert("RGB").save(out, optimize=True)
    print(out, out.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
