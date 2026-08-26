#include "Utils.hpp"
#include "logging.hpp"
#include "Data/SongDetailsContainer.hpp"

#include <charconv>
#include <limits>
#include <memory>
#include <stdexcept>

namespace SongDetailsCache {
    namespace {
        constexpr std::size_t MAX_WEB_RESPONSE_SIZE = std::size_t{128} * 1024 * 1024; // 128 MiB
        constexpr std::size_t MAX_WEB_HEADER_SIZE = std::size_t{1} * 1024 * 1024; // 1 MiB
        constexpr char USER_AGENT[] = "https://github.com/bsq-ports/SongDetails/" VERSION;
        constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

        class CurlGlobal final {
            public:
                CurlGlobal() : status(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
                ~CurlGlobal() {
                    if (status == CURLE_OK) curl_global_cleanup();
                }

                const CURLcode status;
        };

        const CurlGlobal& GetCurlGlobal() {
            static CurlGlobal global;
            return global;
        }

        struct CurlWriteTarget {
            std::string& output;
            std::size_t maximumSize;
            bool limitExceeded = false;
            bool writeFailed = false;
        };

        std::size_t CurlWriteCallback(
            char* contents,
            std::size_t size,
            std::size_t nmemb,
            void* userdata
        ) noexcept {
            auto* target = static_cast<CurlWriteTarget*>(userdata);
            if (!target) return 0;

            try {
                if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) {
                    target->limitExceeded = true;
                    return 0;
                }

                const std::size_t length = size * nmemb;
                if (
                    target->output.size() > target->maximumSize ||
                    length > target->maximumSize - target->output.size()
                ) {
                    target->limitExceeded = true;
                    return 0;
                }

                target->output.append(contents, length);
                return length;
            } catch (...) {
                target->writeFailed = true;
                return 0;
            }
        }

        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        using CurlHeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

        bool AppendCurlHeader(CurlHeaderList& headers, const std::string& header) {
            auto* original = headers.release();
            auto* appended = curl_slist_append(original, header.c_str());
            headers.reset(appended ? appended : original);
            return appended != nullptr;
        }

        template<typename T>
        bool SetCurlOption(
            CURL* curl,
            CURLoption option,
            T value,
            WebUtil::WebResponse& response
        ) {
            const auto status = curl_easy_setopt(curl, option, value);
            if (status == CURLE_OK) return true;

            response.curlStatus = status;
            ERROR(
                "curl_easy_setopt({}) failed: {} ({})",
                static_cast<int>(option),
                curl_easy_strerror(status),
                static_cast<int>(status)
            );
            return false;
        }
    }

    std::string HexUtil::ByteArrayToHex(std::span<const uint8_t> bytes) {
        std::string result(bytes.size() * 2, '\0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            result[i * 2] = HEX_DIGITS[bytes[i] >> 4];
            result[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0f];
        }
        return result;
    }

    std::string HexUtil::SongBytesToHash(std::size_t index) {
        const uint8_t* bytes = SongDetailsContainer::hashBytes->operator[](index);
        return ByteArrayToHex({bytes, SongDetailsContainer::HASH_SIZE_BYTES});
    }

    SongHash HexUtil::ToSongHash(std::string_view hex) {
        if (hex.size() != SongDetailsContainer::HASH_SIZE_BYTES * 2) return SongHash();

        try {
            const auto bytes = ToBytes(hex);
            return SongHash(bytes.data());
        } catch (const std::invalid_argument&) {
            return SongHash();
        }
    }

    std::vector<uint8_t> HexUtil::ToBytes(std::string_view hex) {
        if (hex.size() % 2 != 0) {
            throw std::invalid_argument("Hex string must contain an even number of characters");
        }

        std::vector<uint8_t> result(hex.size() / 2);
        for (std::size_t i = 0; i < result.size(); ++i) {
            unsigned int value = 0;
            const char* first = hex.data() + i * 2;
            const char* last = first + 2;
            const auto [end, error] = std::from_chars(first, last, value, 16);
            if (error != std::errc{} || end != last) {
                throw std::invalid_argument("Hex string contains a non-hex character");
            }
            result[i] = static_cast<uint8_t>(value);
        }
        return result;
    }

