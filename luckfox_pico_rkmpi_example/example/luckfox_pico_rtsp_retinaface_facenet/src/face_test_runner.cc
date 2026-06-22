#include "face_test_runner.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "opencv2/highgui/highgui.hpp"

FaceTestRunner::FaceTestRunner(FaceEventManager* event_manager)
    : event_manager_(event_manager)
{
}

int FaceTestRunner::run(const std::string& image_dir,
                        const RecognizeImageCallback& recognizer)
{
    if (!event_manager_) {
        printf("[test] FaceEventManager is null\n");
        return -1;
    }
    if (!recognizer) {
        printf("[test] Recognizer callback is empty\n");
        return -1;
    }

    const std::vector<std::string> images = listImages(image_dir);
    if (images.empty()) {
        printf("[test] No test images found in %s\n", image_dir.c_str());
        return -1;
    }

    printf("[test] Running real image recognition from %s (%zu image(s))\n",
           image_dir.c_str(), images.size());

    for (const std::string& image_path : images) {
        cv::Mat image = cv::imread(image_path);
        if (image.empty()) {
            printf("[test] Cannot load image: %s\n", image_path.c_str());
            continue;
        }

        FaceResult result;
        if (!recognizer(image, image_path, &result)) {
            printf("[test] %s -> UNKNOWN or no face\n", image_path.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        printf("[test] %s -> id=%s name=\"%s\" confidence=%.3f dist=%.3f\n",
               image_path.c_str(), result.person_id.c_str(),
               result.name.c_str(), result.confidence, result.distance);

        event_manager_->onFrame(Frame(image), result);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

std::vector<std::string> FaceTestRunner::listImages(
    const std::string& image_dir)
{
    std::vector<std::string> images;
    DIR* dir = opendir(image_dir.c_str());
    if (!dir) {
        printf("[test] Cannot open image directory: %s\n", image_dir.c_str());
        return images;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name.empty() || name[0] == '.')
            continue;
        if (!isImageFile(name))
            continue;

        std::string path = image_dir;
        if (!path.empty() && path[path.size() - 1] != '/')
            path += "/";
        path += name;
        images.push_back(path);
    }

    closedir(dir);
    std::sort(images.begin(), images.end());
    return images;
}

bool FaceTestRunner::isImageFile(const std::string& filename)
{
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)tolower(c); });

    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 ||
            lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".bmp") == lower.size() - 4 ||
            (lower.size() >= 5 &&
             lower.rfind(".jpeg") == lower.size() - 5));
}

std::string FaceTestRunner::basename(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}
