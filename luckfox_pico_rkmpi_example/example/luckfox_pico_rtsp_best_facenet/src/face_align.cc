// Face alignment via 5-point landmark similarity transform.
//
// Uses only opencv2/core (matrix algebra) and opencv2/imgproc (warpAffine).
// calib3d / estimateAffinePartial2D is NOT required.
//
// The similarity transform [a, -b, tx; b, a, ty] is estimated by solving the
// over-determined linear system (5 landmark pairs → 10 equations, 4 unknowns)
// via the normal equations:
//
//   A x = B   (10×4 system)
//   => (A^T A) x = A^T B   (4×4 system, solved with LU decomposition)
//
// where x = [a, b, tx, ty]^T and the constraint is:
//   x_dst = a * x_src - b * y_src + tx
//   y_dst = b * x_src + a * y_src + ty

#include "face_align.h"
#include <stdio.h>

// Standard ArcFace / MobileFaceNet reference landmarks for a 112x112 crop.
// Order: left_eye, right_eye, nose, left_mouth_corner, right_mouth_corner.
static const float ARCFACE_REF[5][2] = {
    {38.2946f, 51.6963f},   // left eye
    {73.5318f, 51.5014f},   // right eye
    {56.0252f, 71.7366f},   // nose tip
    {41.5493f, 92.3655f},   // left mouth corner
    {70.7299f, 92.2041f},   // right mouth corner
};

// Estimate a 2x3 similarity-transform matrix that maps 'src' landmark
// positions to 'dst' reference positions via least-squares.
// Returns a CV_64F 2x3 matrix suitable for cv::warpAffine (forward mapping).
static cv::Mat estimate_similarity_transform(
    const std::vector<cv::Point2f>& src,
    const std::vector<cv::Point2f>& dst)
{
    int N = (int)src.size();

    // Build the 2N × 4 matrix A and the 2N × 1 vector B.
    // Each landmark pair contributes two rows:
    //   [ xs, -ys, 1, 0 ] [a; b; tx; ty] = [xd]
    //   [ ys,  xs, 0, 1 ]                  [yd]
    cv::Mat A(2 * N, 4, CV_64F);
    cv::Mat B(2 * N, 1, CV_64F);

    for (int i = 0; i < N; i++) {
        double xs = (double)src[i].x;
        double ys = (double)src[i].y;

        A.at<double>(2*i,   0) =  xs;
        A.at<double>(2*i,   1) = -ys;
        A.at<double>(2*i,   2) =  1.0;
        A.at<double>(2*i,   3) =  0.0;

        A.at<double>(2*i+1, 0) =  ys;
        A.at<double>(2*i+1, 1) =  xs;
        A.at<double>(2*i+1, 2) =  0.0;
        A.at<double>(2*i+1, 3) =  1.0;

        B.at<double>(2*i,   0) = (double)dst[i].x;
        B.at<double>(2*i+1, 0) = (double)dst[i].y;
    }

    // Solve the 4×4 normal equations (A^T A) x = A^T B.
    cv::Mat ATA = A.t() * A;
    cv::Mat ATB = A.t() * B;
    cv::Mat x;
    bool ok = cv::solve(ATA, ATB, x, cv::DECOMP_LU);
    if (!ok) {
        printf("[face_align] Warning: solve failed, returning identity\n");
        return cv::Mat::eye(2, 3, CV_64F);
    }

    double a  = x.at<double>(0, 0);
    double b  = x.at<double>(1, 0);
    double tx = x.at<double>(2, 0);
    double ty = x.at<double>(3, 0);

    // Forward affine matrix: dst_pt = M * [src_x, src_y, 1]^T
    cv::Mat M(2, 3, CV_64F);
    M.at<double>(0, 0) =  a;
    M.at<double>(0, 1) = -b;
    M.at<double>(0, 2) =  tx;
    M.at<double>(1, 0) =  b;
    M.at<double>(1, 1) =  a;
    M.at<double>(1, 2) =  ty;

    return M;
}

cv::Mat align_face(const cv::Mat&                   image,
                   const std::vector<cv::Point2f>& landmarks)
{
    std::vector<cv::Point2f> dst;
    dst.reserve(5);
    for (int i = 0; i < 5; i++)
        dst.emplace_back(ARCFACE_REF[i][0], ARCFACE_REF[i][1]);

    cv::Mat M = estimate_similarity_transform(landmarks, dst);

    cv::Mat aligned;
    cv::warpAffine(image, aligned, M, cv::Size(112, 112),
                   cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return aligned;
}
