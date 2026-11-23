#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>

#include <WebView2.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <io.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "helper_of.hpp"
#include "rendering.hpp"
#include "tal/pixels.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// tal-render-webview2 (E6e): the web engine of tal on Windows, WebView2 (the Edge runtime) in a
// process of its own, speaking the protocol of tal's renderers (RENDERING.md, version 2) for
// pages, as tal-render-gtk does on Linux.
//
// Each view is a controller on a window of ours that is never shown, so that its pictures have the
// view's size whatever the scale of the screen; its pictures come as PNG and are decoded by tal.
// The browser's data lives in a folder of ours under the temporary folder, held while we live by
// a file opened for us alone, so that a start sweeps away the folders of processes gone.
//
// With the network off every request of the page but file: of no host or of localhost is
// answered 403 (file://host would be an SMB connection that leaks the user's NTLM hash), and the
// browser runs with a proxy to nowhere, a resolver that resolves nothing and no prefetching; since
// those arguments hold for a whole environment, views with the network get another. Page scripts
// are on only when asked; the input (helper.js) runs through ExecuteScript, which works with them
// off, its functions taken when the document is made, before the page could change them. A page
// goes on by itself only where its Browsing allows (NavigationStarting cancels the rest); new
// windows, downloads, dialogs and permissions are refused, every refusal told; the failure of the
// page's process is told as lost.
namespace {

namespace rendering = tal::rendering;
using rendering::Frame;
using rendering::Sort;
using tal::Bytes;
using tal::Kind;
using tal::Trouble;

constexpr UINT heard_frame = WM_APP + 1;
constexpr UINT heard_end = WM_APP + 2;
constexpr UINT closing = WM_APP + 3;
constexpr std::wstring_view prefix = L"tal-render-webview2-";

FILE* wire = nullptr;
HWND window = nullptr;
std::filesystem::path folder;

// The interface `T` held, released when it goes.
template <typename T>
class Held {
public:
    Held() = default;

    // `given`, its reference taken.
    explicit Held(T* given) : held_(given) {
        if (held_ != nullptr) held_->AddRef();
    }

    // Lets the interface go.
    ~Held() { reset(); }

    Held(const Held& other) : Held(other.held_) {}

    // `other`'s interface held instead.
    Held& operator=(const Held& other) {
        Held made{other};

        std::swap(held_, made.held_);

        return *this;
    }

    // Lets the interface go, if any.
    void reset() {
        if (held_ != nullptr) held_->Release();

        held_ = nullptr;
    }

    // The place to receive a new interface, whose reference becomes ours.
    T** put() {
        reset();

        return &held_;
    }

    // The interface.
    T* operator->() const { return held_; }

    // The interface, or none.
    T* get() const { return held_; }

    // Whether there is one.
    explicit operator bool() const { return held_ != nullptr; }

private:
    T* held_ = nullptr;
};

// A COM handler of the interface `Face` that calls `call` with what it is given.
template <typename Face, typename... Args>
class Handler final : public Face {
public:
    // A handler known as `iid`, calling `call`.
    Handler(const IID& iid, std::function<HRESULT(Args...)> call) : iid_(iid), call_(std::move(call)) {}

    // Itself for IUnknown and its interface.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iid_)) {
            *out = static_cast<Face*>(this);
            AddRef();

            return S_OK;
        }

        *out = nullptr;

        return E_NOINTERFACE;
    }

    // One more reference.
    ULONG STDMETHODCALLTYPE AddRef() override { return ++count_; }

    // One reference less; gone with the last.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --count_;

        if (left == 0) delete this;

        return left;
    }

    // The call.
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return call_(args...); }

private:
    IID iid_;
    std::function<HRESULT(Args...)> call_;
    ULONG count_ = 1;
};

// A handler of `Face` known as `iid` calling `call`.
template <typename Face, typename... Args>
Face* handler(const IID& iid, std::type_identity_t<std::function<HRESULT(Args...)>> call) {
    return new Handler<Face, Args...>(iid, std::move(call));
}

