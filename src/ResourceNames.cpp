#include "ResourceNames.h"

// HttpUtils.h needs these but does not include them, and windows.h's min/max macros break the SDK headers
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <vector>
#include <Util/HttpUtils.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <future>
#include <string_view>
#include <unordered_map>

#include <Logging.h>
#include <Glacier/ZResourceID.h>

static const char* c_HashListUrl = "https://raw.githubusercontent.com/glacier-modding/Hitman-Hashes/refs/heads/main/paths/TEMP.json";
static const std::string_view c_EntityTypeSuffix = "].pc_entitytype";

static std::unordered_map<uint64_t, std::string> g_Names;
static std::atomic<bool> g_NamesReady = false;
static std::future<void> g_Loading;

static std::string_view JsonStringField(std::string_view object, std::string_view key)
{
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    const size_t keyPos = object.find(quotedKey);

    if (keyPos == std::string_view::npos)
    {
        return {};
    }

    const size_t open = object.find('"', object.find(':', keyPos + quotedKey.size()) + 1);
    const size_t close = open == std::string_view::npos ? std::string_view::npos : object.find('"', open + 1);

    if (close == std::string_view::npos)
    {
        return {};
    }

    return object.substr(open + 1, close - open - 1);
}

static std::string ShortName(std::string_view path)
{
    if (path.ends_with(c_EntityTypeSuffix))
    {
        path.remove_suffix(c_EntityTypeSuffix.size());
    }

    const size_t slash = path.find_last_of('/');

    if (slash != std::string_view::npos)
    {
        path.remove_prefix(slash + 1);
    }

    for (const std::string_view extension : { ".entitytemplate", ".prim", ".brick", ".entity", ".class" })
    {
        if (path.ends_with(extension))
        {
            path.remove_suffix(extension.size());
            break;
        }
    }

    return std::string(path);
}

// Saved owners are identified by blueprint (TBLU) ids, but the list is keyed by TEMP id, so the blueprint id is derived from the path
static void ParseHashList(const std::string& json)
{
    size_t cursor = 0;

    while (true)
    {
        const size_t open = json.find('{', cursor);
        const size_t close = open == std::string::npos ? std::string::npos : json.find('}', open);

        if (close == std::string::npos)
        {
            break;
        }

        cursor = close + 1;
        const std::string_view object(json.data() + open, close - open);
        const std::string_view hash = JsonStringField(object, "hash");
        const std::string_view path = JsonStringField(object, "path");

        if (hash.size() != 16)
        {
            continue;
        }

        const std::string name = path.empty() ? std::string(JsonStringField(object, "hint")) : ShortName(path);

        if (name.empty())
        {
            continue;
        }

        g_Names.emplace(ZRuntimeResourceID::FromString(std::string(hash)).GetID(), name);

        if (path.ends_with(c_EntityTypeSuffix))
        {
            std::string blueprintPath(path.substr(0, path.size() - c_EntityTypeSuffix.size()));
            blueprintPath += "].pc_entityblueprint";
            std::transform(blueprintPath.begin(), blueprintPath.end(), blueprintPath.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            g_Names.emplace(ZRuntimeResourceID::FromString(blueprintPath).GetID(), name);
        }
    }
}

void ResourceNames::StartLoading()
{
    if (g_Loading.valid())
    {
        return;
    }

    g_Loading = std::async(std::launch::async, [] {
        try
        {
            const std::string json = Util::HttpUtils::DownloadFromUrl(c_HashListUrl);
            ParseHashList(json);
        }
        catch (const std::exception& e)
        {
            Logger::Warn("CrashDebugHelper: Failed to read the hash list ({}).", e.what());
            g_Names.clear();
        }

        if (g_Names.empty())
        {
            Logger::Warn("CrashDebugHelper: Could not download the hash list, so save summaries will show resource ids instead of template names.");
            return;
        }

        g_NamesReady = true;
        Logger::Info("CrashDebugHelper: Loaded {} template names for save summaries.", g_Names.size());
    });
}

std::string ResourceNames::Name(uint64_t runtimeResourceId)
{
    if (g_NamesReady)
    {
        const auto it = g_Names.find(runtimeResourceId);

        if (it != g_Names.end())
        {
            return it->second;
        }
    }

    return fmt::format("{:016X}", runtimeResourceId);
}
