// Copyright (C) 2006-2010 Apple Inc. All rights reserved.
// Ported and adapted for miniblink132.

#include "content/renderer/npapi/WebPluginImpl.h"

#include <cstring>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/renderer/npapi/npruntime_priv.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_plugin_container.h"
#include "url/gurl.h"

namespace content {

namespace {

blink::KURL makePluginURL(const blink::KURL& baseURL, const char* relativeURLString)
{
    std::string raw = relativeURLString ? relativeURLString : "";
    std::erase(raw, '\n');
    std::erase(raw, '\r');
    return blink::KURL(baseURL, WTF::String::FromUTF8(raw));
}

int locationAfterFirstBlankLine(const std::vector<char>& buffer)
{
    if (buffer.size() < 4)
        return -1;
    const char* bytes = buffer.data();
    size_t length = buffer.size();
    for (size_t i = 0; i + 4 <= length; ++i) {
        if (bytes[i] == '\n' && bytes[i + 1] == '\n')
            return static_cast<int>(i + 2);
        if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
            i += 2;
            if (i == 2)
                return static_cast<int>(i);
            if (bytes[i] == '\n')
                return static_cast<int>(i + 1);
            if (bytes[i] == '\r' && bytes[i + 1] == '\n')
                return static_cast<int>(i + 2);
        }
    }
    return -1;
}

void parsePostHeaderFields(const char* bytes, size_t length, net::HttpRequestHeaders* outHeaders)
{
    std::string headerBlock(bytes, length);
    size_t pos = 0;
    while (pos < headerBlock.size()) {
        size_t eol = headerBlock.find_first_of("\r\n", pos);
        size_t lineEnd = (eol == std::string::npos) ? headerBlock.size() : eol;
        std::string line = headerBlock.substr(pos, lineEnd - pos);
        pos = lineEnd;
        if (pos < headerBlock.size() && headerBlock[pos] == '\r')
            ++pos;
        if (pos < headerBlock.size() && headerBlock[pos] == '\n')
            ++pos;
        if (line.empty())
            break;
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = std::string(base::TrimWhitespaceASCII(line.substr(0, colon), base::TRIM_ALL));
        std::string val = std::string(base::TrimWhitespaceASCII(line.substr(colon + 1), base::TRIM_ALL));
        if (!key.empty())
            outHeaders->SetHeader(key, val);
    }
}

} // namespace

NPError WebPluginImpl::loadRequest(
    std::unique_ptr<network::ResourceRequest> request, const char* target, bool sendNotification, void* notifyData)
{
    if (!m_webFrame || !request || !request->url.is_valid())
        return NPERR_INVALID_URL;

    auto req = std::make_unique<PluginRequest>();
    req->request = std::move(request);
    if (target)
        req->targetFrameName = target;
    req->sendNotification = sendNotification;
    req->notifyData = notifyData;
    req->shouldAllowPopups = arePopupsAllowed();
    scheduleRequest(std::move(req));
    return NPERR_NO_ERROR;
}

void WebPluginImpl::scheduleRequest(std::unique_ptr<PluginRequest> request)
{
    m_requests.push_back(std::move(request));
    if (!m_requestTimer.IsRunning()) {
        m_requestTimer.Start(FROM_HERE, base::TimeDelta(),
            base::BindOnce(&WebPluginImpl::requestTimerFired, base::Unretained(this)));
    }
}

void WebPluginImpl::requestTimerFired()
{
    if (m_requests.empty())
        return;
    std::unique_ptr<PluginRequest> req = std::move(m_requests.front());
    m_requests.erase(m_requests.begin());

    if (!m_requests.empty() && !m_requestTimer.IsRunning()) {
        m_requestTimer.Start(FROM_HERE, base::TimeDelta(),
            base::BindOnce(&WebPluginImpl::requestTimerFired, base::Unretained(this)));
    }
    performRequest(std::move(req));
}

