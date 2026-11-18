// Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
// Ported and adapted for miniblink132.

#include "content/renderer/npapi/PluginDatabase.h"

#include <algorithm>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

#if defined(_WIN32)
#include <shlwapi.h>
#endif

namespace content {

namespace {

bool g_npapiPluginsEnabled = true;
mbGetPluginListCallback g_pluginListCallback = nullptr;
void* g_pluginListCallbackParam = nullptr;

#if defined(_WIN32)
WTF::String moduleBaseDirectory(HMODULE mod)
{
    WCHAR moduleFileNameStr[MAX_PATH] = { 0 };
    DWORD len = ::GetModuleFileNameW(mod, moduleFileNameStr, MAX_PATH);
    if (!len || len == MAX_PATH)
        return WTF::String();
    if (!::PathRemoveFileSpecW(moduleFileNameStr))
        return WTF::String();
    return WTF::String::FromUTF8(base::WideToUTF8(moduleFileNameStr));
}

WTF::String currentDllDirectory()
{
    HMODULE selfMod = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&currentDllDirectory), &selfMod)) {
        return WTF::String();
    }
    return moduleBaseDirectory(selfMod);
}

WTF::String appPluginsDirectory()
{
    WTF::String exeDir = moduleBaseDirectory(nullptr);
    if (exeDir.empty())
        return WTF::String();
    return WTF::String::FromUTF8(exeDir.Utf8() + "\\plugins");
}

void pushUniqueDirectory(std::vector<WTF::String>& directories, const WTF::String& dir)
{
    if (dir.empty())
        return;
    std::string dirUtf8 = dir.Utf8();
    for (const auto& existing : directories) {
        if (base::EqualsCaseInsensitiveASCII(existing.Utf8(), dirUtf8))
            return;
    }
    directories.push_back(dir);
}

void addPluginPathsFromRegistry(HKEY rootKey, std::set<std::string>& paths)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(rootKey, L"Software\\MozillaPlugins", 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
        return;

    wchar_t name[128];
    FILETIME lastModified;
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = 128;
        if (::RegEnumKeyExW(key, i, name, &nameLen, nullptr, nullptr, nullptr, &lastModified) != ERROR_SUCCESS)
            break;

        WCHAR pathBuf[MAX_PATH] = { 0 };
        DWORD pathBytes = sizeof(pathBuf);
        DWORD type = 0;
        if (::SHGetValueW(key, name, L"Path", &type, pathBuf, &pathBytes) == ERROR_SUCCESS
            && type == REG_SZ && pathBytes > sizeof(WCHAR)) {
            paths.insert(base::WideToUTF8(pathBuf));
        }
    }
    ::RegCloseKey(key);
}

void addMacromediaPluginDirectories(std::vector<WTF::String>& directories)
{
    // Note: On x64 process GetSystemDirectoryW returns System32 (64-bit Flash);
    // on WOW64 x86 process GetSystemDirectoryW redirects to SysWOW64 (32-bit Flash).
    WCHAR systemDirectoryStr[MAX_PATH] = { 0 };
    if (::GetSystemDirectoryW(systemDirectoryStr, MAX_PATH)) {
        WCHAR macromediaDir[MAX_PATH] = { 0 };
        ::PathCombineW(macromediaDir, systemDirectoryStr, L"Macromed\\Flash");
        pushUniqueDirectory(directories, WTF::String::FromUTF8(base::WideToUTF8(macromediaDir)));
    }
}
#endif

base::FilePath utf8ToFilePath(const std::string& utf8Path)
{
    return base::FilePath::FromUTF8Unsafe(utf8Path);
}

bool getPathModifiedTime(const std::string& utf8Path, time_t* outModified)
{
    base::File::Info info;
    if (!base::GetFileInfo(utf8ToFilePath(utf8Path), &info) || info.is_directory)
        return false;
    *outModified = info.last_modified.ToTimeT();
    return true;
}

} // namespace

bool isNpapiPluginsEnabled()
{
    return g_npapiPluginsEnabled;
}

void setNpapiPluginsEnabled(bool enabled)
{
    g_npapiPluginsEnabled = enabled;
}