// Ends the process at once with `code`.
[[noreturn]] void gone(int code) { std::_Exit(code); }

std::set<std::uint32_t> asked;

// The frame of `sort` with `id` and `payload` written; a program that hears no more ends us. An
// answer goes only to a question still open, once: WebView2 may complete what was cancelled.
void said(Sort sort, std::uint32_t id, Bytes payload = {}) {
    if ((sort == Sort::done || sort == Sort::failed) && id != 0 && asked.erase(id) == 0) {
        std::fprintf(stderr, "tal-render-webview2: a second answer to %u let go\n", static_cast<unsigned>(id));

        return;
    }

    Frame frame;

    frame.sort = sort;
    frame.id = id;
    frame.payload = std::move(payload);

    const Bytes bytes = rendering::framed(frame);

    if (std::fwrite(bytes.data(), 1, bytes.size(), wire) != bytes.size() || std::fflush(wire) != 0) gone(0);
}

// The question `id` failed with `trouble`.
void failed(std::uint32_t id, const Trouble& trouble) { said(Sort::failed, id, rendering::failed_with(trouble)); }

// Said before any hello: the engine cannot work here, for `why`; then the end.
[[noreturn]] void unavailable(const std::string& why) {
    failed(0, Trouble{Kind::unsupported, why});
    gone(0);
}

// `wide` in UTF-8.
std::string narrow(std::wstring_view wide) {
    const int much = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string made(static_cast<std::size_t>(std::max(much, 0)), '\0');

    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), made.data(), much, nullptr, nullptr);

    return made;
}

// `text` in UTF-16.
std::wstring widened(std::string_view text) {
    const int much = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring made(static_cast<std::size_t>(std::max(much, 0)), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), much);

    return made;
}

// A string WebView2 gave, in UTF-8 cut to 64 KiB at a whole character; the string freed.
std::string taken(LPWSTR given) {
    std::string made = given != nullptr ? tal::text::repaired(narrow(given)) : std::string{};

    if (given != nullptr) CoTaskMemFree(given);

    if (made.size() > rendering::widest_said) {
        std::size_t end = rendering::widest_said;

        while (end > 0 && (static_cast<unsigned char>(made[end]) & 0xC0U) == 0x80U) --end;

        made.resize(end);
    }

    return made;
}

// The trouble of a call of WebView2 that failed with `result`, saying what was done.
Trouble trouble_of(const std::string& what, HRESULT result) {
    char code[16];

    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(result));

    return Trouble{Kind::broken, what + " failed (" + code + ")."};
}

struct View {
    std::uint32_t number = 0;
    bool scripts = false;
    bool network = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Held<ICoreWebView2Controller> controller;
    Held<ICoreWebView2> web;
    std::optional<std::uint32_t> loading;
    bool closed = false;
};

std::map<std::uint32_t, std::shared_ptr<View>> views;

// Whether `uri` is a file of no host or of localhost.
bool local(std::string_view uri) { return uri.starts_with("file:///") || uri.starts_with("file://localhost/"); }

// Whether `view` may go to `uri` as its page: never javascript:, data: or blob:; with the network
// off only local files (and about:blank); with it on, the web and local files.
bool allowed(const View& view, std::string_view uri) {
    if (uri == "about:blank" || local(uri)) return true;

    return view.network && (uri.starts_with("http://") || uri.starts_with("https://"));
}

// What `view` refused for `reason` (the protocol's number), at `uri`, told.
void refused(const View& view, std::uint8_t reason, std::string uri) {
    said(Sort::refused, 0, rendering::refused_with(rendering::Refused{view.number, reason, std::move(uri)}));
}

// The state of `view` told: its title, address, loading and ways back and forward.
void told(const View& view, bool loading) {
    if (view.closed) return;

    rendering::Changed made;
    LPWSTR title = nullptr;
    LPWSTR source = nullptr;
    BOOL back = FALSE;
    BOOL forward = FALSE;

    view.web->get_DocumentTitle(&title);
    view.web->get_Source(&source);
    view.web->get_CanGoBack(&back);
    view.web->get_CanGoForward(&forward);
    made.view = view.number;
    made.title = taken(title);
    made.address = taken(source);
    made.loading = loading;
    made.back = back != FALSE;
    made.forward = forward != FALSE;
    said(Sort::changed, 0, rendering::changed_with(made));
}

