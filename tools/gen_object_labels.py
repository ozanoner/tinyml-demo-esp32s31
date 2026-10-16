#!/usr/bin/env python3
"""Generate main/object_labels.hpp from the model's own class list.

The list is not transcribed by hand and not taken from memory: it is parsed out of
the ONNX metadata the Ultralytics export embedded in the model the app actually
runs — managed_components/espressif__coco_detect/models/yolo11n_320.onnx, the model
`COCODetect::YOLO11N_320_S8_V1` selects (main/detector.cpp). The `.espdl` image the
target runs carries no name strings, and esp-dl ships name tables for classification
only (vision/classification/imagenet_category_name.hpp, hand_gesture_category_name.hpp),
so the model's metadata is the only source for this mapping.

The script asserts the parse is complete and contiguous 0..79 before it writes.

Usage: python3 tools/gen_object_labels.py            # regenerate the header
       python3 tools/gen_object_labels.py --check    # verify only, print the list
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MODEL = REPO / "managed_components/espressif__coco_detect/models/yolo11n_320.onnx"
OUT = REPO / "main/object_labels.hpp"

if not MODEL.is_file():
    sys.exit(f"FAIL: model not found at {MODEL} — run a build first (the component "
             f"manager fetches managed_components/)")

dump = subprocess.run(["strings", str(MODEL)], capture_output=True, text=True, check=True).stdout
m = re.search(r"\{0: 'person'.*?79: '[a-z ]+'\}", dump)
if not m:
    sys.exit("FAIL: model metadata does not contain the 0..79 class list")

pairs = re.findall(r"(\d+): '([^']*)'", m.group(0))
if len(pairs) != 80:
    sys.exit(f"FAIL: expected 80 classes, parsed {len(pairs)}")
for i, (idx, _) in enumerate(pairs):
    if int(idx) != i:
        sys.exit(f"FAIL: class list is not contiguous at position {i} (got index {idx})")

names = [n for _, n in pairs]

if "--check" in sys.argv:
    print(f"OK: {len(names)} classes parsed, contiguous 0..79")
    for i, n in enumerate(names):
        print(f"{i:3d} {n}")
    sys.exit(0)

rows, line = [], "    "
for n in names:
    token = f'"{n}",'
    if len(line) + len(token) + 1 > 96:
        rows.append(line.rstrip())
        line = "    "
    line += token + " "
rows.append(line.rstrip())
body = "\n".join(rows)

OUT.write_text(f'''/**
 * @file object_labels.hpp
 * @brief The detection model's own class names.
 *
 * Source of truth — the class list embedded in the model's own metadata
 * ({MODEL.relative_to(REPO)}),
 * the model `COCODetect::YOLO11N_320_S8_V1` selects (`main/detector.cpp`):
 *
 *   {{0: 'person', 1: 'bicycle', … , 79: 'toothbrush'}}
 *
 * The names are copied verbatim, in the model's own order, and this file is
 * generated from that string by `tools/gen_object_labels.py` — which asserts the
 * list is complete and contiguous 0..79 before writing. The `.espdl` image the
 * target runs carries no name strings, and esp-dl ships name tables for
 * classification only (`vision/classification/imagenet_category_name.hpp`,
 * `hand_gesture_category_name.hpp`), so this is the only place the mapping can
 * come from. Regenerate with `python3 tools/gen_object_labels.py`; the file is
 * cheap to diff and must never be edited by hand.
 *
 * A category the list does not know is not named: callers must handle nullptr
 * rather than print an invented label.
 */
#pragma once

namespace obj {{

inline constexpr const char *kNames[] = {{
{body}
}};

inline constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

/** The model's name for a category, or nullptr if the index is outside the list. */
inline const char *name(int category)
{{
    if (category < 0 || category >= kCount) {{
        return nullptr;
    }}
    return kNames[category];
}}

}}  // namespace obj
''')
print(f"wrote {OUT.relative_to(REPO)} with {len(names)} names")