void setNpapiPluginListCallback(mbGetPluginListCallback callback, void* param)
{
    g_pluginListCallback = callback;
    g_pluginListCallbackParam = param;
}

mbGetPluginListCallback getNpapiPluginListCallback()
{
    return g_pluginListCallback;
}

void* getNpapiPluginListCallbackParam()
{
    return g_pluginListCallbackParam;
}

PluginDatabase* PluginDatabase::installedPlugins(bool populate)
{
    static PluginDatabase* s_plugins = nullptr;
    if (!s_plugins) {
        s_plugins = new PluginDatabase();
        if (populate) {
            s_plugins->setPluginDirectories(PluginDatabase::defaultPluginDirectories());
            s_plugins->refresh();
        }
    }
    return s_plugins;
}

bool PluginDatabase::isMIMETypeRegistered(const std::string& mimeType)
{
    if (mimeType.empty())
        return false;
    std::string lower = base::ToLowerASCII(mimeType);
    if (m_registeredMIMETypes.count(lower))
        return true;
    return refresh() && m_registeredMIMETypes.count(lower) > 0;
}

void PluginDatabase::addExtraPluginDirectory(const WTF::String& directory)
{
    if (directory.empty())
        return;
    for (const auto& existing : m_pluginDirectories) {
        if (existing == directory) {
            refresh();
            return;
        }
    }
    m_pluginDirectories.push_back(directory);
    refresh();
}

bool PluginDatabase::refresh()
{
    bool pluginSetChanged = false;

    if (!m_plugins.empty()) {
        std::vector<PluginPackage*> pluginsToUnload;
        getDeletedPlugins(pluginsToUnload);
        for (PluginPackage* pkg : pluginsToUnload)
            remove(pkg);
        pluginSetChanged = !pluginsToUnload.empty();
    }

    std::set<std::string> paths;
    getPluginPathsInDirectories(paths);

    std::map<std::string, time_t> pathsWithTimes;
    bool shouldSkipUnchangedFiles = !pluginSetChanged;

    for (const std::string& path : paths) {
        time_t lastModified = 0;
        if (!getPathModifiedTime(path, &lastModified))
            continue;

        pathsWithTimes[path] = lastModified;

        auto timeIt = m_pluginPathsWithTimes.find(path);
        if (shouldSkipUnchangedFiles && timeIt != m_pluginPathsWithTimes.end() && timeIt->second == lastModified)
            continue;

        auto oldIt = m_pluginsByPath.find(path);
        if (oldIt != m_pluginsByPath.end())
            remove(oldIt->second.get());

        scoped_refptr<PluginPackage> package = PluginPackage::createPackage(WTF::String::FromUTF8(path), lastModified);
        if (package && add(std::move(package)))
            pluginSetChanged = true;
    }

    m_pluginPathsWithTimes.swap(pathsWithTimes);

    if (!pluginSetChanged)
        return false;

    m_registeredMIMETypes.clear();
    for (const auto& pkg : m_plugins) {
        for (const auto& mimePair : pkg->mimeToDescriptions())
            m_registeredMIMETypes.insert(mimePair.first);
    }
    return true;
}

std::vector<PluginPackage*> PluginDatabase::plugins() const
{
    std::vector<PluginPackage*> result;
    result.reserve(m_plugins.size());
    for (const auto& pkg : m_plugins)
        result.push_back(pkg.get());
    return result;
}

PluginPackage* PluginDatabase::pluginForMIMEType(const std::string& mimeType)
{
    if (mimeType.empty())
        return nullptr;

    std::string key = base::ToLowerASCII(mimeType);
    auto prefIt = m_preferredPlugins.find(key);
    if (prefIt != m_preferredPlugins.end() && prefIt->second && prefIt->second->isEnabled()
        && prefIt->second->mimeToDescriptions().count(key)) {
        return prefIt->second.get();
    }

    std::vector<PluginPackage*> choices;
    for (const auto& pkg : m_plugins) {
        if (pkg->isEnabled() && pkg->mimeToDescriptions().count(key))
            choices.push_back(pkg.get());
    }
    if (choices.empty())
        return nullptr;

    std::sort(choices.begin(), choices.end(),
        [](const PluginPackage* a, const PluginPackage* b) { return a->compare(*b) < 0; });
    return choices[0];
}

