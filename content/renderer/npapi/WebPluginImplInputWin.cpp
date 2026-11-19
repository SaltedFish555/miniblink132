// Copyright (C) 2006-2013 Apple Inc. All rights reserved.
// Ported and adapted for miniblink132 (Mouse / Wheel / Keyboard / IME input).

#include "content/renderer/npapi/WebPluginImpl.h"

#include <algorithm>
#include <cmath>

#include "base/strings/utf_string_conversions.h"
#include "content/renderer/npapi/WebPluginImeWin.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/base/ime/ime_text_span.h"

namespace content {

void WebPluginImpl::UpdateFocus(bool focused, blink::mojom::FocusType)
{
    if (!m_plugin || !m_plugin->pluginFuncs()->event || !m_instance)
        return;

#if defined(_WIN32)
    scoped_refptr<WebPluginImpl> protect(this);
    NPEvent focusEvent {};
    focusEvent.event = focused ? WM_SETFOCUS : WM_KILLFOCUS;
    focusEvent.wParam = 0;
    focusEvent.lParam = 0;

    if (!m_pluginIme && m_plugin->quirks().contains(PluginQuirkEmulateIme))
        m_pluginIme = std::make_unique<WebPluginIMEWin>();

    m_plugin->pluginFuncs()->event(m_instance, &focusEvent);
#endif
}

bool WebPluginImpl::handleMouseEvent(const blink::WebInputEvent& rawEvent)
{
#if defined(_WIN32)
    if (!m_plugin || m_isWindowed)
        return false;

    scoped_refptr<WebPluginImpl> protect(this);
    const auto& evt = static_cast<const blink::WebMouseEvent&>(rawEvent);
    NPEvent npEvent {};

    // paintIntoTransformedContext positions windowless Flash at local (0, 0),
    // and WebPluginContainerImpl delivers PositionInWidget() in plugin-local coordinates.
    gfx::PointF localPt = evt.PositionInWidget();
    int x = static_cast<int>(localPt.x());
    int y = static_cast<int>(localPt.y());
    npEvent.lParam = MAKELPARAM(x, y);
    npEvent.wParam = 0;

    int mods = evt.GetModifiers();
    if (mods & blink::WebInputEvent::kControlKey)
        npEvent.wParam |= MK_CONTROL;
    if (mods & blink::WebInputEvent::kShiftKey)
        npEvent.wParam |= MK_SHIFT;
    if (mods & blink::WebInputEvent::kLeftButtonDown)
        npEvent.wParam |= MK_LBUTTON;
    if (mods & blink::WebInputEvent::kMiddleButtonDown)
        npEvent.wParam |= MK_MBUTTON;
    if (mods & blink::WebInputEvent::kRightButtonDown)
        npEvent.wParam |= MK_RBUTTON;

    blink::WebInputEvent::Type type = evt.GetType();
    if (type == blink::WebInputEvent::Type::kMouseMove
        || type == blink::WebInputEvent::Type::kMouseLeave
        || type == blink::WebInputEvent::Type::kMouseEnter) {
        npEvent.event = WM_MOUSEMOVE;
    } else if (type == blink::WebInputEvent::Type::kMouseDown) {
        switch (evt.button) {
        case blink::WebPointerProperties::Button::kLeft:
            npEvent.event = WM_LBUTTONDOWN;
            break;
        case blink::WebPointerProperties::Button::kMiddle:
            npEvent.event = WM_MBUTTONDOWN;
            break;
        case blink::WebPointerProperties::Button::kRight:
            if (m_plugin->quirks().contains(PluginQuirkIgnoreRightClickInWindowlessMode))
                return false;
            npEvent.event = WM_RBUTTONDOWN;
            break;
        default:
            return false;
        }
    } else if (type == blink::WebInputEvent::Type::kMouseUp) {
        switch (evt.button) {
        case blink::WebPointerProperties::Button::kLeft:
            npEvent.event = WM_LBUTTONUP;
            break;
        case blink::WebPointerProperties::Button::kMiddle:
            npEvent.event = WM_MBUTTONUP;
            break;
        case blink::WebPointerProperties::Button::kRight:
            npEvent.event = WM_RBUTTONUP;
            break;
        default:
            return false;
        }
    } else if (type == blink::WebInputEvent::Type::kMouseWheel) {
        const auto& wheelEvt = static_cast<const blink::WebMouseWheelEvent&>(rawEvent);
        npEvent.event = WM_MOUSEWHEEL;
        int wheelDeltaInt = 0;
        if (wheelEvt.wheel_ticks_y != 0.0f) {
            wheelDeltaInt = static_cast<int>(std::lround(wheelEvt.wheel_ticks_y * WHEEL_DELTA));
        } else if (wheelEvt.delta_y != 0.0f) {
            wheelDeltaInt = (wheelEvt.delta_y > 0.0f)
                ? std::max<int>(WHEEL_DELTA, static_cast<int>(std::lround(wheelEvt.delta_y)))
                : std::min<int>(-WHEEL_DELTA, static_cast<int>(std::lround(wheelEvt.delta_y)));
        }
        short wheelDelta = static_cast<short>(std::clamp(wheelDeltaInt, -32768, 32767));
        npEvent.wParam = MAKEWPARAM(LOWORD(npEvent.wParam), static_cast<WORD>(wheelDelta));
    } else {
        return false;
    }

    return dispatchNPEvent(npEvent);
#else
    return false;
#endif
}

bool WebPluginImpl::handleKeyboardCharEventForEmulateIme(int windowsKeyCode)
{
#if defined(_WIN32)
    if (!m_plugin || !m_instance)
        return false;
    if (!m_pluginIme && m_plugin->quirks().contains(PluginQuirkEmulateIme))
        m_pluginIme = std::make_unique<WebPluginIMEWin>();
    if (!m_pluginIme)
        return false;

    NPEvent focusEvent {};
    focusEvent.event = WM_SETFOCUS;
    dispatchNPEvent(focusEvent);

    char16_t ch = static_cast<char16_t>(windowsKeyCode);
    std::u16string text(1, ch);
    std::vector<int> clauses { 0 };
    std::vector<int> target { 0 };
    m_pluginIme->compositionUpdated(text, clauses, target, 0);
    m_pluginIme->compositionCompleted(text);
    m_pluginIme->sendEvents(m_instance, m_plugin->pluginFuncs()->event);
    return true;
#else
    return false;
#endif
}

bool WebPluginImpl::handleKeyboardEvent(const blink::WebKeyboardEvent& evt)
{
#if defined(_WIN32)
    if (!m_plugin || m_isWindowed)
        return false;

    scoped_refptr<WebPluginImpl> protect(this);
    if (!m_pluginIme && m_plugin->quirks().contains(PluginQuirkEmulateIme))
        m_pluginIme = std::make_unique<WebPluginIMEWin>();
    WebPluginIMEWin::ScopedLock lock(m_pluginIme.get());

    UINT scanCode = ::MapVirtualKeyW(static_cast<UINT>(evt.windows_key_code), MAPVK_VK_TO_VSC) & 0xFF;
    uintptr_t baseLParam = 1u | (static_cast<uintptr_t>(scanCode) << 16);
    if (evt.GetModifiers() & blink::WebInputEvent::kIsKeyPad)
        baseLParam |= (1u << 24);
    switch (evt.windows_key_code) {
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
    case VK_HOME:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_INSERT:
    case VK_DELETE:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_DIVIDE:
        baseLParam |= (1u << 24);
        break;
    default:
        break;
    }

    NPEvent npEvent {};
    npEvent.wParam = evt.windows_key_code;

    blink::WebInputEvent::Type type = evt.GetType();
    if (type == blink::WebInputEvent::Type::kRawKeyDown || type == blink::WebInputEvent::Type::kKeyDown) {
        npEvent.event = WM_KEYDOWN;
        npEvent.lParam = baseLParam;
        if (evt.GetModifiers() & blink::WebInputEvent::kIsAutoRepeat)
            npEvent.lParam |= (1u << 30);
    } else if (type == blink::WebInputEvent::Type::kChar) {
        int charCode = evt.text[0] ? evt.text[0] : evt.windows_key_code;
        if (m_plugin->quirks().contains(PluginQuirkEmulateIme) && charCode > 0x7F) {
            return handleKeyboardCharEventForEmulateIme(charCode);
        }
        npEvent.event = WM_CHAR;
        npEvent.wParam = charCode;
        npEvent.lParam = baseLParam;
        dispatchNPEvent(npEvent);
        return true;
    } else if (type == blink::WebInputEvent::Type::kKeyUp) {
        npEvent.event = WM_KEYUP;
        npEvent.lParam = baseLParam | 0xC0000000u;
    } else {
        return false;
    }

    return dispatchNPEvent(npEvent);
#else
    return false;
#endif
}

blink::WebInputEventResult WebPluginImpl::HandleInputEvent(
    const blink::WebCoalescedInputEvent& coalescedEvent, ui::Cursor* /*cursor*/)
{
    if (m_isWindowed)
        return blink::WebInputEventResult::kNotHandled;

    const blink::WebInputEvent& evt = coalescedEvent.Event();
    blink::WebInputEvent::Type type = evt.GetType();

    if (blink::WebInputEvent::IsMouseEventType(type) || type == blink::WebInputEvent::Type::kMouseWheel) {
        return handleMouseEvent(evt)
            ? blink::WebInputEventResult::kHandledApplication
            : blink::WebInputEventResult::kNotHandled;
    }

    if (blink::WebInputEvent::IsKeyboardEventType(type)) {
        return handleKeyboardEvent(static_cast<const blink::WebKeyboardEvent&>(evt))
            ? blink::WebInputEventResult::kHandledApplication
            : blink::WebInputEventResult::kNotHandled;
    }

    return blink::WebInputEventResult::kNotHandled;
}

bool WebPluginImpl::getImeStatus(int* inputType, gfx::Rect* caretRect)
{
    if (!m_pluginIme)
        return false;
    return m_pluginIme->getStatus(inputType, caretRect);
}

bool WebPluginImpl::SetComposition(const blink::WebString& text, const blink::WebVector<ui::ImeTextSpan>& imeTextSpans,
    const blink::WebRange& /*replacementRange*/, int /*selectionStart*/, int selectionEnd)
{
    if (!m_plugin || !m_instance)
        return false;
    if (!m_pluginIme && m_plugin->quirks().contains(PluginQuirkEmulateIme))
        m_pluginIme = std::make_unique<WebPluginIMEWin>();
    if (!m_pluginIme)
        return false;

    std::vector<int> clauses;
    std::vector<int> target;
    for (const auto& span : imeTextSpans) {
        clauses.push_back(static_cast<int>(span.start_offset));
        clauses.push_back(static_cast<int>(span.end_offset));
        if (span.thickness == ui::ImeTextSpan::Thickness::kThick) {
            target = { static_cast<int>(span.start_offset), static_cast<int>(span.end_offset) };
        }
    }

    std::u16string text16 = text.Utf16();
    m_pluginIme->compositionUpdated(text16, clauses, target, selectionEnd);
    m_pluginIme->sendEvents(m_instance, m_plugin->pluginFuncs()->event);
    return true;
}

bool WebPluginImpl::CommitText(const blink::WebString& text, const blink::WebVector<ui::ImeTextSpan>& /*imeTextSpans*/,
    const blink::WebRange& /*replacementRange*/, int /*relativeCaretPosition*/)
{
    if (!m_plugin || !m_instance)
        return false;
    if (!m_pluginIme && m_plugin->quirks().contains(PluginQuirkEmulateIme))
        m_pluginIme = std::make_unique<WebPluginIMEWin>();
    if (!m_pluginIme)
        return false;

    m_pluginIme->compositionCompleted(text.Utf16());
    m_pluginIme->sendEvents(m_instance, m_plugin->pluginFuncs()->event);
    return true;
}

bool WebPluginImpl::FinishComposingText(blink::WebInputMethodController::ConfirmCompositionBehavior)
{
    return true;
}

} // namespace content
