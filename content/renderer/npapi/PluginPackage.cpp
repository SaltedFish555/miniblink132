// Copyright (C) 2006, 2008 Apple Inc. All rights reserved.
// Ported and adapted for miniblink132.

#include "content/renderer/npapi/PluginPackage.h"

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/renderer/npapi/PluginDatabase.h"
#include "content/renderer/npapi/npruntime_priv.h"
#include <string.h>

#if defined(_WIN32)
#include <shlwapi.h>
#include <winver.h>
#endif

namespace content {

namespace {

#if defined(_WIN32)
WTF::String getVersionInfoField(const LPVOID versionInfoData, const std::wstring& info)
{
    LPVOID buffer = nullptr;
    UINT bufferLength = 0;
    std::wstring subInfo = L"\\StringFileInfo\\040904E4\\" + info;
    BOOL retval = ::VerQueryValueW(versionInfoData, subInfo.c_str(), &buffer, &bufferLength);
    if (!retval || bufferLength == 0) {
        subInfo = L"\\StringFileInfo\\080404b0\\" + info;
        retval = ::VerQueryValueW(versionInfoData, subInfo.c_str(), &buffer, &bufferLength);
        if (!retval || bufferLength == 0)
            return WTF::String();
    }
    return WTF::String(reinterpret_cast<const UChar*>(buffer), bufferLength - 1);
}
#endif

} // namespace

PluginPackage::PluginPackage(const WTF::String& path, time_t lastModified)
    : m_path(path)
    , m_lastModified(lastModified)
{
    std::string utf8Path = path.Utf8();
    size_t slashPos = utf8Path.find_last_of("\\/");
    if (slashPos != std::string::npos) {
        m_fileName = WTF::String::FromUTF8(utf8Path.substr(slashPos + 1));
        m_parentDirectory = WTF::String::FromUTF8(utf8Path.substr(0, slashPos));
    } else {
        m_fileName = path;
    }
}

PluginPackage::~PluginPackage()
{
    if (!m_loadCount)
        unloadWithoutShutdown();
    else
        unload();
}

void PluginPackage::freeLibrarySoon()
{
    if (!m_freeLibraryTimer.IsRunning()) {
        m_freeLibraryTimer.Start(FROM_HERE, base::TimeDelta(),
            base::BindOnce(&PluginPackage::freeLibraryTimerFired, base::Unretained(this)));
    }
}

void PluginPackage::freeLibraryTimerFired()
{
    if (!m_loadCount) {
        // Intentional: do NOT call ::FreeLibrary(m_module) because lingering NPObjects in
        // liveObjectMap may still reference vtables inside the plugin DLL (ported from miniblink49).
        m_module = nullptr;
    }
}

void PluginPackage::unload()
{
    if (!m_isLoaded)
        return;
    if (--m_loadCount > 0)
        return;
    if (m_NPP_Shutdown)
        m_NPP_Shutdown();
    unloadWithoutShutdown();
}

void PluginPackage::unloadWithoutShutdown()
{
    if (!m_isLoaded)
        return;
    freeLibrarySoon();
    m_isLoaded = false;
}

scoped_refptr<PluginPackage> PluginPackage::createPackage(const WTF::String& path, time_t lastModified)
{
    scoped_refptr<PluginPackage> package(new PluginPackage(path, lastModified));
    if (!package->fetchInfo())
        return nullptr;
    return package;
}

scoped_refptr<PluginPackage> PluginPackage::createVirtualPackage(
    NP_InitializeFuncPtr npInitialize, NP_GetEntryPointsFuncPtr npGetEntryPoints, NPP_ShutdownProcPtr nppShutdown)
{
    static int s_count = 0;
    std::string descStd = "application/virtual-plugin-" + std::to_string(s_count++);
    WTF::String description = WTF::String::FromUTF8(descStd);

    PluginDatabase* database = PluginDatabase::installedPlugins();
    if (PluginPackage* existing = database->pluginForMIMEType(descStd))
        return existing;

    scoped_refptr<PluginPackage> package(new PluginPackage(WTF::String(), 0));
    package->m_isVirtual = true;
    package->m_NP_GetEntryPoints = npGetEntryPoints;
    package->m_NP_Initialize = npInitialize;
    package->m_NPP_Shutdown = nppShutdown;
    package->m_name = "VirtualPlugin";
    package->m_description = description;
    package->m_mimeToDescriptions[descStd] = description;
    package->m_mimeToExtensions[descStd] = { descStd };

    database->addVirtualPlugin(package);
    database->setPreferredPluginForMIMEType(descStd, package.get());
    return package;
}

bool PluginPackage::isPluginBlacklisted() const
{
    if (m_name == "Citrix ICA Client")
        return true;
    std::string lowerFile = base::ToLowerASCII(m_fileName.Utf8());
    if (lowerFile == "npmozax.dll" || lowerFile == "npwpf.dll")
        return true;
    return false;
}

void PluginPackage::determineQuirks(const std::string& mimeType)
{
    if (mimeType == "application/x-shockwave-flash") {
        static const PlatformModuleVersion flashTenVersion(0x00000000, 0x000a0000);
        if (compareFileVersion(flashTenVersion) < 0)
            m_quirks.add(PluginQuirkWantsMozillaUserAgent);

        m_quirks.add(PluginQuirkThrottleInvalidate);
        m_quirks.add(PluginQuirkThrottleWMUserPlusOneMessages);
        m_quirks.add(PluginQuirkFlashURLNotifyBug);
        m_quirks.add(PluginQuirkEmulateIme);
    }

    std::string nameUtf8 = m_name.Utf8();
    if (nameUtf8.find("Microsoft") != std::string::npos && nameUtf8.find("Windows Media") != std::string::npos) {
        m_quirks.add(PluginQuirkDeferFirstSetWindowCall);
        m_quirks.add(PluginQuirkRemoveWindowlessVideoParam);
        m_quirks.add(PluginQuirkHasModalMessageLoop);
    }

    if (m_name == "VLC Multimedia Plugin" || m_name == "VLC Multimedia Plug-in") {
        m_quirks.add(PluginQuirkDontSetNullWindowHandleOnDestroy);
        m_quirks.add(PluginQuirkDontAllowMultipleInstances);
    }

    if (mimeType == "video/divx")
        m_quirks.add(PluginQuirkDeferFirstSetWindowCall);

    if (mimeType == "application/x-silverlight")
        m_quirks.add(PluginQuirkDontUnloadPlugin);
}

bool PluginPackage::fetchInfo()
{
#if defined(_WIN32)
    std::wstring widePath = base::UTF8ToWide(m_path.Utf8());
    DWORD zeroHandle = 0;
    DWORD versionInfoSize = ::GetFileVersionInfoSizeW(widePath.c_str(), &zeroHandle);
    if (versionInfoSize == 0)
        return false;

    std::vector<char> versionInfoData(versionInfoSize * 2);
    if (!::GetFileVersionInfoW(widePath.c_str(), 0, versionInfoSize, versionInfoData.data()))
        return false;

    m_name = getVersionInfoField(versionInfoData.data(), L"ProductName");
    m_description = getVersionInfoField(versionInfoData.data(), L"FileDescription");
    if (m_name.IsNull() || m_description.IsNull())
        return false;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!::VerQueryValueW(versionInfoData.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize)
        || infoSize < sizeof(VS_FIXEDFILEINFO)) {
        return false;
    }
    m_moduleVersion.leastSig = info->dwFileVersionLS;
    m_moduleVersion.mostSig = info->dwFileVersionMS;

