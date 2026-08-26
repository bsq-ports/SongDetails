#include "Data/SongDetailsContainer.hpp"
#include "logging.hpp"
#include "SongDetails.hpp"
#include "Data/DataGetter.hpp"
#include <cstring>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>
#include "StopWatch.hpp"

#include "SongProto.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

namespace SongDetailsCache {
    namespace {
        constexpr int MAX_DATABASE_SIZE = 256 * 1024 * 1024;

        template<typename T>
        T ReadUnaligned(const uint8_t* bytes) noexcept {
            T value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        template<typename T>
        T ReadStringBytes(std::string_view bytes, std::size_t offset) noexcept {
            if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return {};
            return ReadUnaligned<T>(
                reinterpret_cast<const uint8_t*>(bytes.data() + offset)
            );
        }
    }

    shared_ptr_vector<uint32_t> SongDetailsContainer::keys;
    shared_ptr_vector<SongHash> SongDetailsContainer::hashBytes;
    shared_ptr_vector<uint32_t> SongDetailsContainer::hashBytesLUT;
    shared_ptr_vector<std::string> SongDetailsContainer::songNames;
    shared_ptr_vector<std::string> SongDetailsContainer::songAuthorNames;
    shared_ptr_vector<std::string> SongDetailsContainer::levelAuthorNames;
    shared_ptr_vector<std::string> SongDetailsContainer::uploaderNames;
    shared_ptr_unordered_map<std::string, uint64_t> SongDetailsContainer::tags;

    std::chrono::sys_seconds SongDetailsContainer::scrapeEndedTimeUnix;
    std::chrono::steady_clock::time_point SongDetailsContainer::lastSuccessfulUpdate{};

    shared_ptr_vector<Song> SongDetailsContainer::songs;
    shared_ptr_vector<SongDifficulty> SongDetailsContainer::difficulties;

    BSHookUtils::UnorderedEventCallback<> SongDetailsContainer::dataAvailableOrUpdatedInternal;
    BSHookUtils::UnorderedEventCallback<std::string> SongDetailsContainer::dataLoadFailedInternal;

    std::future<void> SongDetailsContainer::Load(bool reload, int acceptableAgeHours) {
        return std::async(std::launch::async, std::bind(&SongDetailsContainer::Load_internal, reload, acceptableAgeHours));
    }

    void SongDetailsContainer::Load_internal(bool reload, int acceptableAgeHours) {
        using namespace std::chrono;
        using namespace std::chrono_literals;
        struct stat fInfo = {0};

        bool shouldLoadFresh = false;
        bool cacheFileValid = false;
        if (stat(DataGetter::cachePath().c_str(), &fInfo) == 0) { // only succeeds if exists
            try {
                auto cachedStreamOpt = DataGetter::ReadCachedDatabase();
                if (cachedStreamOpt) {
                    DEBUG("Processing cached data!");
                    StopWatch sw; sw.Start();
                    cacheFileValid = Process(*cachedStreamOpt, false);
                    DEBUG("Processed cached data in {}ms", sw.EllapsedMilliseconds());
                    cachedStreamOpt->close();

                    if (!cacheFileValid) {
                        INFO("Cached data was invalid, will download fresh data");
                        shouldLoadFresh = true;
                    }
                } else {
                    shouldLoadFresh = true;
                }
            } catch (const std::exception& exception) {
                ERROR(
                    "Failed to read cached data, will download fresh data: {}",
                    exception.what()
                );
                shouldLoadFresh = true;
            } catch (...) {
                ERROR("Failed to read cached data, will download fresh data");
                shouldLoadFresh = true;
            }

            if (get_isDataAvailable() && std::chrono::system_clock::now().time_since_epoch() - scrapeEndedTimeUnix.time_since_epoch() > std::chrono::hours(std::max(acceptableAgeHours, 1))) {
                shouldLoadFresh = true;
            }
            if (reload) shouldLoadFresh = true;
        } else { // didn't exist or otherwise failed
            shouldLoadFresh = true;
        }

        const auto now = steady_clock::now();
        const bool recentlyUpdated =
            lastSuccessfulUpdate != steady_clock::time_point{} &&
            now - lastSuccessfulUpdate < 30min;
        if (!shouldLoadFresh || (!reload && cacheFileValid && recentlyUpdated)) {
            SongDetails::isLoading = false;
            return;
        }

        bool refreshCompleted = false;

        for (const auto& [src, _] : DataGetter::dataSources) {
            try {
                // A conditional request is safe only when the local cache has
                // parsed and passed all structural validation checks.
                auto db = DataGetter::UpdateAndReadDatabase(src, cacheFileValid).get();
                if (!db.has_value()) {
                    // A 304 response means the already validated cache remains current.
                    refreshCompleted = true;
                    break;
                }

                StopWatch sw; sw.Start();
                if (!Process(*db->data)) {
                    ERROR("Downloaded invalid data from source {}", src);
                    cacheFileValid = false;
                    continue;
                }
                refreshCompleted = true;
                DEBUG("Processed new data in {}ms", sw.EllapsedMilliseconds());
                sw.Restart();
                if (DataGetter::WriteCachedDatabase(std::move(*db)).get()) {
                    cacheFileValid = true;
                    DEBUG("Wrote data in {}ms", sw.EllapsedMilliseconds());
                } else {
                    ERROR("Failed to cache database from source {}", src);
                }

                if (get_isDataAvailable()) {
                    break;
                }
            } catch (const std::exception& exception) {
                ERROR("Failed to download from source {}: {}", src, exception.what());
            } catch (...) {
                ERROR("Failed to download from source {} due to an unknown error", src);
            }
        }

        if (refreshCompleted) {
            lastSuccessfulUpdate = steady_clock::now();
        }

        if (!get_isDataAvailable()) {
            // TODO: Collect the last error
            dataLoadFailedInternal.invoke("DB failed to download");
        }
        SongDetails::isLoading = false;
    }

