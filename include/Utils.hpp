#pragma once

#include "Data/SongDetailsContainer.hpp"
#include <cstdint>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"

namespace SongDetailsCache {
    class HexUtil {
        public:
            static std::string ByteArrayToHex(std::span<const uint8_t> bytes);
            static std::string SongBytesToHash(std::size_t index);

            /// @throws std::invalid_argument if the input is not complete hexadecimal bytes
            static std::vector<uint8_t> ToBytes(std::string_view hex);
            static SongHash ToSongHash(std::string_view hex);
    };

    class WebUtil {
        public:
            using Headers = std::unordered_map<std::string, std::string>;

            struct WebResponse {
                /// @brief http response code
                long httpCode = 0;
                /// @brief curl status code after curl_easy_perform
                CURLcode curlStatus = CURLE_FAILED_INIT;
                /// @brief http headers
                std::string headers;
                /// @brief http content
                std::string content;

            };

            /// @brief performs an async get request
            /// @param url the url to GET
            /// @param timeoutSeconds total timeout for the request, in seconds
            /// @param headers headers to pass along with the request
            /// @return async future to await for the response
            static std::future<WebResponse> GetAsync(
                std::string_view url,
                uint32_t timeoutSeconds,
                const Headers& headers
            );
        private:
            static WebResponse GetAsync_internal(
                std::string url,
                uint32_t timeoutSeconds,
                Headers headers
            );
    };
}
