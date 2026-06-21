#ifndef ATTENDANCE_UTILS_H
#define ATTENDANCE_UTILS_H

#include <string>

std::string generateTransID();
std::string formatTimeForTelegram(const std::string& timestamp);

#endif /* ATTENDANCE_UTILS_H */