    bool SongDetailsContainer::Process(std::istream& istream, bool force) {
        StopWatch sw; sw.Start();
        Structs::SongDetailsV3 parsedContainer;
        google::protobuf::io::IstreamInputStream rawInput(&istream);
        google::protobuf::io::CodedInputStream codedInput(&rawInput);
        codedInput.SetTotalBytesLimit(MAX_DATABASE_SIZE);
        if (
            !parsedContainer.ParseFromCodedStream(&codedInput) ||
            !codedInput.ConsumedEntireMessage()
        ) {
            ERROR("Failed to parse Song container from istream!");
            return false;
        }

        INFO("Parsed protobuf in {}ms", sw.EllapsedMilliseconds());
        return Process(parsedContainer, force);
    }

    bool SongDetailsContainer::Process(const std::vector<uint8_t>& data, bool force) {
        StopWatch sw; sw.Start();
        Structs::SongDetailsV3 parsedContainer;
        if (data.size() > static_cast<std::size_t>(MAX_DATABASE_SIZE)) {
            ERROR("Song container exceeded the {} byte parsing limit", MAX_DATABASE_SIZE);
            return false;
        }
        if (!parsedContainer.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            ERROR("Failed to parse Song container from data!");
            return false;
        }

        INFO("Parsed protobuf in {}ms", sw.EllapsedMilliseconds());
        return Process(parsedContainer, force);
    }