// A copy of `text` for WebView2 to free, at `out`.
HRESULT handed(const std::wstring& text, LPWSTR* out) {
    if (out == nullptr) return E_POINTER;

    *out = static_cast<LPWSTR>(CoTaskMemAlloc((text.size() + 1) * sizeof(wchar_t)));

    if (*out == nullptr) return E_OUTOFMEMORY;

    std::wmemcpy(*out, text.c_str(), text.size() + 1);

    return S_OK;
}

// The options of an environment: the browser's arguments, the language of the system, the version
// of WebView2 the SDK was made for, no single sign-on.
class Options final : public ICoreWebView2EnvironmentOptions {
public:
    // Options with the browser's arguments `arguments`.
    explicit Options(std::wstring arguments) : arguments_(std::move(arguments)) {}

    // Itself for IUnknown and its interface.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) {
            *out = static_cast<ICoreWebView2EnvironmentOptions*>(this);
            AddRef();

            return S_OK;
        }

        *out = nullptr;

        return E_NOINTERFACE;
    }

    // One more reference.
    ULONG STDMETHODCALLTYPE AddRef() override { return ++count_; }

    // One reference less; gone with the last.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --count_;

        if (left == 0) delete this;

        return left;
    }

    // The browser's arguments.
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* out) override { return handed(arguments_, out); }

    // Not changed from outside.
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR) override { return E_NOTIMPL; }

    // The language of the system (empty).
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* out) override { return handed(L"", out); }

    // Not changed from outside.
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return E_NOTIMPL; }

    // The version of WebView2 the SDK was made for (156.0.4314.35).
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* out) override { return handed(L"156.0.4314.35", out); }

    // Not changed from outside.
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return E_NOTIMPL; }

    // No single sign-on with the account of the system.
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* out) override {
        *out = FALSE;

        return S_OK;
    }

    // Not changed from outside.
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return E_NOTIMPL; }

private:
    std::wstring arguments_;
    ULONG count_ = 1;
};

// The environments of WebView2: one without the network, one with it, each made once, those who
// wait for one called when it comes.
struct Environment {
    Held<ICoreWebView2Environment> made;
    std::optional<HRESULT> failure;
    bool making = false;
    std::vector<std::function<void()>> waiting;
};

Environment environments[2];

// `then` called once the environment of `network` is made (or could not be).
void with_environment(bool network, std::function<void()> then) {
    Environment& one = environments[network ? 1 : 0];

    if (one.made || one.failure) return then();

    one.waiting.push_back(std::move(then));

    if (one.making) return;

    one.making = true;

    const std::wstring data = (folder / (network ? L"online" : L"offline")).wstring();
    const std::wstring arguments = network ? L"" : L"--proxy-server=http://127.0.0.1:9 --proxy-bypass-list=<-loopback> "
                                                   L"--host-resolver-rules=\"MAP * ~NOTFOUND\" --dns-prefetch-disable "
                                                   L"--disable-features=NetworkPrediction";
    const auto done = handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
        IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, [network](HRESULT result, ICoreWebView2Environment* environment) {
            Environment& it = environments[network ? 1 : 0];

            if (FAILED(result) || environment == nullptr) it.failure = FAILED(result) ? result : E_FAIL;
            else it.made = Held<ICoreWebView2Environment>{environment};

            std::vector<std::function<void()>> waiting;

            waiting.swap(it.waiting);

            for (auto& call : waiting) call();

            return S_OK;
        });
    Options* options = new Options(arguments);
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), options, done);

    options->Release();
    done->Release();

    if (FAILED(started)) {
        one.failure = started;

        std::vector<std::function<void()>> waiting;

        waiting.swap(one.waiting);

        for (auto& call : waiting) call();
    }
}

