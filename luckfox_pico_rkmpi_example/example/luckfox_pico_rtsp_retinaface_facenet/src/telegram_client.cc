#include "telegram_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace {
constexpr int kTelegramRetryCount = 3;
constexpr long kConnectTimeoutSeconds = 5;
constexpr long kTotalTimeoutSeconds = 20;

std::once_flag g_curl_init_once;

size_t writeStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    if (!userdata)
        return 0;

    std::string* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

void configureCommonCurl(CURL* curl,
                         const std::string& url,
                         bool allow_insecure,
                         std::string* response)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luckfox-attendance/1.0");

    if (allow_insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

bool logCurlResult(const char* op,
                   CURLcode code,
                   CURL* curl,
                   const std::string& response)
{
    long http_code = 0;
    if (curl)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (code == CURLE_OK && http_code >= 200 && http_code < 300) {
        printf("[telegram] %s success http=%ld response=%s\n",
               op, http_code, response.empty() ? "<empty>" : response.c_str());
        return true;
    }

    printf("[telegram] %s failed curl_code=%d curl_error=%s http=%ld response=%s\n",
           op, (int)code, curl_easy_strerror(code), http_code,
           response.empty() ? "<empty>" : response.c_str());

    if (code == CURLE_SSL_CACERT || code == CURLE_PEER_FAILED_VERIFICATION) {
        printf("[telegram] TLS verify failed. Use TELEGRAM_ALLOW_INSECURE=1 for testing or install CA certificates\n");
    } else if (code == CURLE_COULDNT_RESOLVE_HOST) {
        printf("[telegram] DNS failed. Check /etc/resolv.conf and api.telegram.org\n");
    } else if (code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT) {
        printf("[telegram] Network connect/timeout. Check HTTPS access to api.telegram.org:443\n");
    }

    if (http_code == 400 || http_code == 401 || http_code == 403) {
        printf("[telegram] Telegram rejected request. Check bot token, chat_id, and whether the user has started the bot\n");
    }

    return false;
}
}  // namespace

TelegramClient::TelegramClient()
    : bot_token_(getEnvOrDefault("TELEGRAM_BOT_TOKEN", "")),
      chat_id_(getEnvOrDefault("TELEGRAM_CHAT_ID", "")),
      allow_insecure_(getEnvBool("TELEGRAM_ALLOW_INSECURE", false))
{
    std::call_once(g_curl_init_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });

    printf("[telegram] init token=%s chat_id=%s allow_insecure=%s transport=libcurl/%s\n",
           maskToken(bot_token_).c_str(), maskId(chat_id_).c_str(),
           allow_insecure_ ? "true" : "false", curl_version());
}

TelegramClient::TelegramClient(std::string bot_token, std::string chat_id)
    : bot_token_(std::move(bot_token)),
      chat_id_(std::move(chat_id)),
      allow_insecure_(getEnvBool("TELEGRAM_ALLOW_INSECURE", false))
{
    std::call_once(g_curl_init_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });

    printf("[telegram] init token=%s chat_id=%s allow_insecure=%s transport=libcurl/%s\n",
           maskToken(bot_token_).c_str(), maskId(chat_id_).c_str(),
           allow_insecure_ ? "true" : "false", curl_version());
}

bool TelegramClient::isConfigured() const
{
    return !bot_token_.empty() && !chat_id_.empty();
}

bool TelegramClient::sendMessage(const std::string& text) const
{
    if (!isConfigured()) {
        printf("[telegram] TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID is not set\n");
        return false;
    }

    const std::string url =
        "https://api.telegram.org/bot" + bot_token_ + "/sendMessage";

    printf("[telegram] sendMessage start text_len=%zu token=%s chat_id=%s\n",
           text.size(), maskToken(bot_token_).c_str(),
           maskId(chat_id_).c_str());

    for (int attempt = 1; attempt <= kTelegramRetryCount; ++attempt) {
        printf("[telegram] sendMessage attempt %d/%d via libcurl\n",
               attempt, kTelegramRetryCount);
        if (postMessage(url, chat_id_, text, allow_insecure_))
            return true;

        sleep((unsigned int)attempt);
    }

    return false;
}

bool TelegramClient::sendPhoto(const std::string& image_path,
                               const std::string& caption) const
{
    if (!isConfigured()) {
        printf("[telegram] TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID is not set\n");
        return false;
    }
    if (!fileExists(image_path)) {
        printf("[telegram] image file does not exist: %s\n",
               image_path.c_str());
        return false;
    }

    const std::string url =
        "https://api.telegram.org/bot" + bot_token_ + "/sendPhoto";

    printf("[telegram] sendPhoto start image=%s caption_len=%zu token=%s chat_id=%s\n",
           image_path.c_str(), caption.size(), maskToken(bot_token_).c_str(),
           maskId(chat_id_).c_str());

    for (int attempt = 1; attempt <= kTelegramRetryCount; ++attempt) {
        printf("[telegram] sendPhoto attempt %d/%d via libcurl\n",
               attempt, kTelegramRetryCount);
        if (postPhoto(url, chat_id_, image_path, caption, allow_insecure_))
            return true;

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

bool TelegramClient::getEnvBool(const char* name, bool fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

std::string TelegramClient::maskToken(const std::string& token)
{
    if (token.empty())
        return "<empty>";
    const size_t colon = token.find(':');
    if (colon == std::string::npos || token.size() <= colon + 8)
        return "<set,len=" + std::to_string(token.size()) + ">";
    return token.substr(0, std::min<size_t>(colon, 4)) + "***" +
           token.substr(colon, 5) + "***" +
           token.substr(token.size() - 4);
}

std::string TelegramClient::maskId(const std::string& id)
{
    if (id.empty())
        return "<empty>";
    if (id.size() <= 4)
        return "***";
    return "***" + id.substr(id.size() - 4);
}

bool TelegramClient::fileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool TelegramClient::postMessage(const std::string& url,
                                 const std::string& chat_id,
                                 const std::string& text,
                                 bool allow_insecure)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        printf("[telegram] curl_easy_init failed\n");
        return false;
    }

    std::string response;
    configureCommonCurl(curl, url, allow_insecure, &response);

    char* chat_escaped = curl_easy_escape(curl, chat_id.c_str(), 0);
    char* text_escaped = curl_easy_escape(curl, text.c_str(), 0);
    if (!chat_escaped || !text_escaped) {
        printf("[telegram] curl_easy_escape failed\n");
        if (chat_escaped)
            curl_free(chat_escaped);
        if (text_escaped)
            curl_free(text_escaped);
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string fields =
        "chat_id=" + std::string(chat_escaped) +
        "&text=" + std::string(text_escaped);
    curl_free(chat_escaped);
    curl_free(text_escaped);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)fields.size());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
                                "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode code = curl_easy_perform(curl);
    const bool ok = logCurlResult("sendMessage", code, curl, response);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

bool TelegramClient::postPhoto(const std::string& url,
                               const std::string& chat_id,
                               const std::string& image_path,
                               const std::string& caption,
                               bool allow_insecure)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        printf("[telegram] curl_easy_init failed\n");
        return false;
    }

    std::string response;
    configureCommonCurl(curl, url, allow_insecure, &response);

    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        printf("[telegram] curl_mime_init failed\n");
        curl_easy_cleanup(curl);
        return false;
    }

    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, chat_id.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "caption");
    curl_mime_data(part, caption.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "photo");
    curl_mime_filedata(part, image_path.c_str());

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode code = curl_easy_perform(curl);
    const bool ok = logCurlResult("sendPhoto", code, curl, response);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return ok;
}