    if (isPluginBlacklisted())
        return false;

    std::vector<std::string> types = base::SplitString(
        getVersionInfoField(versionInfoData.data(), L"MIMEType").Utf8(), "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    std::vector<std::string> extensionLists = base::SplitString(
        getVersionInfoField(versionInfoData.data(), L"FileExtents").Utf8(), "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    std::vector<std::string> descriptions = base::SplitString(
        getVersionInfoField(versionInfoData.data(), L"FileOpenName").Utf8(), "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

    if (types.empty())
        return false;

    for (size_t i = 0; i < types.size(); ++i) {
        std::string type = base::ToLowerASCII(types[i]);
        std::string desc = i < descriptions.size() ? descriptions[i] : "";
        std::string extList = i < extensionLists.size() ? extensionLists[i] : "";

        std::vector<std::string> exts = base::SplitString(extList, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
        for (auto& ext : exts) {
            if (!ext.empty() && ext[0] == '.')
                ext.erase(0, 1);
            ext = base::ToLowerASCII(ext);
        }

        size_t pos = desc.find("(*");
        if (pos != std::string::npos) {
            if (pos > 0 && desc[pos - 1] == ' ')
                --pos;
            desc = desc.substr(0, pos);
        }

        determineQuirks(type);
        m_mimeToExtensions[type] = exts;
        m_mimeToDescriptions[type] = WTF::String::FromUTF8(desc);
    }
    return true;
#else
    return false;
#endif
}

bool PluginPackage::load()
{
    if (m_isVirtual) {
        if (m_isLoaded)
            return true;
        if (doLoad()) {
            m_isLoaded = true;
            ++m_loadCount;
            return true;
        }
        return false;
    }

    if (m_freeLibraryTimer.IsRunning()) {
        m_freeLibraryTimer.Stop();
    } else if (m_isLoaded) {
        if (m_quirks.contains(PluginQuirkDontAllowMultipleInstances))
            return false;
        ++m_loadCount;
        return true;
    } else {
#if defined(_WIN32)
        WCHAR currentPath[MAX_PATH] = { 0 };
        if (!::GetCurrentDirectoryW(MAX_PATH, currentPath))
            return false;

        std::wstring widePath = base::UTF8ToWide(m_path.Utf8());
        std::wstring parentDir = base::UTF8ToWide(m_parentDirectory.Utf8());
        if (!parentDir.empty())
            ::SetCurrentDirectoryW(parentDir.c_str());

        m_module = ::LoadLibraryExW(widePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        ::SetCurrentDirectoryW(currentPath);
#endif
    }

    if (!m_module)
        return false;

    m_isLoaded = true;

#if defined(_WIN32)
    m_NP_Initialize = reinterpret_cast<NP_InitializeFuncPtr>(::GetProcAddress(m_module, "NP_Initialize"));
    m_NP_GetEntryPoints = reinterpret_cast<NP_GetEntryPointsFuncPtr>(::GetProcAddress(m_module, "NP_GetEntryPoints"));
    m_NPP_Shutdown = reinterpret_cast<NPP_ShutdownProcPtr>(::GetProcAddress(m_module, "NP_Shutdown"));
#endif

    if (!m_NP_Initialize || !m_NP_GetEntryPoints || !m_NPP_Shutdown || !doLoad()) {
        unloadWithoutShutdown();
        return false;
    }

    ++m_loadCount;
    return true;
}

bool PluginPackage::doLoad()
{
    memset(&m_pluginFuncs, 0, sizeof(m_pluginFuncs));
    m_pluginFuncs.size = sizeof(m_pluginFuncs);

    if (m_NP_GetEntryPoints(&m_pluginFuncs) != NPERR_NO_ERROR)
        return false;

    initializeBrowserFuncs();

    if (m_NP_Initialize(&m_browserFuncs) != NPERR_NO_ERROR)
        return false;

    return true;
}

void PluginPackage::initializeBrowserFuncs()
{
    memset(&m_browserFuncs, 0, sizeof(m_browserFuncs));
    m_browserFuncs.size = sizeof(m_browserFuncs);
    m_browserFuncs.version = NPVersion();

    m_browserFuncs.geturl = NPN_GetURL;
    m_browserFuncs.posturl = NPN_PostURL;
    m_browserFuncs.requestread = NPN_RequestRead;
    m_browserFuncs.newstream = NPN_NewStream;
    m_browserFuncs.write = NPN_Write;
    m_browserFuncs.destroystream = NPN_DestroyStream;
    m_browserFuncs.status = NPN_Status;
    m_browserFuncs.uagent = NPN_UserAgent;
    m_browserFuncs.memalloc = NPN_MemAlloc;
    m_browserFuncs.memfree = NPN_MemFree;
    m_browserFuncs.memflush = NPN_MemFlush;
    m_browserFuncs.reloadplugins = NPN_ReloadPlugins;
    m_browserFuncs.geturlnotify = NPN_GetURLNotify;
    m_browserFuncs.posturlnotify = NPN_PostURLNotify;
    m_browserFuncs.getvalue = NPN_GetValue;
    m_browserFuncs.setvalue = NPN_SetValue;
    m_browserFuncs.invalidaterect = NPN_InvalidateRect;
    m_browserFuncs.invalidateregion = NPN_InvalidateRegion;
    m_browserFuncs.forceredraw = NPN_ForceRedraw;
    m_browserFuncs.getJavaEnv = NPN_GetJavaEnv;
    m_browserFuncs.getJavaPeer = NPN_GetJavaPeer;
    m_browserFuncs.pushpopupsenabledstate = NPN_PushPopupsEnabledState;
    m_browserFuncs.poppopupsenabledstate = NPN_PopPopupsEnabledState;
    m_browserFuncs.pluginthreadasynccall = NPN_PluginThreadAsyncCall;

    m_browserFuncs.releasevariantvalue = _NPN_ReleaseVariantValue;
    m_browserFuncs.getstringidentifier = _NPN_GetStringIdentifier;
    m_browserFuncs.getstringidentifiers = _NPN_GetStringIdentifiers;
    m_browserFuncs.getintidentifier = _NPN_GetIntIdentifier;
    m_browserFuncs.identifierisstring = _NPN_IdentifierIsString;
    m_browserFuncs.utf8fromidentifier = _NPN_UTF8FromIdentifier;
    m_browserFuncs.intfromidentifier = _NPN_IntFromIdentifier;
    m_browserFuncs.createobject = _NPN_CreateObject;
    m_browserFuncs.retainobject = _NPN_RetainObject;
    m_browserFuncs.releaseobject = _NPN_ReleaseObject;
    m_browserFuncs.invoke = _NPN_Invoke;
    m_browserFuncs.invokeDefault = _NPN_InvokeDefault;
    m_browserFuncs.evaluate = _NPN_Evaluate;
    m_browserFuncs.getproperty = _NPN_GetProperty;
    m_browserFuncs.setproperty = _NPN_SetProperty;
    m_browserFuncs.removeproperty = _NPN_RemoveProperty;
    m_browserFuncs.hasproperty = _NPN_HasProperty;
    m_browserFuncs.hasmethod = _NPN_HasMethod;
    m_browserFuncs.setexception = _NPN_SetException;
    m_browserFuncs.enumerate = _NPN_Enumerate;
    m_browserFuncs.construct = _NPN_Construct;
    m_browserFuncs.getvalueforurl = NPN_GetValueForURL;
    m_browserFuncs.setvalueforurl = NPN_SetValueForURL;
    m_browserFuncs.getauthenticationinfo = NPN_GetAuthenticationInfo;
    m_browserFuncs.popupcontextmenu = NPN_PopUpContextMenu;
}

int PluginPackage::compareFileVersion(const PlatformModuleVersion& compareVersion) const
{
    if (m_moduleVersion.mostSig != compareVersion.mostSig)
        return m_moduleVersion.mostSig > compareVersion.mostSig ? 1 : -1;
    if (m_moduleVersion.leastSig != compareVersion.leastSig)
        return m_moduleVersion.leastSig > compareVersion.leastSig ? 1 : -1;
    return 0;
}

int PluginPackage::compare(const PluginPackage& compareTo) const
{
    bool aMulti = !quirks().contains(PluginQuirkDontAllowMultipleInstances);
    bool bMulti = !compareTo.quirks().contains(PluginQuirkDontAllowMultipleInstances);
    if (aMulti != bMulti)
        return aMulti ? -1 : 1;

    bool aPref = PluginDatabase::isPreferredPluginDirectory(parentDirectory());
    bool bPref = PluginDatabase::isPreferredPluginDirectory(compareTo.parentDirectory());
    if (aPref != bPref)
        return aPref ? -1 : 1;

    int diff = strcmp(name().Utf8().c_str(), compareTo.name().Utf8().c_str());
    if (diff)
        return diff;

    diff = compareFileVersion(compareTo.version());
    if (diff)
        return diff;

    return strcmp(parentDirectory().Utf8().c_str(), compareTo.parentDirectory().Utf8().c_str());
}

bool PluginPackage::equal(const PluginPackage& a, const PluginPackage& b)
{
    if (a.m_name != b.m_name || a.m_description != b.m_description)
        return false;
    if (a.m_mimeToExtensions.size() != b.m_mimeToExtensions.size())
        return false;
    for (const auto& kv : a.m_mimeToExtensions) {
        if (b.m_mimeToExtensions.find(kv.first) == b.m_mimeToExtensions.end())
            return false;
    }
    return true;
}

} // namespace content
