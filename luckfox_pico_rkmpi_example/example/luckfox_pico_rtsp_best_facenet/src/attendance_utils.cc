#include "attendance_utils.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <random>
#include <sstream>

std::string generateTransID()
{
    static std::random_device rd;
    static std::mt19937_64 rng(
        ((uint64_t)rd() << 32) ^ (uint64_t)time(nullptr) ^ (uint64_t)getpid());

    uint64_t value = rng();
    char buf[16];
    snprintf(buf, sizeof(buf), "%011llx",
             (unsigned long long)(value & 0x7ffffffffffULL));
    return std::string(buf);
}

std::string formatTimeForTelegram(const std::string& timestamp)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(timestamp.c_str(), "%d-%d-%d %d:%d:%d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        return timestamp;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
             day, month, year, hour, minute, second);
    return std::string(buf);
}
