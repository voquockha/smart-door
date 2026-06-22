#ifndef TELEGRAM_CLIENT_H
#define TELEGRAM_CLIENT_H

#include <string>

class TelegramClient {
public:
    TelegramClient();
    TelegramClient(std::string bot_token, std::string chat_id);

    bool isConfigured() const;
    bool sendMessage(const std::string& text) const;
    bool sendPhoto(const std::string& image_path,
                   const std::string& caption) const;

private:
    static std::string getEnvOrDefault(const char* name,
                                       const std::string& fallback);
    static bool getEnvBool(const char* name, bool fallback);
    static std::string maskToken(const std::string& token);
    static std::string maskId(const std::string& id);
    static bool fileExists(const std::string& path);
    static bool postMessage(const std::string& url,
                            const std::string& chat_id,
                            const std::string& text,
                            bool allow_insecure);
    static bool postPhoto(const std::string& url,
                          const std::string& chat_id,
                          const std::string& image_path,
                          const std::string& caption,
                          bool allow_insecure);

    std::string bot_token_;
    std::string chat_id_;
    bool allow_insecure_;
};

#endif /* TELEGRAM_CLIENT_H */
