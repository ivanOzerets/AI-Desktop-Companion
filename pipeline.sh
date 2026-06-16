#!/bin/bash

DOWNLOADS="/c/Users/Vanya/Downloads/animations"
BIRB_DIR="/c/Users/Vanya/Projects/John-Carter-Speedrun/Projects/birb"
TEXTUREPACKER="/c/Program Files/CodeAndWeb/TexturePacker/bin/TexturePacker.exe"
TMP_DIR="$BIRB_DIR/_pipeline_tmp"

mkdir -p "$TMP_DIR"

for mp4 in "$DOWNLOADS"/*.mp4; do
    filename=$(basename "$mp4" .mp4)

    # Normalize spaces to underscores
    normalized="${filename// /_}"

    # Split into type and variant number on the last underscore
    variant_raw="${normalized##*_}"
    type_raw="${normalized%_*}"
    variant_num=$((10#$variant_raw))

    # Apply type renames
    case "$type_raw" in
        "turn_around") type="turnaround" ;;
        "flying")      type="fly" ;;
        *)             type="$type_raw" ;;
    esac

    out_dir="$BIRB_DIR/animations/$type/$variant_num"

    if [ -f "$out_dir/spritesheet.png" ] && [ -f "$out_dir/spritesheet.json" ]; then
        echo "Skipping $type/$variant_num (already exists)"
        continue
    fi

    echo ""
    echo "=== $filename -> $type/$variant_num ==="

    frames_raw="$TMP_DIR/${type}_${variant_num}_raw"
    frames_nobg="$TMP_DIR/${type}_${variant_num}_nobg"
    frames_resized="$TMP_DIR/${type}_${variant_num}_resized"

    mkdir -p "$frames_raw" "$frames_nobg" "$frames_resized" "$out_dir"

    # Step 1: Extract frames at 10fps
    echo "  [1/4] Extracting frames..."
    ffmpeg -y -i "$mp4" -vf fps=10 "$frames_raw/%04d.png" -loglevel error
    frame_count=$(ls "$frames_raw"/*.png 2>/dev/null | wc -l)
    echo "        $frame_count frames"

    # Step 2: Remove background
    echo "  [2/4] Removing background (GPU)..."
    rembg p "$frames_raw" "$frames_nobg"

    # Step 3: Resize to 256x256 with transparent padding
    echo "  [3/4] Resizing to 256x256..."
    for f in "$frames_nobg"/*.png; do
        base=$(basename "$f")
        magick "$f" -resize 256x256 -background none -gravity center -extent 256x256 "$frames_resized/$base"
    done

    # Step 4: TexturePacker spritesheet
    echo "  [4/4] Packing spritesheet..."
    "$TEXTUREPACKER" \
        --format json-array \
        --data "$out_dir/spritesheet.json" \
        --sheet "$out_dir/spritesheet.png" \
        --max-size 2048 \
        --size-constraints AnySize \
        --basic-sort-by Name \
        --trim-mode None \
        --extrude 0 \
        --algorithm Basic \
        --disable-auto-alias \
        --png-opt-level 0 \
        "$frames_resized/"

    echo "  -> animations/$type/$variant_num/ done"

    rm -rf "$frames_raw" "$frames_nobg" "$frames_resized"
done

rm -rf "$TMP_DIR"

echo ""
echo "=== All done. Regenerating animations.json... ==="

python - <<'PYEOF'
import json, os

anim_dir = "animations"
result = {}

for type_name in sorted(os.listdir(anim_dir)):
    type_path = os.path.join(anim_dir, type_name)
    if not os.path.isdir(type_path):
        continue

    subdirs = sorted(
        [d for d in os.listdir(type_path) if os.path.isdir(os.path.join(type_path, d)) and d.isdigit()],
        key=lambda x: int(x)
    )

    variants = []
    for variant_num in subdirs:
        sheet = f"animations/{type_name}/{variant_num}/spritesheet.png"
        atlas = f"animations/{type_name}/{variant_num}/spritesheet.json"
        if os.path.exists(sheet) and os.path.exists(atlas):
            variants.append({"spritesheet": sheet, "atlas": atlas})

    if variants:
        result[type_name] = variants

with open("animations.json", "w") as f:
    json.dump(result, f, indent=2)

total = sum(len(v) for v in result.values())
print(f"animations.json: {len(result)} types, {total} variants total")
PYEOF
