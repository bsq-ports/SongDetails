#include "Data/DataGetter.hpp"
#include "StopWatch.hpp"
#include "Utils.hpp"
#include "logging.hpp"

#include <algorithm>
#include <vector>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <fmt/core.h>
#include <sys/stat.h>
#include "brotli/decode.h"

namespace SongDetailsCache {
    namespace {
        constexpr size_t BROTLI_INITIAL_OUTPUT_SIZE = 10 * 1024 * 1024; // 10 MiB
        constexpr size_t BROTLI_MAX_OUTPUT_SIZE = 256 * 1024 * 1024; // 256 MiB

        using BrotliDecoderPtr = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;
    }

    std::filesystem::path DataGetter::basePath{"/sdcard/ModData/com.beatgames.beatsaber/Mods/SongDetails"};
    std::filesystem::path DataGetter::cachePath() { return basePath / "SongDetailsCache.proto"; }
    std::filesystem::path DataGetter::cachePathEtag(std::string_view source) {
        return basePath / fmt::format("SongDetailsCache.proto.{}.etag", source);
    }
    // just copied from the C# binary (order from bottom to top)
    const std::unordered_map<std::string, std::string> DataGetter::dataSources {
		// Caches stuff for 12 hours as backup
		{ "JSDelivr", "https://cdn.jsdelivr.net/gh/kinsi55/BeatSaberScrappedData/songDetails2_v3.br" },
        { "Direct", "https://raw.githubusercontent.com/kinsi55/BeatSaberScrappedData/master/songDetails2_v3.br" },
    };

    std::optional<std::ifstream> DataGetter::ReadCachedDatabase() {
        if (!std::filesystem::exists(cachePath())) return std::nullopt;
        return std::ifstream(cachePath(), std::ios::binary);
    }

    std::future<std::optional<DataGetter::DownloadedDatabase>> DataGetter::UpdateAndReadDatabase(std::string_view dataSourceName) {
        return std::async(std::launch::async, std::bind(&DataGetter::UpdateAndReadDatabase_internal, dataSourceName));
    }

    std::future<void> DataGetter::WriteCachedDatabase(DownloadedDatabase& db) {
        return std::async(std::launch::async, std::bind(&DataGetter::WriteCachedDatabase_internal, db));
    }

    bool DataGetter::HasCachedData(int maximumAgeHours) {
        using namespace std::chrono;
        if (!std::filesystem::exists(cachePath())) return false;
        struct stat fInfo = {0};
        if (stat(cachePath().c_str(), &fInfo) == 0) {
            return seconds(fInfo.st_mtim.tv_sec) > (system_clock::now().time_since_epoch() - hours(maximumAgeHours));
        }

        return false;
    }

