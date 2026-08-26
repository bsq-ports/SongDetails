#include "Data/DataGetter.hpp"
#include "StopWatch.hpp"
#include "Utils.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <utility>
#include <vector>
#include <fmt/core.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "brotli/decode.h"

namespace SongDetailsCache {
    namespace {
        constexpr size_t BROTLI_INITIAL_OUTPUT_SIZE = size_t{10} * 1024 * 1024; // 10 MiB
        constexpr size_t BROTLI_MAX_OUTPUT_SIZE = size_t{256} * 1024 * 1024; // 256 MiB
        constexpr size_t MAX_ETAG_SIZE = size_t{16} * 1024;
        constexpr std::string_view ETAG_CACHE_HEADER = "SongDetailsCache ETag v1\n";

        using BrotliDecoderPtr = std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>;

        std::mutex cacheWriteMutex;

        enum class AtomicWriteResult {
            Failed,
            Written,
            Durable,
        };

        void TrimWhitespace(std::string& value) {
            const auto isWhitespace = [](unsigned char character) {
                return std::isspace(character) != 0;
            };

            const auto first = std::find_if_not(value.begin(), value.end(), isWhitespace);
            const auto last = std::find_if_not(value.rbegin(), value.rend(), isWhitespace).base();
            if (first >= last) {
                value.clear();
                return;
            }
            value = std::string(first, last);
        }

        bool IsValidEtag(std::string_view value) {
            if (value.starts_with("W/")) value.remove_prefix(2);
            if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
                return false;
            }

            value.remove_prefix(1);
            value.remove_suffix(1);
            return std::all_of(value.begin(), value.end(), [](unsigned char character) {
                return character == 0x21 ||
                    (character >= 0x23 && character <= 0x7e) ||
                    character >= 0x80;
            });
        }

        bool IsHeaderName(std::string_view value, std::string_view expected) {
            return value.size() == expected.size() && std::equal(
                value.begin(),
                value.end(),
                expected.begin(),
                [](unsigned char lhs, unsigned char rhs) {
                    return std::tolower(lhs) == std::tolower(rhs);
                }
            );
        }

        std::string ReadFinalResponseHeader(
            std::string_view rawHeaders,
            std::string_view expectedName
        ) {
            std::istringstream input{std::string(rawHeaders)};
            std::string line;
            std::string currentValue;
            std::string finalValue;
            bool inResponseBlock = false;

            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.starts_with("HTTP/")) {
                    currentValue.clear();
                    inResponseBlock = true;
                    continue;
                }

                if (!inResponseBlock) continue;
                if (line.empty()) {
                    finalValue = std::move(currentValue);
                    currentValue.clear();
                    inResponseBlock = false;
                    continue;
                }

                const auto separator = line.find(':');
                if (separator == std::string::npos) continue;
                if (!IsHeaderName(std::string_view(line).substr(0, separator), expectedName)) {
                    continue;
                }

