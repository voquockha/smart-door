#ifndef FACE_TEST_RUNNER_H
#define FACE_TEST_RUNNER_H

#include <string>
#include <vector>

#include "face_event_manager.h"

class FaceTestRunner {
public:
    explicit FaceTestRunner(FaceEventManager* event_manager);

    int run(const std::string& image_dir = "/test_images");

private:
    static std::vector<std::string> listImages(const std::string& image_dir);
    static bool isImageFile(const std::string& filename);
    static std::string basename(const std::string& path);
    static std::string stem(const std::string& filename);
    static std::string labelFromFilename(const std::string& filename);
    static std::string personIdFromLabel(const std::string& label);
    static bool isNumberToken(const std::string& token);

    FaceEventManager* event_manager_;
};

#endif /* FACE_TEST_RUNNER_H */