    bool DataGetter::DecompressBrotli(std::vector<uint8_t>& out, const std::string& in) {
        out.clear();
        if (in.empty()) return false;

        BrotliDecoderPtr state(
            BrotliDecoderCreateInstance(nullptr, nullptr, nullptr),
            &BrotliDecoderDestroyInstance
        );
        if (!state) return false;

        try {
            // Temp array where brotli decodes 
            std::vector<uint8_t> decoded(BROTLI_INITIAL_OUTPUT_SIZE);

            // Pointers to the input and output buffers, as required by the Brotli API
            const auto* next_in = reinterpret_cast<const uint8_t*>(in.data()); // Next byte to read from
            size_t available_in = in.size();
            uint8_t* next_out = decoded.data(); // Next byte to write to
            size_t available_out = decoded.size();  // Available bytes to write to
            size_t total_out = 0; // Total number of bytes written to the output buffer

            while (true) {
                const BrotliDecoderResult result = BrotliDecoderDecompressStream(
                    state.get(),
                    &available_in,
                    &next_in,
                    &available_out,
                    &next_out,
                    &total_out
                );

                switch (result) {
                    case BROTLI_DECODER_RESULT_SUCCESS:
                        // This database is exactly one Brotli stream. Do not silently
                        // accept a valid stream followed by unrelated trailing bytes.
                        if (available_in != 0 || !BrotliDecoderIsFinished(state.get())) {
                            ERROR("Brotli stream finished with {} trailing input bytes", available_in);
                            return false;
                        }

                        decoded.resize(total_out);
                        out = std::move(decoded);
                        return true;

                    case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: {
                        if (decoded.size() >= BROTLI_MAX_OUTPUT_SIZE) {
                            ERROR("Brotli output exceeded the {} byte limit", BROTLI_MAX_OUTPUT_SIZE);
                            return false;
                        }

                        // Grow geometrically while clamping to the configured maximum.
                        const size_t remaining_capacity = BROTLI_MAX_OUTPUT_SIZE - decoded.size();
                        const size_t growth = std::min(decoded.size(), remaining_capacity);
                        decoded.resize(decoded.size() + growth);
                        available_out = decoded.size() - total_out;
                        next_out = decoded.data() + total_out;
                        break;
                    }

                    case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
                        // The entire HTTP response was supplied above, so asking for
                        // more input means that the compressed stream is incomplete.
                        ERROR("Brotli stream ended before decompression completed");
                        return false;

                    case BROTLI_DECODER_RESULT_ERROR: {
                        const auto error = BrotliDecoderGetErrorCode(state.get());
                        ERROR("Brotli decompression failed: {}", BrotliDecoderErrorString(error));
                        return false;
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            ERROR("Failed to allocate memory while decompressing Brotli data");
            return false;
        }
    }

    std::optional<DataGetter::DownloadedDatabase> DataGetter::UpdateAndReadDatabase_internal(std::string_view dataSourceName) {
        std::string dataSource;
        for (const auto& [key, value] : dataSources) {
            if (key == dataSourceName) {
                dataSource = value;
            }
        }
        // if we don't find a datasource url just return;
        if (dataSource.empty()) {
            ERROR("Could not find correct datasource for name {}", dataSourceName);
            return std::nullopt;
        }

        std::unordered_map<std::string, std::string> req_headers {
            {"Accept", "*/*"}
        };

        if (std::filesystem::exists(cachePathEtag(dataSourceName))) {
            std::ifstream(cachePathEtag(dataSourceName)) >> req_headers["If-None-Match"];
        }

        auto resp = WebUtil::GetAsync(dataSource, 20, req_headers).get();
        // value is same
        if (resp.httpCode == 304 && resp.curlStatus == CURLE_OK) {
            INFO("Etag was the same, returning nullopt");
            return std::nullopt;
        }

        if (!resp) { // failed to perform Get operation properly
            std::string log = fmt::format("Failed to dl database: httpCode: {}, curl status: {} ({})", resp.httpCode, curl_easy_strerror(resp.curlStatus), (int)resp.curlStatus);
            ERROR("{}", log);
            throw std::runtime_error(log);
        }


        DownloadedDatabase downloadedDatabase;
        // assign data source
        downloadedDatabase.source = dataSourceName;

        // get etag from buffer
        std::istringstream buf(resp.headers);
        std::string line;
        while (std::getline(buf, line)) {
            if (line.starts_with("ETag: ")) {
                auto tagStart = line.find_first_of('"');
                auto tagLength = (line.find_last_of('"') + 1) - tagStart;
                downloadedDatabase.etag = line.substr(tagStart, tagLength);
                break;
            }
        }

        // Decompress contents as brotli
        StopWatch sw; sw.Start();
        downloadedDatabase.data = std::make_shared<std::vector<uint8_t>>();
        auto decompressOk = DecompressBrotli(*downloadedDatabase.data, resp.content);
        if (!decompressOk) {
            ERROR("Failed to decompress brotli data");
            return std::nullopt;
        }
        INFO("Decompressed brotli data in {}ms", sw.EllapsedMilliseconds());
        // DEBUG("Downloaded Database source   : {}", downloadedDatabase.source);
        // DEBUG("Downloaded Database etag     : {}", downloadedDatabase.etag);
        // DEBUG("Downloaded Database data     : {}", fmt::ptr(downloadedDatabase.data->data()));
        // DEBUG("Downloaded Database data size: {}", downloadedDatabase.data->size());
        return downloadedDatabase;
    }

    void DataGetter::WriteCachedDatabase_internal(DownloadedDatabase& db) {
        // DEBUG("Writing cached Database with source {}", db.source);
        std::filesystem::create_directories(cachePath().parent_path());
        auto out = std::ofstream(cachePath(), std::ios::binary);
        out.write((char*)db.data->data(), db.data->size());

        // DEBUG("Writing etag as well: '{}'", db.etag);
        auto etagPath = cachePathEtag(db.source);
        auto etagOut = std::ofstream(etagPath, std::ios::binary);
        etagOut.write(db.etag.data(), db.etag.size());
    }
}