                currentValue = line.substr(separator + 1);
                TrimWhitespace(currentValue);
            }

            if (inResponseBlock) finalValue = std::move(currentValue);
            return finalValue;
        }

        std::string ReadEtag(const std::filesystem::path& path) {
            auto input = std::ifstream(path, std::ios::binary);
            if (!input) return {};

            std::error_code sizeError;
            const auto size = std::filesystem::file_size(path, sizeError);
            if (sizeError || size <= ETAG_CACHE_HEADER.size() || size > MAX_ETAG_SIZE) {
                return {};
            }

            std::string stored(static_cast<std::size_t>(size), '\0');
            input.read(stored.data(), static_cast<std::streamsize>(stored.size()));
            if (!input || !stored.starts_with(ETAG_CACHE_HEADER)) return {};

            std::string etag = stored.substr(ETAG_CACHE_HEADER.size());
            TrimWhitespace(etag);
            return IsValidEtag(etag) ? etag : std::string{};
        }

        bool FlushDescriptor(int descriptor) {
            int result;
            do {
                result = ::fsync(descriptor);
            } while (result != 0 && errno == EINTR);
            return result == 0;
        }

        bool FlushDirectory(const std::filesystem::path& directory) {
            const auto directoryString = directory.string();
            const int descriptor = ::open(
                directoryString.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC
            );
            if (descriptor < 0) {
                const int error = errno;
                WARNING(
                    "Failed to open cache directory '{}' for flushing: {} ({})",
                    directoryString,
                    std::strerror(error),
                    error
                );
                return false;
            }

            bool success = true;
            if (!FlushDescriptor(descriptor)) {
                const int error = errno;
                WARNING(
                    "Failed to flush cache directory '{}': {} ({})",
                    directoryString,
                    std::strerror(error),
                    error
                );
                success = false;
            }
            if (::close(descriptor) != 0) {
                const int error = errno;
                WARNING(
                    "Failed to close cache directory '{}': {} ({})",
                    directoryString,
                    std::strerror(error),
                    error
                );
                success = false;
            }
            return success;
        }

        AtomicWriteResult WriteFileAtomically(
            const std::filesystem::path& destination,
            const uint8_t* data,
            size_t size
        ) {
            const auto destinationString = destination.string();
            std::string temporaryString = destinationString + ".tmp.XXXXXX";
            int descriptor = ::mkstemp(temporaryString.data());
            if (descriptor < 0) {
                const int error = errno;
                ERROR(
                    "Failed to create a temporary cache file for '{}': {} ({})",
                    destinationString,
                    std::strerror(error),
                    error
                );
                return AtomicWriteResult::Failed;
            }

            const auto fail = [&](std::string_view operation, int error) {
                if (descriptor >= 0) {
                    ::close(descriptor);
                    descriptor = -1;
                }
                ::unlink(temporaryString.c_str());
                ERROR(
                    "Failed to {} cache file '{}': {} ({})",
                    operation,
                    temporaryString,
                    std::strerror(error),
                    error
                );
                return AtomicWriteResult::Failed;
            };

            if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
                return fail("set close-on-exec on", errno);
            }

            size_t offset = 0;
            while (offset < size) {
                const size_t remaining = size - offset;
                const size_t writeSize = std::min(
                    remaining,
                    static_cast<size_t>(std::numeric_limits<ssize_t>::max())
                );
                const ssize_t written = ::write(descriptor, data + offset, writeSize);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    return fail("write", errno);
                }
                if (written == 0) {
                    return fail("write", EIO);
                }
                offset += static_cast<size_t>(written);
            }

            if (!FlushDescriptor(descriptor)) {
                return fail("flush", errno);
            }

            if (::close(descriptor) != 0) {
                const int error = errno;
                descriptor = -1;
                ::unlink(temporaryString.c_str());
                ERROR(
                    "Failed to close temporary cache file '{}': {} ({})",
                    temporaryString,
                    std::strerror(error),
                    error
                );
                return AtomicWriteResult::Failed;
            }
            descriptor = -1;

            if (::rename(temporaryString.c_str(), destinationString.c_str()) != 0) {
                const int error = errno;
                ::unlink(temporaryString.c_str());
                ERROR(
                    "Failed to replace cache file '{}': {} ({})",
                    destinationString,
                    std::strerror(error),
                    error
                );
                return AtomicWriteResult::Failed;
            }

            return FlushDirectory(destination.parent_path())
                ? AtomicWriteResult::Durable
                : AtomicWriteResult::Written;
        }

        bool InvalidateCachedEtags() {
            for (const auto& [source, _] : DataGetter::dataSources) {
                const auto path = DataGetter::cachePathEtag(source);
                const auto pathString = path.string();

                int flags = O_WRONLY | O_TRUNC | O_CLOEXEC;
#ifdef O_NOFOLLOW
                flags |= O_NOFOLLOW;
#endif
                int descriptor = ::open(pathString.c_str(), flags);
                if (descriptor < 0) {
                    if (errno == ENOENT) continue;
                    const int error = errno;
                    ERROR(
                        "Failed to invalidate cached ETag '{}': {} ({})",
                        pathString,
                        std::strerror(error),
                        error
                    );
                    return false;
                }

                if (!FlushDescriptor(descriptor)) {
                    const int error = errno;
                    ::close(descriptor);
                    ERROR(
                        "Failed to flush invalidated ETag '{}': {} ({})",
                        pathString,
                        std::strerror(error),
                        error
                    );
                    return false;
                }
                if (::close(descriptor) != 0) {
                    const int error = errno;
                    ERROR(
                        "Failed to close invalidated ETag '{}': {} ({})",
                        pathString,
                        std::strerror(error),
                        error
                    );
                    return false;
                }
            }
            return true;
        }
    }

    std::filesystem::path DataGetter::basePath{"/sdcard/ModData/com.beatgames.beatsaber/Mods/SongDetails"};
    std::filesystem::path DataGetter::cachePath() { return basePath / "SongDetailsCache.proto"; }
    std::filesystem::path DataGetter::cachePathEtag(std::string_view source) {
        return basePath / fmt::format("SongDetailsCache.proto.{}.etag", source);
    }
    const std::array<std::pair<std::string_view, std::string_view>, 2> DataGetter::dataSources {{
        {"Direct", "https://raw.githubusercontent.com/kinsi55/BeatSaberScrappedData/master/songDetails2_v3.br"},
        // JSDelivr is a cached fallback when the direct GitHub request fails.
        {"JSDelivr", "https://cdn.jsdelivr.net/gh/kinsi55/BeatSaberScrappedData/songDetails2_v3.br"},
    }};

    std::optional<std::ifstream> DataGetter::ReadCachedDatabase() {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(cachePath(), sizeError);
        if (sizeError || size == 0 || size > BROTLI_MAX_OUTPUT_SIZE) {
            if (!sizeError) {
                ERROR(
                    "Cached database had an invalid size: {} bytes (maximum {})",
                    size,
                    BROTLI_MAX_OUTPUT_SIZE
                );
            }
            return std::nullopt;
        }
        return std::ifstream(cachePath(), std::ios::binary);
    }

    std::future<std::optional<DataGetter::DownloadedDatabase>> DataGetter::UpdateAndReadDatabase(
        std::string_view dataSourceName,
        bool useCachedEtag
    ) {
        return std::async(
            std::launch::async,
            &DataGetter::UpdateAndReadDatabase_internal,
            std::string(dataSourceName),
            useCachedEtag
        );
    }

    std::future<bool> DataGetter::WriteCachedDatabase(DownloadedDatabase db) {
        return std::async(
            std::launch::async,
            [db = std::move(db)] {
                return WriteCachedDatabase_internal(db);
            }
        );
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

    std::optional<DataGetter::DownloadedDatabase> DataGetter::UpdateAndReadDatabase_internal(
        std::string_view dataSourceName,
        bool useCachedEtag
    ) {
        const auto sourceEntry = std::find_if(
            dataSources.begin(),
            dataSources.end(),
            [dataSourceName](const auto& entry) {
                return entry.first == dataSourceName;
            }
        );
        if (sourceEntry == dataSources.end()) {
            throw std::invalid_argument(fmt::format(
                "Could not find database source '{}'",
                dataSourceName
            ));
        }
        const auto dataSource = sourceEntry->second;

        std::unordered_map<std::string, std::string> req_headers {
            {"Accept", "*/*"}
        };

        bool sentEtag = false;
        if (useCachedEtag) {
            auto etag = ReadEtag(cachePathEtag(dataSourceName));
            if (!etag.empty()) {
                req_headers.emplace("If-None-Match", std::move(etag));
                sentEtag = true;
            }
        }

        auto resp = WebUtil::GetAsync(dataSource, 20, req_headers).get();
        if (resp.httpCode == 304 && resp.curlStatus == CURLE_OK) {
            if (!sentEtag) {
                throw std::runtime_error(fmt::format(
                    "Database source '{}' returned 304 without a conditional request",
                    dataSourceName
                ));
            }
            INFO("Database source '{}' was not modified", dataSourceName);
            return std::nullopt;
        }

        if (resp.curlStatus != CURLE_OK || resp.httpCode != 200) {
            throw std::runtime_error(fmt::format(
                "Failed to download database from '{}': HTTP {}, curl {} ({})",
                dataSourceName,
                resp.httpCode,
                curl_easy_strerror(resp.curlStatus),
                static_cast<int>(resp.curlStatus)
            ));
        }

        DownloadedDatabase downloadedDatabase;
        downloadedDatabase.source = std::string(dataSourceName);
        downloadedDatabase.etag = ReadFinalResponseHeader(resp.headers, "ETag");
        if (
            downloadedDatabase.etag.size() > MAX_ETAG_SIZE - ETAG_CACHE_HEADER.size() ||
            (!downloadedDatabase.etag.empty() && !IsValidEtag(downloadedDatabase.etag))
        ) {
            WARNING(
                "Ignoring invalid ETag from database source '{}': {} bytes",
                dataSourceName,
                downloadedDatabase.etag.size()
            );
            downloadedDatabase.etag.clear();
        }

        StopWatch sw; sw.Start();
        downloadedDatabase.data = std::make_shared<std::vector<uint8_t>>();
        if (!DecompressBrotli(*downloadedDatabase.data, resp.content)) {
            throw std::runtime_error(fmt::format(
                "Failed to decompress database from '{}'",
                dataSourceName
            ));
        }
        INFO("Decompressed brotli data in {}ms", sw.EllapsedMilliseconds());
        // DEBUG("Downloaded Database source   : {}", downloadedDatabase.source);
        // DEBUG("Downloaded Database etag     : {}", downloadedDatabase.etag);
        // DEBUG("Downloaded Database data     : {}", fmt::ptr(downloadedDatabase.data->data()));
        // DEBUG("Downloaded Database data size: {}", downloadedDatabase.data->size());
        return downloadedDatabase;
    }

    bool DataGetter::WriteCachedDatabase_internal(const DownloadedDatabase& db) {
        if (!db.data || db.data->empty()) {
            ERROR("Refusing to cache an empty database from '{}'", db.source);
            return false;
        }

        const bool knownSource = std::any_of(
            dataSources.begin(),
            dataSources.end(),
            [&db](const auto& entry) {
                return entry.first == db.source;
            }
        );
        if (!knownSource) {
            ERROR("Refusing to cache a database from unknown source '{}'", db.source);
            return false;
        }

        std::lock_guard lock(cacheWriteMutex);

        std::error_code directoryError;
        std::filesystem::create_directories(cachePath().parent_path(), directoryError);
        if (directoryError) {
            ERROR(
                "Failed to create cache directory '{}': {}",
                cachePath().parent_path().string(),
                directoryError.message()
            );
            return false;
        }

        // ETags belong to the exact bytes in the shared cache, not just to a
        // source URL. Empty every old source tag before replacing those bytes.
        if (!InvalidateCachedEtags()) {
            return false;
        }

        const auto databaseWrite = WriteFileAtomically(
            cachePath(),
            db.data->data(),
            db.data->size()
        );
        if (databaseWrite == AtomicWriteResult::Failed) {
            return false;
        }

        // Never persist a validator unless the cache rename itself is known to
        // be durable. The complete cache is still usable without an ETag.
        if (databaseWrite != AtomicWriteResult::Durable) {
            WARNING(
                "Database cache was updated, but its ETag was skipped because the cache directory could not be flushed"
            );
            return true;
        }

        if (db.etag.empty()) return true;
        if (
            db.etag.size() > MAX_ETAG_SIZE - ETAG_CACHE_HEADER.size() ||
            !IsValidEtag(db.etag)
        ) {
            WARNING("Database cache was updated, but its ETag for '{}' was invalid", db.source);
            return true;
        }

        std::string storedEtag(ETAG_CACHE_HEADER);
        storedEtag += db.etag;
        const auto* etagData = reinterpret_cast<const uint8_t*>(storedEtag.data());
        if (WriteFileAtomically(cachePathEtag(db.source), etagData, storedEtag.size()) == AtomicWriteResult::Failed) {
            WARNING(
                "Database cache was updated, but its ETag for '{}' could not be saved",
                db.source
            );
        }

        return true;
    }
}