// A page of `view` that did not load, or loaded: told, and its opening answered.
void navigated(View& view, ICoreWebView2NavigationCompletedEventArgs* args) {
    BOOL success = FALSE;

    args->get_IsSuccess(&success);
    told(view, false);

    if (!view.loading) return;

    if (success != FALSE) {
        said(Sort::done, *view.loading);
    } else {
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;

        args->get_WebErrorStatus(&status);
        failed(*view.loading, Trouble{Kind::broken, "The page did not load (error " + std::to_string(static_cast<int>(status)) + ")."});
    }

    view.loading.reset();
}

// The events of `view` taken: where it goes, what it refuses, what it tells.
void listened(const std::shared_ptr<View>& view) {
    EventRegistrationToken token{};
    ICoreWebView2* web = view->web.get();
    const std::weak_ptr<View> weak = view;

    web->add_NavigationStarting(handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
                                    IID_ICoreWebView2NavigationStartingEventHandler,
                                    [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                                        const auto view = weak.lock();
                                        LPWSTR uri = nullptr;

                                        args->get_Uri(&uri);

                                        const std::string address = taken(uri);

                                        if (!view || allowed(*view, address)) {
                                            if (view) told(*view, true);

                                            return S_OK;
                                        }

                                        args->put_Cancel(TRUE);
                                        refused(*view, 1, address);

                                        return S_OK;
                                    }),
                                &token);
    web->add_NavigationCompleted(handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
                                     IID_ICoreWebView2NavigationCompletedEventHandler,
                                     [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
                                         if (const auto view = weak.lock()) navigated(*view, args);

                                         return S_OK;
                                     }),
                                 &token);
    web->add_DocumentTitleChanged(handler<ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>(
                                      IID_ICoreWebView2DocumentTitleChangedEventHandler,
                                      [weak](ICoreWebView2*, IUnknown*) {
                                          if (const auto view = weak.lock()) told(*view, view->loading.has_value());

                                          return S_OK;
                                      }),
                                  &token);
    web->add_HistoryChanged(handler<ICoreWebView2HistoryChangedEventHandler, ICoreWebView2*, IUnknown*>(IID_ICoreWebView2HistoryChangedEventHandler,
                                                                                                         [weak](ICoreWebView2*, IUnknown*) {
                                                                                                             if (const auto view = weak.lock()) told(*view, false);

                                                                                                             return S_OK;
                                                                                                         }),
                            &token);
    web->add_NewWindowRequested(handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
                                    IID_ICoreWebView2NewWindowRequestedEventHandler,
                                    [weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                                        LPWSTR uri = nullptr;

                                        args->get_Uri(&uri);
                                        args->put_Handled(TRUE);

                                        if (const auto view = weak.lock()) refused(*view, 0, taken(uri));
                                        else CoTaskMemFree(uri);

                                        return S_OK;
                                    }),
                                &token);
    web->add_ScriptDialogOpening(handler<ICoreWebView2ScriptDialogOpeningEventHandler, ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs*>(
                                     IID_ICoreWebView2ScriptDialogOpeningEventHandler,
                                     [weak](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args) {
                                         LPWSTR uri = nullptr;

                                         args->get_Uri(&uri);

                                         if (const auto view = weak.lock()) refused(*view, 3, taken(uri));
                                         else CoTaskMemFree(uri);

                                         return S_OK;
                                     }),
                                 &token);
    web->add_PermissionRequested(handler<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*>(
                                     IID_ICoreWebView2PermissionRequestedEventHandler,
                                     [weak](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) {
                                         LPWSTR uri = nullptr;

                                         args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                         args->get_Uri(&uri);

                                         if (const auto view = weak.lock()) refused(*view, 4, taken(uri));
                                         else CoTaskMemFree(uri);

                                         return S_OK;
                                     }),
                                 &token);
    web->add_ProcessFailed(handler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
                               IID_ICoreWebView2ProcessFailedEventHandler,
                               [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) {
                                   const auto view = weak.lock();

                                   if (!view || view->closed) return S_OK;

                                   if (view->loading) failed(*view->loading, Trouble{Kind::broken, "The process of the page ended."});

                                   view->loading.reset();
                                   said(Sort::lost, 0, rendering::Writing().u32(view->number).done());

                                   return S_OK;
                               }),
                           &token);

    Held<ICoreWebView2_4> fourth;

    if (SUCCEEDED(web->QueryInterface(IID_ICoreWebView2_4, reinterpret_cast<void**>(fourth.put())))) {
        fourth->add_DownloadStarting(handler<ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs*>(
                                         IID_ICoreWebView2DownloadStartingEventHandler,
                                         [weak](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                                             Held<ICoreWebView2DownloadOperation> download;
                                             LPWSTR uri = nullptr;

                                             args->put_Cancel(TRUE);
                                             args->get_DownloadOperation(download.put());

                                             if (download) download->get_Uri(&uri);

                                             if (const auto view = weak.lock()) refused(*view, 2, taken(uri));
                                             else CoTaskMemFree(uri);

                                             return S_OK;
                                         }),
                                     &token);
    }

    if (!view->network) {
        web->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        web->add_WebResourceRequested(handler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
                                          IID_ICoreWebView2WebResourceRequestedEventHandler,
                                          [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                                              Held<ICoreWebView2WebResourceRequest> request;
                                              LPWSTR uri = nullptr;

                                              args->get_Request(request.put());

                                              if (request) request->get_Uri(&uri);

                                              if (local(taken(uri))) return S_OK;

                                              Held<ICoreWebView2WebResourceResponse> response;

                                              environments[0].made->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", response.put());
                                              args->put_Response(response.get());

                                              return S_OK;
                                          }),
                                      &token);
    }
}

