#include "Data/DataGetter.hpp"
#include "Utils.hpp"
#include "logging.hpp"

#include <vector>
#include <fstream>
#include <sstream>
#include <fmt/core.h>
#include <sys/stat.h>
#include "brotli/decode.h"

namespace SongDetailsCache {
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
        const size_t size = in.size();
        const char* data = in.c_str();
        if (size == 0) return false;

        // Create decoder state
        BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!state)
            return false; // Could not create decoder state

        const uint8_t* next_in = reinterpret_cast<const uint8_t*>(data);
        size_t available_in = size;

        const size_t chunk_size = 1024 * 1024 * 1;

        // Clear the output vector to start fresh
        out.clear();

        // Variable for brotli to track available output space
        size_t available_out = out.size();
        uint8_t* next_out = &out[0];

        // Total number of bytes written to the output buffer
        size_t total_out = 0;

        while (true) {
            const size_t old_size = out.size();

            // Reserve more space if we run out
            if (available_out == 0) {
                out.resize(old_size + chunk_size);
                available_out = chunk_size;

                // Remake the pointer to the new buffer
                next_out = &out[total_out];
            }

            // Do the decompression
            BrotliDecoderResult result = BrotliDecoderDecompressStream(
                state,
                &available_in,
                &next_in,
                &available_out,
                &next_out,
                &total_out
            );

            // Resize the vector to the actual number of bytes we wrote in this iteration
            if (out.size() > total_out) {
                out.resize(total_out);
            }

            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
                // The decoder wants more input, but we have no more data left in `available_in`
                if (available_in == 0) {
                    BrotliDecoderDestroyInstance(state);
                    return false; // Incomplete data
                }
                // Otherwise, if you’re streaming from a file or network, you’d load more here
            }
            else if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                // Our buffer was completely filled, so we loop again to extend it
                continue;
            }
            else if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                // Decompression successful
                break;
            }
            else {
                // Some error occurred
                BrotliDecoderDestroyInstance(state);
                return false;
            }
        }
        BrotliDecoderDestroyInstance(state);
        return true;
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
        downloadedDatabase.data = std::make_shared<std::vector<uint8_t>>();
        auto decompressOk = DecompressBrotli(*downloadedDatabase.data, resp.content);
        if (!decompressOk) {
            ERROR("Failed to decompress brotli data");
            return std::nullopt;
        }

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