    std::future<WebUtil::WebResponse> WebUtil::GetAsync(
        std::string_view url,
        uint32_t timeoutSeconds,
        const Headers& headers
    ) {
        const auto globalStatus = GetCurlGlobal().status;
        if (globalStatus != CURLE_OK) {
            ERROR(
                "curl_global_init() failed: {} ({})",
                curl_easy_strerror(globalStatus),
                static_cast<int>(globalStatus)
            );
            return std::async(std::launch::deferred, [globalStatus] {
                WebResponse response{};
                response.curlStatus = globalStatus;
                return response;
            });
        }

        return std::async(
            std::launch::async,
            [url = std::string(url), timeoutSeconds, headers]() mutable {
                return GetAsync_internal(std::move(url), timeoutSeconds, std::move(headers));
            }
        );
    }

    WebUtil::WebResponse WebUtil::GetAsync_internal(
        std::string url,
        uint32_t timeoutSeconds,
        Headers headers
    ) {
        WebResponse response{};

        CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
        if (!curl) {
            ERROR("curl_easy_init() failed");
            return response;
        }

        CurlHeaderList curlHeaders(nullptr, &curl_slist_free_all);
        for (const auto& [key, value] : headers) {
            const auto header = fmt::format("{}: {}", key, value);
            if (!AppendCurlHeader(curlHeaders, header)) {
                ERROR("Failed to allocate curl request headers");
                response.curlStatus = CURLE_OUT_OF_MEMORY;
                return response;
            }
        }

        CurlWriteTarget contentTarget{response.content, MAX_WEB_RESPONSE_SIZE};
        CurlWriteTarget headerTarget{response.headers, MAX_WEB_HEADER_SIZE};
        const auto setOption = [&](CURLoption option, auto value) {
            return SetCurlOption(curl.get(), option, value, response);
        };

        if (
            !setOption(CURLOPT_HTTPHEADER, curlHeaders.get()) ||
            !setOption(CURLOPT_URL, url.c_str()) ||
            !setOption(CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds)) ||
            !setOption(CURLOPT_NOSIGNAL, 1L) ||
            !setOption(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(MAX_WEB_RESPONSE_SIZE)) ||
            !setOption(CURLOPT_FOLLOWLOCATION, 1L) ||
            !setOption(CURLOPT_MAXREDIRS, 5L) ||
            !setOption(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS)) ||
            !setOption(CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS)) ||
            !setOption(CURLOPT_WRITEFUNCTION, CurlWriteCallback) ||
            !setOption(CURLOPT_WRITEDATA, &contentTarget) ||
            !setOption(CURLOPT_HEADERFUNCTION, CurlWriteCallback) ||
            !setOption(CURLOPT_HEADERDATA, &headerTarget) ||
            !setOption(CURLOPT_USERAGENT, USER_AGENT) ||
            // TODO: Configure a Quest-compatible CA bundle, then enable peer verification.
            !setOption(CURLOPT_SSL_VERIFYPEER, 0L)
        ) {
            return response;
        }

        response.curlStatus = curl_easy_perform(curl.get());
        /* Check for errors */
        if (response.curlStatus != CURLE_OK) {
            if (contentTarget.limitExceeded) {
                ERROR("HTTP response exceeded the {} byte limit", MAX_WEB_RESPONSE_SIZE);
            } else if (headerTarget.limitExceeded) {
                ERROR("HTTP response headers exceeded the {} byte limit", MAX_WEB_HEADER_SIZE);
            } else if (contentTarget.writeFailed || headerTarget.writeFailed) {
                ERROR("Failed to store the HTTP response in memory");
            }
            ERROR(
                "curl_easy_perform() failed: {}: {}",
                static_cast<int>(response.curlStatus),
                curl_easy_strerror(response.curlStatus)
            );
        }

        const auto infoStatus = curl_easy_getinfo(
            curl.get(),
            CURLINFO_RESPONSE_CODE,
            &response.httpCode
        );
        if (infoStatus != CURLE_OK) {
            ERROR(
                "curl_easy_getinfo(CURLINFO_RESPONSE_CODE) failed: {} ({})",
                curl_easy_strerror(infoStatus),
                static_cast<int>(infoStatus)
            );
            if (response.curlStatus == CURLE_OK) response.curlStatus = infoStatus;
        }

        return response;
    }
}