// The view of a navigate frame opened: its controller on our window, its settings as its Browsing
// allows, the helper, its events, the page asked; answered when it has loaded.
void navigate(const Frame& frame) {
    rendering::Reading from{frame.payload};
    auto view = std::make_shared<View>();

    view->number = from.u32();

    const std::string address = from.text();

    view->scripts = from.u8() != 0;
    view->network = from.u8() != 0;
    view->width = from.u32();
    view->height = from.u32();
    from.ended();

    if (views.contains(view->number)) return failed(frame.id, Trouble{Kind::other, "A view of that number is open."});

    if (!allowed(*view, address)) return failed(frame.id, Trouble{Kind::refused, "The view may not go there."});

    views[view->number] = view;
    view->loading = frame.id;

    with_environment(view->network, [view, address] {
        Environment& environment = environments[view->network ? 1 : 0];

        if (view->closed) return;

        if (!environment.made) {
            failed(*view->loading, trouble_of("Making the environment of WebView2", environment.failure.value_or(E_FAIL)));
            views.erase(view->number);

            return;
        }

        environment.made->CreateCoreWebView2Controller(
            window, handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                        IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, [view, address](HRESULT result, ICoreWebView2Controller* controller) {
                            if (view->closed) {
                                if (controller != nullptr) controller->Close();

                                return S_OK;
                            }

                            if (FAILED(result) || controller == nullptr) {
                                failed(*view->loading, trouble_of("Making a view of WebView2", FAILED(result) ? result : E_FAIL));
                                views.erase(view->number);

                                return S_OK;
                            }

                            view->controller = Held<ICoreWebView2Controller>{controller};
                            controller->get_CoreWebView2(view->web.put());

                            Held<ICoreWebView2Settings> settings;

                            view->web->get_Settings(settings.put());
                            settings->put_IsScriptEnabled(view->scripts ? TRUE : FALSE);
                            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_IsWebMessageEnabled(FALSE);

                            Held<ICoreWebView2Controller3> third;

                            if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller3, reinterpret_cast<void**>(third.put())))) {
                                third->put_ShouldDetectMonitorScaleChanges(FALSE);
                                third->put_RasterizationScale(1.0);
                            }

                            const RECT bounds{0, 0, static_cast<LONG>(view->width), static_cast<LONG>(view->height)};

                            controller->put_Bounds(bounds);
                            controller->put_IsVisible(TRUE);
                            listened(view);
                            view->web->AddScriptToExecuteOnDocumentCreated(widened(helper_of::helper).c_str(), nullptr);
                            view->web->Navigate(widened(address).c_str());

                            return S_OK;
                        }));
    });
}

