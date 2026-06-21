#ifndef TELEGRAM_CLIENT_H
#define TELEGRAM_CLIENT_H

#include <string>

class TelegramClient {
public:
    TelegramClient();
    TelegramClient(std::string bot_token, std::string chat_id);

    bool isConfigured() const;
    bool sendPhoto(const std::string& image_path,
                   const std::string& caption) const;

private:
    static std::string getEnvOrDefault(const char* name,
                                       const std::string& fallback);
    static bool runCurl(const std::string& url,
                        const std::string& chat_id,
                        const std::string& image_path,
                        const std::string& caption);

    std::string bot_token_;
    std::string chat_id_;
};

#endif /* TELEGRAM_CLIENT_H */
