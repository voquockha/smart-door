#include "telegram_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>
#include <vector>

namespace {
constexpr int kTelegramRetryCount = 3;
}

TelegramClient::TelegramClient()
    : bot_token_(getEnvOrDefault("TELEGRAM_BOT_TOKEN", "")),
      chat_id_(getEnvOrDefault("TELEGRAM_CHAT_ID", ""))
{
}

TelegramClient::TelegramClient(std::string bot_token, std::string chat_id)
    : bot_token_(std::move(bot_token)), chat_id_(std::move(chat_id))
{
}

bool TelegramClient::isConfigured() const
{
    return !bot_token_.empty() && !chat_id_.empty();
}

bool TelegramClient::sendPhoto(const std::string& image_path,
                               const std::string& caption) const
{
    if (!isConfigured()) {
        printf("[telegram] TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID is not set\n");
        return false;
    }

    const std::string url =
        "https://api.telegram.org/bot" + bot_token_ + "/sendPhoto";

    for (int attempt = 1; attempt <= kTelegramRetryCount; ++attempt) {
        if (runCurl(url, chat_id_, image_path, caption)) {
            printf("[telegram] sendPhoto success: %s\n", image_path.c_str());
            return true;
        }

        printf("[telegram] sendPhoto failed attempt %d/%d\n",
               attempt, kTelegramRetryCount);
        sleep((unsigned int)attempt);
    }

    return false;
}

std::string TelegramClient::getEnvOrDefault(const char* name,
                                            const std::string& fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return value;
}

bool TelegramClient::runCurl(const std::string& url,
                             const std::string& chat_id,
                             const std::string& image_path,
                             const std::string& caption)
{
    const std::string chat_arg = "chat_id=" + chat_id;
    const std::string photo_arg = "photo=@" + image_path;
    const std::string caption_arg = "caption=" + caption;

    std::vector<std::string> args;
    args.push_back("curl");
    args.push_back("-sS");
    args.push_back("--fail");
    args.push_back("-o");
    args.push_back("/dev/null");
    args.push_back("--connect-timeout");
    args.push_back("5");
    args.push_back("--max-time");
    args.push_back("20");
    args.push_back("-X");
    args.push_back("POST");
    args.push_back(url);
    args.push_back("-F");
    args.push_back(chat_arg);
    args.push_back("-F");
    args.push_back(photo_arg);
    args.push_back("-F");
    args.push_back(caption_arg);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args)
        argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        printf("[telegram] fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        execvp("curl", argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        printf("[telegram] waitpid failed: %s\n", strerror(errno));
        return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