// The view of `number` for the question `id`, or none, the question failed then.
View* found(std::uint32_t id, std::uint32_t number) {
    const auto at = views.find(number);

    if (at != views.end() && at->second->web) return at->second.get();

    failed(id, Trouble{Kind::missing, "No such view."});

    return nullptr;
}

// `script` run in `view` by the helper, answering `id` when done; a file to send refused and
// told.
void helped(View& view, std::uint32_t id, const std::string& script) {
    const std::uint32_t number = view.number;

    view.web->ExecuteScript(widened(script).c_str(), handler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
                                                       IID_ICoreWebView2ExecuteScriptCompletedHandler, [id, number](HRESULT result, LPCWSTR json) {
                                                           if (FAILED(result)) {
                                                               failed(id, trouble_of("The input", result));

                                                               return S_OK;
                                                           }

                                                           const auto at = views.find(number);

                                                           if (json != nullptr && std::wstring_view{json} == L"\"upload\"" && at != views.end()) {
                                                               LPWSTR uri = nullptr;

                                                               at->second->web->get_Source(&uri);
                                                               refused(*at->second, 5, taken(uri));
                                                           }

                                                           said(Sort::done, id);

                                                           return S_OK;
                                                       }));
}

// `text` as a string of JavaScript, quoted and escaped.
std::string literal(std::string_view text) {
    std::string made = "\"";

    for (const char one : text) {
        const auto byte = static_cast<unsigned char>(one);

        if (one == '"' || one == '\\') {
            made += '\\';
            made += one;
        } else if (byte < 0x20 || one == '\x7F') {
            char escaped[8];

            std::snprintf(escaped, sizeof escaped, "\\u%04x", byte);
            made += escaped;
        } else {
            made += one;
        }
    }

    return made + "\"";
}

// `value` written as JavaScript reads it back.
std::string number(double value) {
    char made[32];

    std::snprintf(made, sizeof made, "%.17g", value);

    return made;
}

// The name of a named key of the protocol as the helper calls it.
const char* key_name(std::uint8_t named) {
    constexpr const char* names[] = {"", "enter", "tab", "backspace", "delete", "escape", "up", "down", "left", "right", "home", "end", "pageup", "pagedown"};

    return named < std::size(names) ? names[named] : "";
}

// The picture of `view` asked: PNG decoded by tal; a picture of another size than the view's is
// a trouble.
void shoot(View& view, std::uint32_t id) {
    IStream* stream = SHCreateMemStream(nullptr, 0);
    const std::uint32_t number = view.number;

    if (stream == nullptr) return failed(id, Trouble{Kind::full, "No memory for the picture of the page."});

    view.web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
                             handler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(
                                 IID_ICoreWebView2CapturePreviewCompletedHandler, [stream, id, number](HRESULT result) {
                                     const auto at = views.find(number);

                                     if (FAILED(result) || at == views.end()) {
                                         stream->Release();
                                         failed(id, trouble_of("The picture of the page", FAILED(result) ? result : E_FAIL));

                                         return S_OK;
                                     }

                                     STATSTG stat{};
                                     LARGE_INTEGER start{};

                                     stream->Stat(&stat, STATFLAG_NONAME);
                                     stream->Seek(start, STREAM_SEEK_SET, nullptr);

                                     Bytes png(static_cast<std::size_t>(stat.cbSize.QuadPart));
                                     ULONG read = 0;

                                     stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
                                     stream->Release();

                                     View& view = *at->second;
                                     std::optional<Bytes> rgba;

                                     try {
                                         tal::media::Decoding::png()->decoded(png, [&](std::span<const std::byte> pixels, tal::media::Size size, std::chrono::milliseconds) {
                                             if (size.width == view.width && size.height == view.height) rgba.emplace(pixels.begin(), pixels.end());

                                             return false;
                                         });
                                     } catch (const Trouble& why) {
                                         failed(id, why);

                                         return S_OK;
                                     }

                                     if (!rgba) {
                                         failed(id, Trouble{Kind::other, "The picture of the page is not of the view's size."});

                                         return S_OK;
                                     }

                                     LPWSTR title = nullptr;
                                     LPWSTR source = nullptr;

                                     view.web->get_DocumentTitle(&title);
                                     view.web->get_Source(&source);
                                     said(Sort::done, id,
                                          rendering::Writing().text(taken(title)).text(taken(source)).u32(view.width).u32(view.height).bytes(*rgba).done());

                                     return S_OK;
                                 }));
}

