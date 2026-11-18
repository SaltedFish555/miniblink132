// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Ported and adapted for miniblink132.

#include "content/renderer/npapi/WebPluginImeWin.h"

#include <algorithm>
#include <cstring>

#include "base/strings/string_util.h"

#if defined(_WIN32)
#pragma comment(lib, "imm32.lib")
#endif

namespace content {

WebPluginIMEWin* WebPluginIMEWin::s_instance = nullptr;

namespace {
#if defined(_WIN32)
CRITICAL_SECTION* g_webPluginImeLock = nullptr;

DWORD WINAPI immGetPropertyStub(HKL, DWORD)
{
    return 0xd000a;
}

BOOL WINAPI immIsIMEStub(HKL)
{
    return FALSE;
}

bool importAddressTableHook(HMODULE hModule, LPCSTR targetImageName, LPCVOID targetFuncAddr, LPCVOID replaceFuncAddr)
{
    if (!hModule)
        return false;

    auto* baseAddr = reinterpret_cast<BYTE*>(hModule);
    auto* dosHdr = reinterpret_cast<IMAGE_DOS_HEADER*>(baseAddr);
    if (dosHdr->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* ntHdr = reinterpret_cast<IMAGE_NT_HEADERS*>(baseAddr + dosHdr->e_lfanew);
    if (ntHdr->Signature != IMAGE_NT_SIGNATURE)
        return false;

    DWORD importRva = ntHdr->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva)
        return false;

    auto* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(baseAddr + importRva);
    IMAGE_THUNK_DATA* thunkData = nullptr;

    while (importDesc->Characteristics != 0) {
        const char* dllName = reinterpret_cast<const char*>(baseAddr + importDesc->Name);
        if (base::EqualsCaseInsensitiveASCII(dllName, targetImageName)) {
            thunkData = reinterpret_cast<IMAGE_THUNK_DATA*>(baseAddr + importDesc->FirstThunk);
            break;
        }
        ++importDesc;
    }

    if (!thunkData)
        return false;

    while (thunkData->u1.Function) {
        auto* funcSlot = reinterpret_cast<ULONG_PTR*>(&thunkData->u1.Function);
        if (*funcSlot == reinterpret_cast<ULONG_PTR>(targetFuncAddr)) {
            DWORD oldProtect = 0;
            if (!::VirtualProtect(funcSlot, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect))
                return false;
            *funcSlot = reinterpret_cast<ULONG_PTR>(replaceFuncAddr);
            ::VirtualProtect(funcSlot, sizeof(ULONG_PTR), oldProtect, &oldProtect);
            return true;
        }
        ++thunkData;
    }
    return false;
}
#endif
} // namespace

WebPluginIMEWin::WebPluginIMEWin() = default;

void WebPluginIMEWin::compositionUpdated(
    const std::u16string& text, const std::vector<int>& clauses, const std::vector<int>& target, int cursorPosition)
{
#if defined(_WIN32)
    NPEvent npEvent {};
    if (!m_composingText) {
        m_composingText = true;
        m_resultText.clear();
        npEvent.event = WM_IME_STARTCOMPOSITION;
        m_events.push_back(npEvent);
    }

    npEvent.event = WM_IME_COMPOSITION;
    npEvent.wParam = 0;
    npEvent.lParam = GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS | GCS_DELTASTART;
    m_events.push_back(npEvent);

    m_compositionText = text;
    m_compositionClauses.assign(clauses.begin(), clauses.end());

    if (target.size() == 2) {
        m_compositionAttributes.assign(text.size(), ATTR_CONVERTED);
        for (int i = target[0]; i < target[1] && i < static_cast<int>(text.size()); ++i) {
            if (i >= 0)
                m_compositionAttributes[i] = ATTR_TARGET_CONVERTED;
        }
    } else {
        m_compositionAttributes.assign(text.size(), ATTR_INPUT);
    }

    m_cursorPosition = cursorPosition;
    m_deltaStart = cursorPosition;
#endif
}

void WebPluginIMEWin::compositionCompleted(const std::u16string& text)
{
#if defined(_WIN32)
    m_composingText = false;
    m_resultText = text;
    m_resultClauses[0] = 0;
    m_resultClauses[1] = static_cast<uint32_t>(m_resultText.size());
    m_cursorPosition = m_resultClauses[1];
    m_deltaStart = m_resultClauses[1];

    NPEvent npEvent {};
    npEvent.event = WM_IME_COMPOSITION;
    npEvent.lParam = GCS_CURSORPOS | GCS_DELTASTART | GCS_RESULTSTR | GCS_RESULTCLAUSE;
    m_events.push_back(npEvent);

    npEvent.event = WM_IME_ENDCOMPOSITION;
    npEvent.lParam = 0;
    m_events.push_back(npEvent);

    if (!m_supportImeMessages) {
        npEvent.event = WM_CHAR;
        npEvent.lParam = 0;
        for (char16_t ch : m_resultText) {
            npEvent.wParam = ch;
            m_events.push_back(npEvent);
        }
    }
#endif
}

bool WebPluginIMEWin::sendEvents(NPP instance, NPP_HandleEventProcPtr eventPtr)
{
    if (!eventPtr)
        return false;

    ScopedLock lock(this);
    bool ret = true;
    for (auto& ev : m_events) {
        if (!eventPtr(instance, &ev))
            ret = false;
    }
    m_events.clear();
    return ret;
}

bool WebPluginIMEWin::getStatus(int* inputType, gfx::Rect* caretRect) const
{
    *inputType = m_inputType;
    *caretRect = m_caretRect;
    return true;
}

void WebPluginIMEWin::lock()
{
#if defined(_WIN32)
    if (!g_webPluginImeLock) {
        g_webPluginImeLock = new CRITICAL_SECTION();
        ::InitializeCriticalSection(g_webPluginImeLock);
    }
    ::EnterCriticalSection(g_webPluginImeLock);
#endif
    s_instance = this;
}

void WebPluginIMEWin::unlock()
{
    s_instance = nullptr;
#if defined(_WIN32)
    ::LeaveCriticalSection(g_webPluginImeLock);
#endif
}

bool WebPluginIMEWin::hookGetProcAddress(HMODULE module)
{
#if defined(_WIN32)
    HMODULE kernel32 = ::GetModuleHandleA("kernel32.dll");
    if (!kernel32)
        return false;
    FARPROC origGetProcAddress = ::GetProcAddress(kernel32, "GetProcAddress");
    if (!origGetProcAddress)
        return false;
    return importAddressTableHook(
        module, "kernel32.dll", reinterpret_cast<LPCVOID>(origGetProcAddress), reinterpret_cast<LPCVOID>(&WebPluginIMEWin::GetProcAddressPatch));
#else
    return false;
#endif
}

#if defined(_WIN32)
FARPROC WebPluginIMEWin::getProcAddress(LPCSTR name)
{
    static const struct {
        const char* name;
        FARPROC function;
    } kImm32Functions[] = {
        { "ImmAssociateContextEx", reinterpret_cast<FARPROC>(&immAssociateContextEx) },
        { "ImmGetCompositionStringW", reinterpret_cast<FARPROC>(&immGetCompositionStringW) },
        { "ImmGetContext", reinterpret_cast<FARPROC>(&immGetContext) },
        { "ImmReleaseContext", reinterpret_cast<FARPROC>(&immReleaseContext) },
        { "ImmSetCandidateWindow", reinterpret_cast<FARPROC>(&immSetCandidateWindow) },
        { "ImmSetOpenStatus", reinterpret_cast<FARPROC>(&immSetOpenStatus) },
        { "ImmIsIME", reinterpret_cast<FARPROC>(&immIsIMEStub) },
        { "ImmGetProperty", reinterpret_cast<FARPROC>(&immGetPropertyStub) },
    };

    if (reinterpret_cast<ULONG_PTR>(name) <= 0xFFFF)
        return nullptr;

    for (const auto& entry : kImm32Functions) {
        if (!::lstrcmpiA(name, entry.name))
            return entry.function;
    }
    return nullptr;
}

FARPROC WINAPI WebPluginIMEWin::GetProcAddressPatch(HMODULE module, LPCSTR name)
{
    if (FARPROC immFn = getProcAddress(name))
        return immFn;
    return ::GetProcAddress(module, name);
}

WebPluginIMEWin* WebPluginIMEWin::getInstance(HIMC context)
{
    return (s_instance && context == reinterpret_cast<HIMC>(s_instance)) ? s_instance : nullptr;
}

BOOL WINAPI WebPluginIMEWin::immAssociateContextEx(HWND window, HIMC context, DWORD flags)
{
    WebPluginIMEWin* instance = getInstance(context);
    if (!instance)
        return ::ImmAssociateContextEx(window, context, flags);

    instance->m_inputType = (!context && !flags) ? 1 : 0;
    instance->m_statusUpdated = true;
    return TRUE;
}

LONG WINAPI WebPluginIMEWin::immGetCompositionStringW(HIMC context, DWORD index, LPVOID dstData, DWORD dstSize)
{
    WebPluginIMEWin* instance = getInstance(context);
    if (!instance)
        return ::ImmGetCompositionStringW(context, index, dstData, dstSize);

    const void* srcData = nullptr;
    DWORD srcSize = 0;
    switch (index) {
    case GCS_COMPSTR:
        srcData = instance->m_compositionText.data();
        srcSize = static_cast<DWORD>(instance->m_compositionText.size() * sizeof(char16_t));
        break;
    case GCS_COMPATTR:
    case GCS_COMPCLAUSE:
        return 0;
    case GCS_CURSORPOS:
        return instance->m_cursorPosition;
    case GCS_DELTASTART:
        return instance->m_deltaStart;
    case GCS_RESULTSTR:
        srcData = instance->m_resultText.data();
        srcSize = static_cast<DWORD>(instance->m_resultText.size() * sizeof(char16_t));
        break;
    case GCS_RESULTCLAUSE:
        srcData = &instance->m_resultClauses[0];
        srcSize = sizeof(instance->m_resultClauses);
        break;
    default:
        break;
    }

    if (!srcData || !srcSize)
        return IMM_ERROR_NODATA;
    if (dstData && dstSize >= srcSize)
        memcpy(dstData, srcData, srcSize);
    return static_cast<LONG>(srcSize);
}

HIMC WINAPI WebPluginIMEWin::immGetContext(HWND window)
{
    if (::IsWindow(window)) {
        wchar_t name[128] = { 0 };
        ::GetClassNameW(window, name, 128);
        if (!wcscmp(name, L"NativeWindowClass"))
            return ::ImmGetContext(window);
    }

    if (s_instance)
        s_instance->m_supportImeMessages = true;
    return reinterpret_cast<HIMC>(s_instance);
}

BOOL WINAPI WebPluginIMEWin::immReleaseContext(HWND window, HIMC context)
{
    if (!getInstance(context))
        return ::ImmReleaseContext(window, context);
    return TRUE;
}

BOOL WINAPI WebPluginIMEWin::immSetCandidateWindow(HIMC context, CANDIDATEFORM* candidate)
{
    WebPluginIMEWin* instance = getInstance(context);
    if (!instance)
        return ::ImmSetCandidateWindow(context, candidate);

    if (candidate) {
        gfx::Rect rect(candidate->rcArea.left, candidate->rcArea.top,
            candidate->rcArea.right - candidate->rcArea.left, candidate->rcArea.bottom - candidate->rcArea.top);
        if (instance->m_caretRect != rect) {
            instance->m_caretRect = rect;
            instance->m_statusUpdated = true;
        }
    }
    return TRUE;
}

BOOL WINAPI WebPluginIMEWin::immSetOpenStatus(HIMC context, BOOL open)
{
    WebPluginIMEWin* instance = getInstance(context);
    if (!instance)
        return ::ImmSetOpenStatus(context, open);

    int inputType = open ? 1 : 0;
    if (instance->m_inputType != inputType) {
        instance->m_inputType = inputType;
        instance->m_statusUpdated = true;
    }
    return TRUE;
}
#endif // defined(_WIN32)

} // namespace content
