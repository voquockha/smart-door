#include "face_test_runner.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "opencv2/highgui/highgui.hpp"

FaceTestRunner::FaceTestRunner(FaceEventManager* event_manager)
    : event_manager_(event_manager)
{
}

int FaceTestRunner::run(const std::string& image_dir)
{
    if (!event_manager_) {
        printf("[test] FaceEventManager is null\n");
        return -1;
    }

    const std::vector<std::string> images = listImages(image_dir);
    if (images.empty()) {
        printf("[test] No test images found in %s\n", image_dir.c_str());
        return -1;
    }

    printf("[test] Running image simulation from %s (%zu image(s))\n",
           image_dir.c_str(), images.size());

    for (const std::string& image_path : images) {
        cv::Mat image = cv::imread(image_path);
        if (image.empty()) {
            printf("[test] Cannot load image: %s\n", image_path.c_str());
            continue;
        }

        FaceResult result;
        result.recognized = true;
        result.confidence = 0.85f;
        result.name = labelFromFilename(basename(image_path));
        result.person_id = personIdFromLabel(result.name);

        printf("[test] %s -> id=%s name=\"%s\" confidence=%.2f\n",
               image_path.c_str(), result.person_id.c_str(),
               result.name.c_str(), result.confidence);

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

std::string FaceTestRunner::stem(const std::string& filename)
{
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos)
        return filename;
    return filename.substr(0, dot);
}

std::string FaceTestRunner::labelFromFilename(const std::string& filename)
{
    std::string raw = stem(filename);
    for (char& ch : raw) {
        if (ch == '-' || ch == '.')
            ch = '_';
    }

    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < raw.size()) {
        while (pos < raw.size() && raw[pos] == '_')
            ++pos;
        const size_t start = pos;
        while (pos < raw.size() && raw[pos] != '_')
            ++pos;
        if (pos > start)
            tokens.push_back(raw.substr(start, pos - start));
    }

    while (!tokens.empty() && isNumberToken(tokens.back()))
        tokens.pop_back();

    std::string label;
    for (std::string token : tokens) {
        if (token.empty())
            continue;

        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        token[0] = (char)toupper((unsigned char)token[0]);

        if (!label.empty())
            label += " ";
        label += token;
    }

    if (label.empty())
        label = "Test User";
    return label;
}

std::string FaceTestRunner::personIdFromLabel(const std::string& label)
{
    std::string id = "test_";
    bool last_underscore = false;

    for (char ch : label) {
        unsigned char c = (unsigned char)ch;
        if (isalnum(c)) {
            id.push_back((char)tolower(c));
            last_underscore = false;
        } else if (!last_underscore) {
            id.push_back('_');
            last_underscore = true;
        }
    }

    while (!id.empty() && id[id.size() - 1] == '_')
        id.erase(id.size() - 1);
    if (id == "test")
        id = "test_user";
    return id;
}

bool FaceTestRunner::isNumberToken(const std::string& token)
{
    if (token.empty())
        return false;
    for (char ch : token) {
        if (!isdigit((unsigned char)ch))
            return false;
    }
    return true;
}