// A frame of the program done: answered, or failed with what went wrong.
void handled(const Frame& frame) {
    asked.insert(frame.id);

    try {
        rendering::Reading from{frame.payload};

        switch (frame.sort) {
            case Sort::navigate: return navigate(frame);
            case Sort::shot: {
                View* view = found(frame.id, from.u32());

                if (view != nullptr) shoot(*view, frame.id);

                return;
            }
            case Sort::resize: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                view->width = from.u32();
                view->height = from.u32();
                from.ended();
                view->controller->put_Bounds(RECT{0, 0, static_cast<LONG>(view->width), static_cast<LONG>(view->height)});

                return said(Sort::done, frame.id);
            }
            case Sort::dismiss: {
                const auto at = views.find(from.u32());

                if (at != views.end()) {
                    at->second->closed = true;

                    if (at->second->loading) failed(*at->second->loading, Trouble{Kind::broken, "The view was closed."});

                    at->second->loading.reset();

                    PostMessageW(window, closing, 0, reinterpret_cast<LPARAM>(new std::shared_ptr<View>(at->second)));
                    views.erase(at);
                }

                return said(Sort::done, frame.id);
            }
            case Sort::go: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                const auto how = static_cast<rendering::Going>(from.u8());

                if (how == rendering::Going::address) {
                    const std::string address = from.text();

                    from.ended();

                    if (!allowed(*view, address)) return failed(frame.id, Trouble{Kind::refused, "The view may not go there."});

                    view->web->Navigate(widened(address).c_str());
                } else {
                    from.ended();

                    if (how == rendering::Going::back) view->web->GoBack();
                    else if (how == rendering::Going::forward) view->web->GoForward();
                    else if (how == rendering::Going::reload) view->web->Reload();
                    else if (how == rendering::Going::stop) view->web->Stop();
                    else return failed(frame.id, Trouble{Kind::other, "No such way."});
                }

                return said(Sort::done, frame.id);
            }
            case Sort::click:
            case Sort::wheel: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                double zoom = 1;
                const double x = from.f64();
                const double y = from.f64();

                from.ended();
                view->controller->get_ZoomFactor(&zoom);

                const std::string call = frame.sort == Sort::click ? "tal.click(" : "tal.wheel(";

                return helped(*view, frame.id, call + number(x / zoom) + "," + number(y / zoom) + ")");
            }
            case Sort::key: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                const std::uint8_t named = from.u8();
                const std::uint32_t letter = from.u32();
                const std::uint8_t mods = from.u8();

                from.ended();

                return helped(*view, frame.id, "tal.key(" + literal(key_name(named)) + "," + std::to_string(letter) + "," + std::to_string(mods) + ")");
            }
            case Sort::type: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                const std::string text = from.text();

                from.ended();

                return helped(*view, frame.id, "tal.type(" + literal(text) + ")");
            }
            case Sort::zoom: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                const double factor = from.f64();

                from.ended();

                if (!(factor >= 0.25 && factor <= 5)) return failed(frame.id, Trouble{Kind::other, "A zoom out of range."});

                view->controller->put_ZoomFactor(factor);

                return said(Sort::done, frame.id);
            }
            case Sort::open:
            case Sort::render:
            case Sort::close: return failed(frame.id, Trouble{Kind::unsupported, "WebView2 renders no documents."});
            default: return failed(frame.id, Trouble{Kind::unsupported, "A question this engine does not know."});
        }
    } catch (const Trouble& why) {
        failed(frame.id, why);
    }
}

