#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SongDetailsCache {
    class DataGetter {
        public:
            struct DownloadedDatabase {
                std::string source;
                std::string etag;
                std::shared_ptr<std::vector<uint8_t>> data;
            };

            static const std::array<std::pair<std::string_view, std::string_view>, 2> dataSources;
            static std::filesystem::path cachePath();
            static std::filesystem::path cachePathEtag(std::string_view source);

            static std::future<std::optional<DownloadedDatabase>> UpdateAndReadDatabase(
                std::string_view dataSourceName = "Direct",
                bool useCachedEtag = true
            );
            static std::future<bool> WriteCachedDatabase(DownloadedDatabase db);
            static std::optional<std::ifstream> ReadCachedDatabase();
            static bool HasCachedData(int maximumAgeHours = 12);
        private:
            static std::filesystem::path basePath;
            friend class SongDetails;
            static bool WriteCachedDatabase_internal(const DownloadedDatabase& db);
            static bool DecompressBrotli(std::vector<uint8_t>& out, std::string const& in);
            static std::optional<DownloadedDatabase> UpdateAndReadDatabase_internal(
                std::string_view dataSourceName,
                bool useCachedEtag
            );
    };
}