void WebPluginImpl::performRequest(std::unique_ptr<PluginRequest> req)
{
    if (!m_isStarted || !m_webFrame || !req || !req->request)
        return;

    scoped_refptr<WebPluginImpl> protect(this);
    std::string urlSpec = req->request->url.possibly_invalid_spec();

    if (base::StartsWith(urlSpec, "javascript:", base::CompareCase::INSENSITIVE_ASCII)) {
        std::string jsCode = urlSpec.substr(11);
        NPString npScript { jsCode.c_str(), static_cast<uint32_t>(jsCode.size()) };
        NPVariant evalResult;
        VOID_TO_NPVARIANT(evalResult);
        _NPN_EvaluateHelper(m_instance, req->shouldAllowPopups, windowScriptNPObject(), &npScript, &evalResult);

        if (req->targetFrameName.empty()) {
            std::string jsStr;
            if (NPVARIANT_IS_STRING(evalResult)) {
                NPString s = NPVARIANT_TO_STRING(evalResult);
                if (s.UTF8Characters && s.UTF8Length > 0)
                    jsStr.assign(s.UTF8Characters, s.UTF8Length);
            }
            blink::KURL kurl(WTF::String::FromUTF8(urlSpec));
            auto stream = PluginStream::create(this, m_mbwebviewId, std::move(req->request), nullptr,
                req->sendNotification, req->notifyData, m_plugin->pluginFuncs(), m_instance, m_plugin->quirks());
            m_streams.insert(stream);
            stream->sendJavaScriptStream(kurl, jsStr);
        }
        _NPN_ReleaseVariantValue(&evalResult);
        return;
    }

    if (req->targetFrameName.empty()) {
        scoped_refptr<const blink::SecurityOrigin> origin = blink::SecurityOrigin::CreateFromString(
            m_webFrame->GetDocument().Url().GetString());
        auto stream = PluginStream::create(this, m_mbwebviewId, std::move(req->request), std::move(origin),
            req->sendNotification, req->notifyData, m_plugin->pluginFuncs(), m_instance, m_plugin->quirks());
        m_streams.insert(stream);
        stream->start();
    } else if (m_pluginContainer) {
        blink::WebURLRequest webReq(blink::KURL(WTF::String::FromUTF8(urlSpec)));
        webReq.SetHttpMethod(blink::WebString::FromUTF8(req->request->method));
        m_pluginContainer->LoadFrameRequest(webReq, blink::WebString::FromUTF8(req->targetFrameName));
        if (req->sendNotification && m_plugin->pluginFuncs()->urlnotify) {
            setCurrentPluginView(this);
            setCallingPlugin(true);
            m_plugin->pluginFuncs()->urlnotify(m_instance, urlSpec.c_str(), NPRES_DONE, req->notifyData);
            setCallingPlugin(false);
            setCurrentPluginView(nullptr);
        }
    }
}

NPError WebPluginImpl::getURLNotify(const char* url, const char* target, void* notifyData)
{
    if (!m_webFrame || !url)
        return NPERR_INVALID_URL;
    blink::KURL resolved = makePluginURL(m_webFrame->GetDocument().BaseURL(), url);
    auto req = std::make_unique<network::ResourceRequest>();
    req->method = "GET";
    req->url = GURL(resolved.GetString().Utf8());
    req->headers.SetHeader("User-Agent", blink::Platform::Current()->UserAgent().Utf8());
    req->headers.SetHeader("Referer", m_webFrame->GetDocument().BaseURL().GetString().Utf8());
    return loadRequest(std::move(req), target, true, notifyData);
}

NPError WebPluginImpl::getURL(const char* url, const char* target)
{
    if (!m_webFrame || !url)
        return NPERR_INVALID_URL;
    blink::KURL resolved = makePluginURL(m_webFrame->GetDocument().BaseURL(), url);
    auto req = std::make_unique<network::ResourceRequest>();
    req->method = "GET";
    req->url = GURL(resolved.GetString().Utf8());
    req->headers.SetHeader("User-Agent", blink::Platform::Current()->UserAgent().Utf8());
    req->headers.SetHeader("Referer", m_webFrame->GetDocument().BaseURL().GetString().Utf8());
    return loadRequest(std::move(req), target, false, nullptr);
}

NPError WebPluginImpl::postURLNotify(const char* url, const char* target, uint32_t len, const char* buf, NPBool file, void* notifyData)
{
    return handlePost(url, target, len, buf, file != 0, notifyData, true, true);
}

NPError WebPluginImpl::postURL(const char* url, const char* target, uint32_t len, const char* buf, NPBool file)
{
    return handlePost(url, target, len, buf, file != 0, nullptr, false, file != 0);
}

