#ifndef FACE_ALIGN_H
#define FACE_ALIGN_H

// Face alignment using RetinaFace 5-point landmarks.
//
// Implements a closed-form 2D similarity transform (rotation + scale +
// translation) that warps any detected face into the canonical 112x112
// ArcFace / MobileFaceNet space, independent of pose, scale, or position.
//
// Only opencv2/core and opencv2/imgproc are required (no calib3d).

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include <vector>

// Align a face from 'image' using five RetinaFace landmarks.
//
// 'landmarks' must contain exactly 5 points in this order:
//   [0] left eye
//   [1] right eye
//   [2] nose tip
//   [3] left mouth corner
//   [4] right mouth corner
//
// All coordinates must be in the same pixel space as 'image'.
//
// Returns a new 112x112 BGR image aligned to the standard ArcFace positions:
//   left_eye    = (38.2946, 51.6963)
//   right_eye   = (73.5318, 51.5014)
//   nose        = (56.0252, 71.7366)
//   left_mouth  = (41.5493, 92.3655)
//   right_mouth = (70.7299, 92.2041)
cv::Mat align_face(const cv::Mat& image,
                   const std::vector<cv::Point2f>& landmarks);

#endif // FACE_ALIGN_H
