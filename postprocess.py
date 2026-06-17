import json
from PIL import Image

ALPHA_THRESHOLD = 10   # min alpha to count a pixel as opaque
AIRBORNE_LIFT = 0.04   # foot must be this much higher than resting (as fraction of frame h) to count as airborne

def find_foot_y(img, rect):
    x, y, w, h = rect['x'], rect['y'], rect['w'], rect['h']
    pixels = img.load()
    for row in range(y + h - 1, y - 1, -1):
        for col in range(x, x + w):
            if pixels[col, row][3] > ALPHA_THRESHOLD:
                return (row - y) / h
    return None  # fully transparent frame

def process_variant(spritesheet_path, atlas_path):
    img = Image.open(spritesheet_path).convert('RGBA')
    with open(atlas_path) as f:
        atlas = json.load(f)

    foot_ys = [find_foot_y(img, fd['frame']) for fd in atlas['frames']]
    valid = [fy for fy in foot_ys if fy is not None]
    if not valid:
        return None, None

    resting = max(valid)
    airborne = [False if fy is None else (resting - fy) > AIRBORNE_LIFT for fy in foot_ys]
    return round(resting, 4), airborne

with open('animations.json') as f:
    data = json.load(f)

for anim_type, variants in data.items():
    for variant in variants:
        foot_y, airborne = process_variant(variant['spritesheet'], variant['atlas'])
        if foot_y is not None:
            variant['foot_y'] = foot_y
            variant['airborne_profile'] = airborne
        airborne_count = sum(airborne) if airborne else 0
        total = len(airborne) if airborne else 0
        print(f"  {variant['spritesheet']}: foot_y={foot_y}, airborne={airborne_count}/{total} frames")

with open('animations.json', 'w') as f:
    json.dump(data, f, indent=2)

print("Done.")
