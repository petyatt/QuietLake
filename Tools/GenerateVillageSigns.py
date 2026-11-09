"""Draw the painted village signs as textures (run with the usual python, Pillow needed).

    python Tools/GenerateVillageSigns.py

The plates are textured quads of the village (Source/QuietLake/LakeVillage.cpp) and are imported by
Tools/ImportVillageSigns.py. The engine's Text Render font has no Cyrillic, hence painted signs.
Output: Tools/GeneratedSigns/T_Sign_<Name>.png
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'GeneratedSigns')
FONTS = [r'C:\Windows\Fonts\arialbd.ttf', r'C:\Windows\Fonts\arial.ttf',
         r'C:\Windows\Fonts\calibrib.ttf']
FONT = next((f for f in FONTS if os.path.exists(f)), None)
if not FONT:
    raise RuntimeError('no Cyrillic font found in C:/Windows/Fonts')
STOCK, INK = (208, 194, 160), (48, 36, 24)
SIGNS = {
    'FishingShop': ('РЫБОЛОВНЫЙ МАГАЗИН', 1024, 220, 96),
    'Store': ('ЛАВКА', 512, 220, 110),
    'Market': ('РЫБНЫЙ РЫНОК', 1024, 220, 96),
    'Fish': ('РЫБА', 512, 220, 110),
    'Tackle': ('СНАСТИ', 512, 220, 100),
    'Village': ('ДЕРЕВНЯ ЗАВОДЬ', 1024, 220, 84),
}

os.makedirs(OUT, exist_ok=True)
for name, (text, w, h, size) in SIGNS.items():
    plate = Image.new('RGB', (w, h), STOCK)
    draw = ImageDraw.Draw(plate)
    # Shrink the font until the whole line fits the plate with a margin.
    while size > 10:
        font = ImageFont.truetype(FONT, size)
        box = draw.textbbox((0, 0), text, font=font)
        if box[2] - box[0] <= w - 70:
            break
        size -= 4
    box = draw.textbbox((0, 0), text, font=font)
    draw.text(((w - (box[2] - box[0])) / 2 - box[0], (h - (box[3] - box[1])) / 2 - box[1]), text, font=font, fill=INK)
    draw.rectangle([3, 3, w - 4, h - 4], outline=(92, 74, 52), width=6)
    # The plates show their back side to the viewer: mirror the image so the letters read right.
    plate = plate.transpose(Image.FLIP_LEFT_RIGHT)
    plate.save(os.path.join(OUT, 'T_Sign_%s.png' % name))
    print('sign', name, plate.size)