// What our window is told: a frame heard, the end of the program's words, a view to close.
LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wide, LPARAM given) {
    if (message == heard_frame) {
        const std::unique_ptr<Frame> frame{reinterpret_cast<Frame*>(given)};

        handled(*frame);

        return 0;
    }

    if (message == heard_end) gone(0);

    if (message == closing) {
        const std::unique_ptr<std::shared_ptr<View>> view{reinterpret_cast<std::shared_ptr<View>*>(given)};

        if ((*view)->controller) (*view)->controller->Close();

        return 0;
    }

    return DefWindowProcW(handle, message, wide, given);
}

// The program's words read on a thread of their own and handed to our window frame by frame;
// their end, or words that break the protocol, end us.
void reading() {
    rendering::Unframing unframing;
    std::byte buffer[65536];

    for (;;) {
        DWORD got = 0;

        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof buffer, &got, nullptr) || got == 0) break;

        unframing.add(std::span<const std::byte>{buffer, got});

        try {
            while (std::optional<Frame> frame = unframing.next()) PostMessageW(window, heard_frame, 0, reinterpret_cast<LPARAM>(new Frame(std::move(*frame))));
        } catch (const Trouble&) {
            break;
        }
    }

    PostMessageW(window, heard_end, 0, 0);
}

// The folders of our runs that ended swept away: those whose file of life nobody holds open.
void swept(const std::filesystem::path& base) {
    std::error_code wrong;

    for (const auto& entry : std::filesystem::directory_iterator{base, wrong}) {
        if (!entry.path().filename().wstring().starts_with(prefix) || !entry.is_directory(wrong) || entry.is_symlink(wrong)) continue;

        if (!DeleteFileW((entry.path() / L"alive").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) continue;

        std::error_code ignored;

        std::filesystem::remove_all(entry.path(), ignored);
    }
}

}

// The engine: its wire (bytes as they are: in text mode the runtime would put a CR before every LF
// of a frame), its folder, the runtime asked for, the hello, then the program's words on our
// window's loop.
int main() {
    const int own = _dup(1);

    if (own < 0 || _dup2(2, 1) < 0 || _setmode(own, _O_BINARY) < 0) return 4;

    wire = _fdopen(own, "wb");

    if (wire == nullptr) return 4;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    LPWSTR version = nullptr;

    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) unavailable("WebView2 Runtime is not installed.");

    const std::string runtime = narrow(version);

    CoTaskMemFree(version);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) unavailable("WebView2 could not start COM in this session.");

    wchar_t temporary[MAX_PATH + 1];

    GetTempPathW(MAX_PATH, temporary);

    const std::filesystem::path base{temporary};

    swept(base);
    folder = base / (std::wstring{prefix} + std::to_wstring(GetCurrentProcessId()));

    std::error_code made;

    std::filesystem::create_directories(folder, made);

    if (CreateFileW((folder / L"alive").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) == INVALID_HANDLE_VALUE)
        unavailable("tal-render-webview2 could not hold its folder.");

    _wputenv_s(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"");
    _wputenv_s(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"");
    _wputenv_s(L"WEBVIEW2_USER_DATA_FOLDER", L"");

    WNDCLASSW kind{};

    kind.lpfnWndProc = procedure;
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = L"tal-render-webview2";
    RegisterClassW(&kind);
    window = CreateWindowExW(0, kind.lpszClassName, L"tal-render-webview2", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, kind.hInstance, nullptr);

    if (window == nullptr) unavailable("WebView2 has no window in this session (a service or a remote shell without a desktop).");

    rendering::Hello hello;

    hello.version = rendering::version;
    hello.engine = "WebView2 " + runtime;
    hello.abilities = rendering::pages | rendering::scripts | rendering::network;
    said(Sort::hello, 0, rendering::greeted(hello));

    std::thread{reading}.detach();

    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    gone(0);
}
