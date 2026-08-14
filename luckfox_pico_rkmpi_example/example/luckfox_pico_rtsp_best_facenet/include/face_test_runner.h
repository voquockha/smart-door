#ifndef FACE_TEST_RUNNER_H
#define FACE_TEST_RUNNER_H

#include <functional>
#include <string>
#include <vector>

#include "face_event_manager.h"
#include "opencv2/core/core.hpp"

class FaceTestRunner {
public:
    using RecognizeImageCallback =
        std::function<bool(const cv::Mat& image,
                           const std::string& image_path,
                           FaceResult* result)>;

    explicit FaceTestRunner(FaceEventManager* event_manager);

    int run(const std::string& image_dir,
            const RecognizeImageCallback& recognizer);

private:
    static std::vector<std::string> listImages(const std::string& image_dir);
    static bool isImageFile(const std::string& filename);
    static std::string basename(const std::string& path);

    FaceEventManager* event_manager_;
};

#endif /* FACE_TEST_RUNNER_H */
