/**
 * @file object_labels.hpp
 * @brief The detection model's own class names.
 *
 * Source of truth — the class list embedded in the model's own metadata
 * (managed_components/espressif__coco_detect/models/yolo11n_320.onnx),
 * the model `COCODetect::YOLO11N_320_S8_V1` selects (`main/detector.cpp`):
 *
 *   {0: 'person', 1: 'bicycle', … , 79: 'toothbrush'}
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

namespace obj {

inline constexpr const char *kNames[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
    "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush",
};

inline constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

/** The model's name for a category, or nullptr if the index is outside the list. */
inline const char *name(int category)
{
    if (category < 0 || category >= kCount) {
        return nullptr;
    }
    return kNames[category];
}

}  // namespace obj