    bool SongDetailsContainer::Process(const Structs::SongDetailsV3& parsedContainer, bool force) {
        constexpr uint32_t expectedFormatVersion = 3;
        auto& parsedField = parsedContainer.songs();

        if (parsedContainer.formatversion() != expectedFormatVersion) {
            ERROR(
                "Unsupported SongDetails format version: expected {}, got {}",
                expectedFormatVersion,
                parsedContainer.formatversion()
            );
            return false;
        }

        if (parsedField.empty()) {
            ERROR("Parsing data yielded no songs!");
            return false;
        }

        const auto songCount = static_cast<std::size_t>(parsedField.size());
        const auto expectedHashBytes = songCount * HASH_SIZE_BYTES;
        if (parsedContainer.songhashes().size() != expectedHashBytes) {
            ERROR(
                "Song hash data had an invalid size: expected {}, got {}",
                expectedHashBytes,
                parsedContainer.songhashes().size()
            );
            return false;
        }

        constexpr int maxTagCount = std::numeric_limits<uint64_t>::digits;
        const int tagCount = parsedContainer.taglist_size();
        if (tagCount > maxTagCount) {
            ERROR(
                "Song tag list exceeded the {} tag bitmask limit: got {}",
                maxTagCount,
                parsedContainer.taglist_size()
            );
            return false;
        }

        std::unordered_set<std::string_view> uniqueTags;
        uniqueTags.reserve(static_cast<std::size_t>(tagCount));
        for (const auto& tag : parsedContainer.taglist()) {
            if (tag.empty()) {
                ERROR("Song tag list contained an empty tag");
                return false;
            }
            if (!uniqueTags.emplace(tag).second) {
                ERROR("Song tag list contained duplicate tag '{}'", tag);
                return false;
            }
        }

        // A valid cache can still be useful for conditional requests even when
        // an existing in-memory database should not be replaced.
        if (!force && songs) return true;

        const auto newScrapeEndedTimeUnix = std::chrono::sys_seconds(
            std::chrono::seconds(parsedContainer.scrapeendedunix())
        );
        const auto len = songCount;
        StopWatch sw; sw.Start();

        std::vector<const Structs::SongV3*> parsed;
        parsed.resize(len);
        INFO("Got {} songs in data", len);
        for (std::size_t idx = 0; const auto& s : parsedField) parsed[idx++] = &s;

        // we run everything with resize so everything is already valid memory
        auto newSongs = make_shared_vec<Song>();
        newSongs->reserve(len);

		auto newKeys = make_shared_vec<uint32_t>();
        newKeys->reserve(len);
		auto newHashes = make_shared_vec<SongHash>();
        newHashes->reserve(len);
		auto newHashesLUT = make_shared_vec<uint32_t>();
        newHashesLUT->reserve(len);

		auto newSongNames = make_shared_vec<std::string>();
        newSongNames->reserve(len);
		auto newSongAuthorNames = make_shared_vec<std::string>();
        newSongAuthorNames->reserve(len);
		auto newLevelAuthorNames = make_shared_vec<std::string>();
        newLevelAuthorNames->reserve(len);
		auto newUploaderNames = make_shared_vec<std::string>();
        newUploaderNames->reserve(len);
        auto newTags = make_shared_unordered_map<std::string, uint64_t>();

        auto newDiffs = make_shared_vec<SongDifficulty>();
        std::size_t diffLen = 0;
        for (auto s : parsed) diffLen += s->difficulties().size();
        newDiffs->reserve(diffLen);
        sw.Restart();
        std::size_t diffIndex = 0;

        // Fill the tags
        auto& parsedTagList = parsedContainer.taglist();
        for (int i = 0; i < parsedTagList.size(); i++) {
            newTags->emplace(parsedTagList[i], uint64_t{1} << i);
        }

        // Cast it to a vector of SongHashes
        const auto* songHashesRaw = reinterpret_cast<const uint8_t*>(
            parsedContainer.songhashes().data()
        );
        for (std::size_t i = 0; i < len; i++) {
            const auto& parsedSong = parsed[i];
            uint8_t diffCount = std::min(255, parsedSong->difficulties_size());
            const auto& builtSong = newSongs->emplace_back(i, diffIndex, diffCount, parsedSong);
            newKeys->emplace_back(parsedSong->mapid());
            newHashes->push_back(SongHash(songHashesRaw + i * HASH_SIZE_BYTES));

            newSongNames->emplace_back(parsedSong->songname());
            newSongAuthorNames->emplace_back(parsedSong->songauthorname());
            newLevelAuthorNames->emplace_back(parsedSong->levelauthorname());
            // If they're equal, uploaderName is omitted from the dump
            newUploaderNames->emplace_back(parsedSong->has_uploadername() ? parsedSong->uploadername() : parsedSong->levelauthorname());
            if (parsedSong->difficulties().empty()) continue;

            for (const auto& diff : parsedSong->difficulties()) {
                newDiffs->emplace_back(i, &diff);
                diffIndex++;
            }
        }
        INFO("Processed songs in {}ms", sw.EllapsedMilliseconds());
        sw.Restart();
        // we need to sort the hashes, but not the original vector, let's just use pointers because that's more efficient than by copy
        std::vector<const Song*> sortedByHashes{};
        sortedByHashes.resize(len);
        for (std::size_t idx = 0; const auto& s : *newSongs) sortedByHashes[idx++] = &s;
        std::stable_sort(sortedByHashes.begin(), sortedByHashes.end(), [&](const Song* lhs, const Song* rhs){
            return newHashes->operator[](lhs->index).c1 < newHashes->operator[](rhs->index).c1;
        });

        for (const auto& sorted : sortedByHashes) {
            newHashesLUT->emplace_back(sorted->index);
        }

        INFO("Made hashes LUT in {}ms", sw.EllapsedMilliseconds());

        INFO("Finished Processing");
        if (!force && songs) return true;

        INFO("Assigning new values");
        songs = newSongs;
        keys = newKeys;
        hashBytes = newHashes;
        hashBytesLUT = newHashesLUT;

        songNames = newSongNames;
        songAuthorNames = newSongAuthorNames;
        levelAuthorNames = newLevelAuthorNames;
        uploaderNames = newUploaderNames;
        tags = newTags;

        difficulties = newDiffs;
        scrapeEndedTimeUnix = newScrapeEndedTimeUnix;

        if (get_isDataAvailable()) {
            try {
                dataAvailableOrUpdatedInternal.invoke();
            } catch (const std::exception& _) {}
        }
        return get_isDataAvailable();
    }

    SongHash::SongHash(const uint8_t* bytes) noexcept :
        c1(ReadUnaligned<uint32_t>(bytes)),
        c2(ReadUnaligned<uint64_t>(bytes + sizeof(c1))),
        c3(ReadUnaligned<uint64_t>(bytes + sizeof(c1) + sizeof(c2))) {}

    SongHash::SongHash(const std::string& str) :
        c1(ReadStringBytes<uint32_t>(str, 0)),
        c2(ReadStringBytes<uint64_t>(str, sizeof(c1))),
        c3(ReadStringBytes<uint64_t>(str, sizeof(c1) + sizeof(c2))) {}

    SongHash::SongHash() : c1(0), c2(0), c3(0) {}
}