std::string PluginDatabase::MIMETypeForExtension(const std::string& extension) const
{
    if (extension.empty())
        return std::string();

    std::string lowerExt = base::ToLowerASCII(extension);
    if (!lowerExt.empty() && lowerExt[0] == '.')
        lowerExt.erase(0, 1);

    std::vector<PluginPackage*> choices;
    std::map<PluginPackage*, std::string> mimeForPlugin;

    for (const auto& pkg : m_plugins) {
        if (!pkg->isEnabled())
            continue;
        for (const auto& kv : pkg->mimeToExtensions()) {
            auto prefIt = m_preferredPlugins.find(kv.first);
            bool found = false;
            for (const std::string& ext : kv.second) {
                if (base::EqualsCaseInsensitiveASCII(ext, lowerExt)) {
                    if (prefIt != m_preferredPlugins.end() && prefIt->second && PluginPackage::equal(*pkg, *prefIt->second))
                        return kv.first;
                    choices.push_back(pkg.get());
                    mimeForPlugin[pkg.get()] = kv.first;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    if (choices.empty())
        return std::string();

    std::sort(choices.begin(), choices.end(),
        [](const PluginPackage* a, const PluginPackage* b) { return a->compare(*b) < 0; });
    return mimeForPlugin[choices[0]];
}

PluginPackage* PluginDatabase::findPlugin(const blink::KURL& url, std::string& mimeType)
{
    if (!isNpapiPluginsEnabled())
        return nullptr;

    if (!mimeType.empty())
        return pluginForMIMEType(mimeType);

    std::string path = url.GetPath().Utf8();
    if (path.empty() || path.back() == '/')
        return nullptr;

    size_t dotPos = path.rfind('.');
    if (dotPos == std::string::npos)
        return nullptr;

    std::string mimeForExt = MIMETypeForExtension(path.substr(dotPos + 1));
    PluginPackage* plugin = pluginForMIMEType(mimeForExt);
    if (!plugin)
        return nullptr;

    mimeType = mimeForExt;
    return plugin;
}

void PluginDatabase::setPreferredPluginForMIMEType(const std::string& mimeType, PluginPackage* plugin)
{
    std::string lower = base::ToLowerASCII(mimeType);
    if (!plugin) {
        m_preferredPlugins.erase(lower);
        return;
    }
    if (plugin->mimeToExtensions().count(lower))
        m_preferredPlugins[lower] = plugin;
}

bool PluginDatabase::fileExistsAndIsNotDisabled(const std::string& filePath) const
{
    size_t slashPos = filePath.find_last_of("\\/");
    std::string fileName = (slashPos == std::string::npos) ? filePath : filePath.substr(slashPos + 1);
    if (m_disabledPluginFiles.count(base::ToLowerASCII(fileName)))
        return false;
    return base::PathExists(utf8ToFilePath(filePath));
}

void PluginDatabase::getDeletedPlugins(std::vector<PluginPackage*>& plugins) const
{
    for (const auto& pkg : m_plugins) {
        if (pkg->isVirtual())
            continue;
        if (!fileExistsAndIsNotDisabled(pkg->path().Utf8()))
            plugins.push_back(pkg.get());
    }
}

bool PluginDatabase::add(scoped_refptr<PluginPackage> package)
{
    if (!package)
        return false;
    for (const auto& existing : m_plugins) {
        if (PluginPackage::equal(*existing, *package))
            return false;
    }
    m_pluginsByPath[package->path().Utf8()] = package;
    m_plugins.push_back(std::move(package));
    return true;
}

void PluginDatabase::remove(PluginPackage* package)
{
    for (const auto& kv : package->mimeToExtensions()) {
        auto it = m_preferredPlugins.find(kv.first);
        if (it != m_preferredPlugins.end() && it->second.get() == package)
            m_preferredPlugins.erase(it);
    }
    m_pluginsByPath.erase(package->path().Utf8());
    m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
                        [package](const scoped_refptr<PluginPackage>& p) { return p.get() == package; }),
        m_plugins.end());
}

void PluginDatabase::clear()
{
    m_plugins.clear();
    m_pluginsByPath.clear();
    m_pluginPathsWithTimes.clear();
    m_registeredMIMETypes.clear();
    m_preferredPlugins.clear();
}

void PluginDatabase::getPluginPathsInDirectories(std::set<std::string>& paths) const
{
#if defined(_WIN32)
    for (const WTF::String& dirStr : m_pluginDirectories) {
        std::wstring wideDir = base::UTF8ToWide(dirStr.Utf8());
        if (wideDir.empty())
            continue;

        // Also allow passing a direct DLL file path in mbAddPluginDirectory.
        DWORD attr = ::GetFileAttributesW(wideDir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            paths.insert(base::WideToUTF8(wideDir));
            continue;
        }

        std::wstring pattern = wideDir + L"\\*";
        WIN32_FIND_DATAW findData;
        HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string fileName = base::WideToUTF8(findData.cFileName);
            std::string lowerName = base::ToLowerASCII(fileName);
            if (!base::EndsWith(lowerName, ".dll", base::CompareCase::SENSITIVE))
                continue;
            if (base::StartsWith(lowerName, "pepflash", base::CompareCase::SENSITIVE)
                || base::StartsWith(lowerName, "mb132", base::CompareCase::SENSITIVE)
                || base::StartsWith(lowerName, "miniblink", base::CompareCase::SENSITIVE)) {
                continue;
            }
            bool isCandidateDll = base::StartsWith(lowerName, "np", base::CompareCase::SENSITIVE)
                || lowerName.find("flash") != std::string::npos;
            if (!isCandidateDll)
                continue;

            std::wstring fullPath = wideDir + L"\\" + findData.cFileName;
            paths.insert(base::WideToUTF8(fullPath));
        } while (::FindNextFileW(hFind, &findData) != 0);

        ::FindClose(hFind);
    }

    addPluginPathsFromRegistry(HKEY_LOCAL_MACHINE, paths);
    addPluginPathsFromRegistry(HKEY_CURRENT_USER, paths);
#endif
}

std::vector<WTF::String> PluginDatabase::defaultPluginDirectories()
{
    std::vector<WTF::String> directories;
#if defined(_WIN32)
    WTF::String exeDir = moduleBaseDirectory(nullptr);
    if (!exeDir.empty()) {
        pushUniqueDirectory(directories, WTF::String::FromUTF8(exeDir.Utf8() + "\\plugins"));
        pushUniqueDirectory(directories, exeDir);
    }
    WTF::String dllDir = currentDllDirectory();
    if (!dllDir.empty()) {
        pushUniqueDirectory(directories, WTF::String::FromUTF8(dllDir.Utf8() + "\\plugins"));
        pushUniqueDirectory(directories, dllDir);
    }
    addMacromediaPluginDirectories(directories);
#endif
    return directories;
}

bool PluginDatabase::isPreferredPluginDirectory(const WTF::String& directory)
{
#if defined(_WIN32)
    if (directory.empty())
        return false;
    std::string dirUtf8 = directory.Utf8();
    WTF::String exeDir = moduleBaseDirectory(nullptr);
    if (!exeDir.empty()) {
        std::string exeUtf8 = exeDir.Utf8();
        if (base::EqualsCaseInsensitiveASCII(dirUtf8, exeUtf8)
            || base::EqualsCaseInsensitiveASCII(dirUtf8, exeUtf8 + "\\plugins")) {
            return true;
        }
    }
    WTF::String dllDir = currentDllDirectory();
    if (!dllDir.empty()) {
        std::string dllUtf8 = dllDir.Utf8();
        if (base::EqualsCaseInsensitiveASCII(dirUtf8, dllUtf8)
            || base::EqualsCaseInsensitiveASCII(dirUtf8, dllUtf8 + "\\plugins")) {
            return true;
        }
    }
#endif
    return false;
}

} // namespace content