NPError WebPluginImpl::handlePostReadFile(std::vector<char>& buffer, uint32_t len, const char* buf)
{
    std::string filename(buf, len);
    if (base::StartsWith(filename, "file:///", base::CompareCase::INSENSITIVE_ASCII))
        filename.erase(0, 8);

    base::FilePath path = base::FilePath::FromUTF8Unsafe(filename);
    std::string contents;
    if (!base::ReadFileToString(path, &contents))
        return NPERR_FILE_NOT_FOUND;
    buffer.assign(contents.begin(), contents.end());
    return NPERR_NO_ERROR;
}

NPError WebPluginImpl::handlePost(
    const char* url, const char* target, uint32_t len, const char* buf, bool file, void* notifyData, bool sendNotification, bool allowHeaders)
{
    if (!m_webFrame || !url || !len || !buf)
        return NPERR_INVALID_PARAM;

    std::vector<char> buffer;
    if (file) {
        NPError readErr = handlePostReadFile(buffer, len, buf);
        if (readErr != NPERR_NO_ERROR)
            return readErr;
    } else {
        buffer.assign(buf, buf + len);
    }

    const char* postData = buffer.data();
    size_t postDataLen = buffer.size();

    auto req = std::make_unique<network::ResourceRequest>();
    req->method = "POST";
    blink::KURL resolved = makePluginURL(m_webFrame->GetDocument().BaseURL(), url);
    req->url = GURL(resolved.GetString().Utf8());
    req->headers.SetHeader("User-Agent", blink::Platform::Current()->UserAgent().Utf8());
    req->headers.SetHeader("Referer", m_webFrame->GetDocument().BaseURL().GetString().Utf8());

    if (allowHeaders && !buffer.empty()) {
        if (buffer[0] == '\n') {
            ++postData;
            --postDataLen;
        } else {
            int loc = locationAfterFirstBlankLine(buffer);
            if (loc != -1) {
                parsePostHeaderFields(buffer.data(), static_cast<size_t>(loc), &req->headers);
                req->headers.RemoveHeader("Content-Length");
                postData += loc;
                postDataLen = buffer.size() - static_cast<size_t>(loc);
            }
        }
    }

    req->request_body = network::ResourceRequestBody::CreateFromBytes(postData, postDataLen);
    return loadRequest(std::move(req), target, sendNotification, notifyData);
}

NPError WebPluginImpl::newStream(NPMIMEType, const char*, NPStream**)
{
    return NPERR_GENERIC_ERROR;
}

int32_t WebPluginImpl::write(NPStream*, int32_t, void*)
{
    return -1;
}

NPError WebPluginImpl::destroyStream(NPStream* stream, NPReason reason)
{
    if (!stream || PluginStream::ownerForStream(stream) != m_instance)
        return NPERR_INVALID_INSTANCE_ERROR;
    static_cast<PluginStream*>(stream->ndata)->cancelAndDestroyStream(reason);
    return NPERR_NO_ERROR;
}

void WebPluginImpl::disconnectStream(PluginStream* stream)
{
    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        if (it->get() == stream) {
            m_streams.erase(it);
            break;
        }
    }
}

void WebPluginImpl::DidReceiveResponse(const blink::WebURLResponse& response)
{
    if (m_status != PluginStatusLoadedSuccessfully || !m_plugin)
        return;

    auto req = std::make_unique<network::ResourceRequest>();
    req->method = "GET";
    req->url = GURL(response.CurrentRequestUrl().GetString().Utf8());
    m_manualStream = PluginStream::create(
        this, m_mbwebviewId, std::move(req), nullptr, false, nullptr, m_plugin->pluginFuncs(), m_instance, m_plugin->quirks());
    m_manualStream->setLoadManually(true);
    m_manualStream->didReceiveResponseManual(response);
}

void WebPluginImpl::DidReceiveData(base::span<const char> data)
{
    if (m_status == PluginStatusLoadedSuccessfully && m_manualStream)
        m_manualStream->didReceiveDataManual(data);
}

void WebPluginImpl::DidFinishLoading()
{
    if (m_status == PluginStatusLoadedSuccessfully && m_manualStream)
        m_manualStream->didFinishLoadingManual();
}

void WebPluginImpl::DidFailLoading(const blink::WebURLError& error)
{
    if (m_status == PluginStatusLoadedSuccessfully && m_manualStream)
        m_manualStream->didFailManual(error);
}

} // namespace content
