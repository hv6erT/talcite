#include <gtk/gtk.h>
#include <glib-unix.h>
#include <webkit/webkit.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "helper_of.hpp"
#include "rendering.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// tal-render-gtk (E6e): the web engine of tal on Linux, WebKitGTK 6 on GTK 4 in a process of its
// own, speaking the protocol of tal's renderers (RENDERING.md, version 2) for pages: navigate,
// shot, resize, dismiss, go, click, wheel, key, type and zoom, and telling of its views unasked.
//
// The windows show on a private Broadway display (gtk4-broadwayd, its HTTP on a socket in a folder
// of ours, no TCP port), started here and killed with us (PR_SET_PDEATHSIG); the folder lives
// under the real XDG_RUNTIME_DIR, 0700, and is held by a lock on a file in it while we live, so
// that a start sweeps away the folders of processes gone (ours only, links never followed). No
// session bus and no accessibility bridge. The frames go out on a copy of the standard output,
// which itself goes to the standard error, so that nothing GTK or WebKit prints breaks them.
//
// Each view has a network session of its own, ephemeral. With the network off a proxy leads
// nowhere and a content filter lets only file: of no host or of localhost through (file://host is
// a connection to that host); DNS prefetching is off. Page scripts are on only when asked; the
// input (helper.js) runs in a world of its own either way. A view goes on by itself only where its
// Browsing allows; javascript: never, data: and blob: never as the page itself; new windows,
// downloads, dialogs, permissions and choosing files are refused, and every refusal told. Pages
// from file: reach no other files from scripts. The death of a page's process is told as lost.
namespace {

namespace rendering = tal::rendering;
using rendering::Frame;
using rendering::Sort;
using tal::Bytes;
using tal::Kind;
using tal::Trouble;

constexpr std::string_view prefix = "tal-render-gtk-";
constexpr const char* world = "tal";

std::filesystem::path folder;
pid_t broadway = 0;
FILE* wire = nullptr;
rendering::Unframing unframing;
WebKitUserContentFilter* local_only = nullptr;

// Ends the process at once with `code`, the display with it.
[[noreturn]] void gone(int code) {
    if (broadway > 0) kill(broadway, SIGKILL);

    std::error_code ignored;

    std::filesystem::remove_all(folder, ignored);
    std::_Exit(code);
}

// The frame of `sort` with `id` and `payload` written; a program that hears no more ends us.
void said(Sort sort, std::uint32_t id, Bytes payload = {}) {
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

// `text` cut to at most 64 KiB at a whole character, as the protocol holds titles and addresses.
std::string bounded(const char* text) {
    std::string made = text != nullptr ? tal::text::repaired(text) : std::string{};

    if (made.size() <= rendering::widest_said) return made;

    std::size_t end = rendering::widest_said;

    while (end > 0 && (static_cast<unsigned char>(made[end]) & 0xC0U) == 0x80U) --end;

    made.resize(end);

    return made;
}

struct View {
    std::uint32_t number = 0;
    GtkWidget* window = nullptr;
    WebKitWebView* web = nullptr;
    WebKitNetworkSession* session = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool scripts = false;
    bool network = false;
    std::optional<std::uint32_t> loading;
    std::optional<std::uint32_t> sizing;
    guint ticking = 0;
    int ticks = 0;
};

std::map<std::uint32_t, std::unique_ptr<View>> views;

// Whether `uri` is a file of no host or of localhost.
bool local(std::string_view uri) { return uri.starts_with("file:///") || uri.starts_with("file://localhost/"); }

// Whether `view` may go to `uri` as its page: never javascript:, data: or blob:; with the network
// off only local files (and about:blank); with it on, the web and local files.
bool allowed(const View& view, std::string_view uri) {
    if (uri == "about:blank" || local(uri)) return true;

    return view.network && (uri.starts_with("http://") || uri.starts_with("https://"));
}

// What `view` refused for `reason` (the protocol's number), at `uri`, told.
void refused(const View& view, std::uint8_t reason, const char* uri) {
    said(Sort::refused, 0, rendering::refused_with(rendering::Refused{view.number, reason, bounded(uri)}));
}

// The state of `view` told: its title, address, loading and ways back and forward.
void told(const View& view) {
    rendering::Changed made;

    made.view = view.number;
    made.title = bounded(webkit_web_view_get_title(view.web));
    made.address = bounded(webkit_web_view_get_uri(view.web));
    made.loading = webkit_web_view_is_loading(view.web) != FALSE;
    made.back = webkit_web_view_can_go_back(view.web) != FALSE;
    made.forward = webkit_web_view_can_go_forward(view.web) != FALSE;
    said(Sort::changed, 0, rendering::changed_with(made));
}

constexpr gint64 patience = 8 * G_USEC_PER_SEC;

struct Shooting {
    std::uint32_t id = 0;
    std::uint32_t number = 0;
    gint64 since = g_get_monotonic_time();
};

void shot_taken(GObject* source, GAsyncResult* result, gpointer data);

// The picture of the view `asked` names asked for again (`shot_taken` answers), or a trouble when
// the view is gone meanwhile.
gboolean shot_again(gpointer data) {
    auto* asked = static_cast<Shooting*>(data);
    const auto found = views.find(asked->number);

    if (found == views.end()) {
        failed(asked->id, Trouble{Kind::broken, "No picture of the page: the view is gone."});
        delete asked;

        return G_SOURCE_REMOVE;
    }

    webkit_web_view_get_snapshot(found->second->web, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, shot_taken, asked);

    return G_SOURCE_REMOVE;
}

// The picture of a view taken: its pixels as the protocol has them, rows top to bottom, when it
// has the view's size. Just after a new size WebKit may still give a picture of the old one (the
// widget has its size, the page's compositor not yet its frame): it is asked again every 20 ms,
// for up to eight seconds from the question (a busy machine may take that long; the program waits
// ten for the answer, so the trouble comes from here and says why), and only then is it a trouble.
void shot_taken(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<Shooting> asked{static_cast<Shooting*>(data)};
    GError* error = nullptr;
    GdkTexture* texture = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
    const auto found = views.find(asked->number);

    if (texture == nullptr || found == views.end()) {
        failed(asked->id, Trouble{Kind::broken, std::string{"No picture of the page: "} + (error != nullptr ? error->message : "the view is gone.")});

        if (error != nullptr) g_error_free(error);

        if (texture != nullptr) g_object_unref(texture);

        return;
    }

    const View& view = *found->second;
    const auto wide = static_cast<std::uint32_t>(gdk_texture_get_width(texture));
    const auto high = static_cast<std::uint32_t>(gdk_texture_get_height(texture));

    if (wide != view.width || high != view.height) {
        g_object_unref(texture);

        if (g_get_monotonic_time() - asked->since < patience) {
            g_timeout_add(20, shot_again, asked.release());

            return;
        }

        failed(asked->id, Trouble{Kind::other, "The picture of the page is not of the view's size yet."});

        return;
    }

    GdkTextureDownloader* downloader = gdk_texture_downloader_new(texture);

    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);

    gsize stride = 0;
    GBytes* bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
    gsize size = 0;
    const auto* pixels = static_cast<const std::byte*>(g_bytes_get_data(bytes, &size));
    rendering::Writing made;

    made.text(bounded(webkit_web_view_get_title(view.web))).text(bounded(webkit_web_view_get_uri(view.web))).u32(wide).u32(high);

    for (std::uint32_t row = 0; row < high; ++row) made.bytes(std::span<const std::byte>{pixels + std::size_t{row} * stride, std::size_t{wide} * 4});

    said(Sort::done, asked->id, made.done());
    g_bytes_unref(bytes);
    gdk_texture_downloader_free(downloader);
    g_object_unref(texture);
}

// A change of loading: told; the first page loaded answers the navigate that opened the view.
void load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer data) {
    View& view = *static_cast<View*>(data);

    told(view);

    if (event == WEBKIT_LOAD_FINISHED && view.loading) {
        said(Sort::done, *view.loading);
        view.loading.reset();
    }
}

// A page that did not load: told; the navigate that opened the view fails with it.
gboolean load_failed(WebKitWebView*, WebKitLoadEvent, gchar* uri, GError* error, gpointer data) {
    View& view = *static_cast<View*>(data);

    if (view.loading && !g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        failed(*view.loading, Trouble{Kind::broken, std::string{"The page "} + uri + " did not load: " + error->message});
        view.loading.reset();
    } else if (view.loading && g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)) {
        failed(*view.loading, Trouble{Kind::refused, "The view may not go there."});
        view.loading.reset();
    }

    told(view);

    return TRUE;
}

// The title or the address changed: told.
void notified(GObject*, GParamSpec*, gpointer data) { told(*static_cast<View*>(data)); }

// Where the view goes and what it takes: a page only where its Browsing allows, no new windows,
// nothing it cannot show (a download); each refusal told.
gboolean deciding(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) {
    View& view = *static_cast<View*>(data);

    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitURIRequest* request = webkit_navigation_action_get_request(webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision)));

        refused(view, 0, webkit_uri_request_get_uri(request));
        webkit_policy_decision_ignore(decision);

        return TRUE;
    }

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));

        if (uri != nullptr && allowed(view, uri)) return FALSE;

        refused(view, 1, uri);
        webkit_policy_decision_ignore(decision);

        return TRUE;
    }

    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* response = WEBKIT_RESPONSE_POLICY_DECISION(decision);

        if (!webkit_response_policy_decision_is_mime_type_supported(response)) {
            refused(view, 2, webkit_uri_request_get_uri(webkit_response_policy_decision_get_request(response)));
            webkit_policy_decision_ignore(decision);

            return TRUE;
        }
    }

    return FALSE;
}

// A dialog of the page's scripts closed at once (confirm false, prompt empty), and told.
gboolean dialog(WebKitWebView* web, WebKitScriptDialog* script, gpointer data) {
    const WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(script);

    if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM || type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM) webkit_script_dialog_confirm_set_confirmed(script, FALSE);

    if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) webkit_script_dialog_prompt_set_text(script, "");

    refused(*static_cast<View*>(data), 3, webkit_web_view_get_uri(web));

    return TRUE;
}

// A request for a permission (place, camera, notifications and the like) denied, and told.
gboolean permission(WebKitWebView* web, WebKitPermissionRequest* request, gpointer data) {
    webkit_permission_request_deny(request);
    refused(*static_cast<View*>(data), 4, webkit_web_view_get_uri(web));

    return TRUE;
}

// A choice of files to send cancelled, and told.
gboolean choosing(WebKitWebView* web, WebKitFileChooserRequest* request, gpointer data) {
    webkit_file_chooser_request_cancel(request);
    refused(*static_cast<View*>(data), 5, webkit_web_view_get_uri(web));

    return TRUE;
}

// A download that started anyway cancelled, and told.
void downloading(WebKitNetworkSession*, WebKitDownload* download, gpointer data) {
    webkit_download_cancel(download);
    refused(*static_cast<View*>(data), 2, webkit_uri_request_get_uri(webkit_download_get_request(download)));
}

// The process of the page ended (its memory, a crash): the view lost, told; its opening fails.
void terminated(WebKitWebView*, WebKitWebProcessTerminationReason, gpointer data) {
    View& view = *static_cast<View*>(data);

    if (view.loading) failed(*view.loading, Trouble{Kind::broken, "The process of the page ended."});

    view.loading.reset();
    said(Sort::lost, 0, rendering::Writing().u32(view.number).done());
}

// A new web view asked by the page: none.
GtkWidget* created(WebKitWebView*, WebKitNavigationAction*, gpointer) { return nullptr; }

// The view of a navigate frame opened on the display: its settings, session and filter as its
// Browsing allows, its signals, the page asked; answered when it has loaded.
void navigate(const Frame& frame) {
    rendering::Reading from{frame.payload};
    auto view = std::make_unique<View>();

    view->number = from.u32();

    const std::string address = from.text();

    view->scripts = from.u8() != 0;
    view->network = from.u8() != 0;
    view->width = from.u32();
    view->height = from.u32();
    from.ended();

    if (views.contains(view->number)) return failed(frame.id, Trouble{Kind::other, "A view of that number is open."});

    if (!allowed(*view, address)) return failed(frame.id, Trouble{Kind::refused, "The view may not go there."});

    view->session = webkit_network_session_new_ephemeral();

    if (!view->network) {
        WebKitNetworkProxySettings* nowhere = webkit_network_proxy_settings_new("http://127.0.0.1:9", nullptr);

        webkit_network_session_set_proxy_settings(view->session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, nowhere);
        webkit_network_proxy_settings_free(nowhere);
    }

    WebKitSettings* settings = webkit_settings_new();

    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_javascript_markup(settings, view->scripts);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_settings_set_allow_file_access_from_file_urls(settings, FALSE);
    webkit_settings_set_allow_universal_access_from_file_urls(settings, FALSE);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    webkit_settings_set_enable_dns_prefetching(settings, FALSE);
    G_GNUC_END_IGNORE_DEPRECATIONS

    WebKitUserContentManager* content = webkit_user_content_manager_new();
    WebKitUserScript* helper = webkit_user_script_new_for_world(std::string{helper_of::helper}.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, world, nullptr, nullptr);

    webkit_user_content_manager_add_script(content, helper);
    webkit_user_script_unref(helper);

    if (!view->network) webkit_user_content_manager_add_filter(content, local_only);

    view->web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", view->session, "settings", settings, "user-content-manager", content, nullptr));
    g_object_unref(settings);
    g_object_unref(content);

    view->window = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(view->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(view->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(view->window), static_cast<int>(view->width), static_cast<int>(view->height));
    gtk_widget_set_size_request(GTK_WIDGET(view->web), static_cast<int>(view->width), static_cast<int>(view->height));
    gtk_window_set_child(GTK_WINDOW(view->window), GTK_WIDGET(view->web));

    View* held = view.get();

    g_signal_connect(view->web, "load-changed", G_CALLBACK(load_changed), held);
    g_signal_connect(view->web, "load-failed", G_CALLBACK(load_failed), held);
    g_signal_connect(view->web, "notify::title", G_CALLBACK(notified), held);
    g_signal_connect(view->web, "notify::uri", G_CALLBACK(notified), held);
    g_signal_connect(view->web, "decide-policy", G_CALLBACK(deciding), held);
    g_signal_connect(view->web, "script-dialog", G_CALLBACK(dialog), held);
    g_signal_connect(view->web, "permission-request", G_CALLBACK(permission), held);
    g_signal_connect(view->web, "run-file-chooser", G_CALLBACK(choosing), held);
    g_signal_connect(view->web, "web-process-terminated", G_CALLBACK(terminated), held);
    g_signal_connect(view->web, "create", G_CALLBACK(created), held);
    g_signal_connect(view->session, "download-started", G_CALLBACK(downloading), held);
    gtk_window_present(GTK_WINDOW(view->window));
    gtk_widget_allocate(GTK_WIDGET(view->web), static_cast<int>(view->width), static_cast<int>(view->height), -1, nullptr);

    view->loading = frame.id;
    webkit_web_view_load_uri(view->web, address.c_str());
    views.emplace(view->number, std::move(view));
}

// The window of `view` laid out anew at the size its page asks: the toplevel presented again,
// which has GTK compute its size from what the page requests. Without it the toplevel keeps its
// first size (no backend recomputes it on a request of its child), and the next layout of the
// window puts the page back to that size, whatever was allocated to it in between.
void presented(const View& view) {
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(view.window));

    if (surface == nullptr || !GDK_IS_TOPLEVEL(surface)) return;

    GdkToplevelLayout* layout = gdk_toplevel_layout_new();

    gdk_toplevel_layout_set_resizable(layout, FALSE);
    gdk_toplevel_present(GDK_TOPLEVEL(surface), layout);
    gdk_toplevel_layout_unref(layout);
}

// A resize answered once the window, its surface and the page all have the size asked; the window
// presented again every half second while they do not.
gboolean ticked(gpointer data) {
    View& view = *static_cast<View*>(data);
    GtkWidget* widget = GTK_WIDGET(view.web);
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(view.window));
    const auto sized = [&view](int wide, int high) { return static_cast<std::uint32_t>(wide) == view.width && static_cast<std::uint32_t>(high) == view.height; };

    if (!sized(gtk_widget_get_width(widget), gtk_widget_get_height(widget)) || surface == nullptr || !sized(gdk_surface_get_width(surface), gdk_surface_get_height(surface))) {
        if (++view.ticks % 50 == 0) presented(view);

        return G_SOURCE_CONTINUE;
    }

    view.ticking = 0;

    if (view.sizing) said(Sort::done, *view.sizing);

    view.sizing.reset();

    return G_SOURCE_REMOVE;
}

struct Acting {
    std::uint32_t id = 0;
    std::uint32_t number = 0;
};

// An input done by the helper: answered, a file to send refused and told.
void acted(GObject* source, GAsyncResult* result, gpointer data) {
    const std::unique_ptr<Acting> asked{static_cast<Acting*>(data)};
    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);

    if (value == nullptr) {
        failed(asked->id, Trouble{Kind::broken, std::string{"The input did not reach the page: "} + (error != nullptr ? error->message : "?")});

        if (error != nullptr) g_error_free(error);

        return;
    }

    char* text = jsc_value_to_string(value);
    const bool upload = text != nullptr && std::string_view{text} == "upload";

    g_free(text);
    g_object_unref(value);

    const auto found = views.find(asked->number);

    if (upload && found != views.end()) refused(*found->second, 5, webkit_web_view_get_uri(found->second->web));

    said(Sort::done, asked->id);
}

// `script` run by the helper's world in `view`, answering `id` when done.
void helped(View& view, std::uint32_t id, const std::string& script) {
    webkit_web_view_evaluate_javascript(view.web, script.data(), static_cast<gssize>(script.size()), world, nullptr, nullptr, acted, new Acting{id, view.number});
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

// The view of `number` for the question `id`, or none, the question failed then.
View* found(std::uint32_t id, std::uint32_t number) {
    const auto at = views.find(number);

    if (at != views.end()) return at->second.get();

    failed(id, Trouble{Kind::missing, "No such view."});

    return nullptr;
}

// The name of a named key of the protocol as the helper calls it.
const char* key_name(std::uint8_t named) {
    constexpr const char* names[] = {"", "enter", "tab", "backspace", "delete", "escape", "up", "down", "left", "right", "home", "end", "pageup", "pagedown"};

    return named < std::size(names) ? names[named] : "";
}

// A frame of the program done: answered, or failed with what went wrong.
void handled(const Frame& frame) {
    try {
        rendering::Reading from{frame.payload};

        switch (frame.sort) {
            case Sort::navigate: return navigate(frame);
            case Sort::shot: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                webkit_web_view_get_snapshot(view->web, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, shot_taken, new Shooting{frame.id, view->number});

                return;
            }
            case Sort::resize: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                view->width = from.u32();
                view->height = from.u32();
                from.ended();

                if (view->sizing) failed(*view->sizing, Trouble{Kind::other, "Another size was asked."});

                view->sizing = frame.id;
                gtk_widget_set_size_request(GTK_WIDGET(view->web), static_cast<int>(view->width), static_cast<int>(view->height));
                gtk_window_set_default_size(GTK_WINDOW(view->window), static_cast<int>(view->width), static_cast<int>(view->height));
                gtk_widget_allocate(GTK_WIDGET(view->web), static_cast<int>(view->width), static_cast<int>(view->height), -1, nullptr);
                presented(*view);
                view->ticks = 0;

                if (view->ticking == 0) view->ticking = g_timeout_add(10, ticked, view);

                return;
            }
            case Sort::dismiss: {
                const auto at = views.find(from.u32());

                if (at != views.end()) {
                    View& view = *at->second;

                    for (const std::optional<std::uint32_t>& waiting : {view.loading, view.sizing})
                        if (waiting) failed(*waiting, Trouble{Kind::broken, "The view was closed."});

                    if (view.ticking != 0) g_source_remove(view.ticking);

                    g_signal_handlers_disconnect_by_data(view.web, &view);
                    g_signal_handlers_disconnect_by_data(view.session, &view);
                    gtk_window_destroy(GTK_WINDOW(view.window));
                    g_object_unref(view.session);
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

                    webkit_web_view_load_uri(view->web, address.c_str());
                } else {
                    from.ended();

                    if (how == rendering::Going::back) webkit_web_view_go_back(view->web);
                    else if (how == rendering::Going::forward) webkit_web_view_go_forward(view->web);
                    else if (how == rendering::Going::reload) webkit_web_view_reload(view->web);
                    else if (how == rendering::Going::stop) webkit_web_view_stop_loading(view->web);
                    else return failed(frame.id, Trouble{Kind::other, "No such way."});
                }

                return said(Sort::done, frame.id);
            }
            case Sort::click:
            case Sort::wheel: {
                View* view = found(frame.id, from.u32());

                if (view == nullptr) return;

                const double zoom = webkit_web_view_get_zoom_level(view->web);
                const double x = from.f64();
                const double y = from.f64();

                from.ended();

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

                webkit_web_view_set_zoom_level(view->web, factor);

                return said(Sort::done, frame.id);
            }
            case Sort::open:
            case Sort::render:
            case Sort::close: return failed(frame.id, Trouble{Kind::unsupported, "WebKitGTK renders no documents."});
            default: return failed(frame.id, Trouble{Kind::unsupported, "A question this engine does not know."});
        }
    } catch (const Trouble& why) {
        failed(frame.id, why);
    }
}

// What the program wrote, read and done frame by frame; its end, or words that break the
// protocol, end us.
gboolean heard(gint fd, GIOCondition, gpointer) {
    std::byte buffer[65536];
    const ssize_t got = read(fd, buffer, sizeof buffer);

    if (got <= 0) gone(0);

    unframing.add(std::span<const std::byte>{buffer, static_cast<std::size_t>(got)});

    try {
        while (std::optional<Frame> frame = unframing.next()) handled(*frame);
    } catch (const Trouble&) {
        gone(2);
    }

    return G_SOURCE_CONTINUE;
}

// The filter of the network made: the hello said, the program heard from now on.
void filtered(GObject* source, GAsyncResult* result, gpointer) {
    GError* error = nullptr;

    local_only = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);

    if (local_only == nullptr) unavailable(std::string{"WebKitGTK could not make its filter: "} + (error != nullptr ? error->message : "?"));

    rendering::Hello hello;

    hello.version = rendering::version;
    hello.engine = "WebKitGTK " + std::to_string(webkit_get_major_version()) + "." + std::to_string(webkit_get_minor_version()) + "." +
                   std::to_string(webkit_get_micro_version());
    hello.abilities = rendering::pages | rendering::scripts | rendering::network;
    said(Sort::hello, 0, rendering::greeted(hello));
    g_unix_fd_add(0, G_IO_IN, heard, nullptr);
}

// The folder our runs live in: the real XDG_RUNTIME_DIR when it is ours and closed to others,
// else the temporary folder.
std::filesystem::path runtime() {
    if (const char* given = std::getenv("XDG_RUNTIME_DIR")) {
        struct stat seen {};

        if (lstat(given, &seen) == 0 && S_ISDIR(seen.st_mode) && seen.st_uid == getuid() && (seen.st_mode & 077) == 0) return given;
    }

    return std::filesystem::temp_directory_path();
}

// The folders of our runs that ended swept away: ours, real folders (no links), whose lock
// nobody holds.
void swept(const std::filesystem::path& base) {
    std::error_code wrong;

    for (const auto& entry : std::filesystem::directory_iterator{base, wrong}) {
        const std::string name = entry.path().filename().string();
        struct stat seen {};

        if (!name.starts_with(prefix) || lstat(entry.path().c_str(), &seen) != 0 || !S_ISDIR(seen.st_mode) || seen.st_uid != getuid()) continue;

        const int lock = open((entry.path() / "alive").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
            close(lock);
            continue;
        }

        if (lock >= 0) close(lock);

        std::error_code ignored;

        std::filesystem::remove_all(entry.path(), ignored);
    }
}

// Where `program` lies along PATH, or empty.
std::string searched(const std::string& program) {
    const char* path = std::getenv("PATH");

    for (std::string_view rest = path != nullptr ? path : ""; !rest.empty();) {
        const std::size_t end = std::min(rest.find(':'), rest.size());
        const std::string made = std::string{rest.substr(0, end)} + "/" + program;

        if (end > 0 && access(made.c_str(), X_OK) == 0) return made;

        rest.remove_prefix(std::min(end + 1, rest.size()));
    }

    return {};
}

}

// The engine: its wire, its folder and display, GTK, the filter, then the hello and the program.
int main() {
    const int own = dup(1);

    if (own < 0 || dup2(2, 1) < 0) return 4;

    wire = fdopen(own, "wb");

    if (wire == nullptr) return 4;

    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    setenv("NO_AT_BRIDGE", "1", 1);
    setenv("GTK_A11Y", "none", 1);
    setenv("GSETTINGS_BACKEND", "memory", 1);

    const std::filesystem::path base = runtime();

    swept(base);

    std::string made = (base / (std::string{prefix} + "XXXXXX")).string();

    if (mkdtemp(made.data()) == nullptr) unavailable("tal-render-gtk could not make its folder.");

    folder = made;

    const int lock = open((folder / "alive").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);

    if (lock < 0 || flock(lock, LOCK_EX) != 0) unavailable("tal-render-gtk could not lock its folder.");

    const std::string broadwayd = searched("gtk4-broadwayd");

    if (broadwayd.empty()) unavailable("gtk4-broadwayd is not installed (package libgtk-4-bin).");

    const std::string display = ":" + std::to_string(getpid() % 60000 + 1);
    const std::string socket = (folder / "http.socket").string();
    std::vector<std::string> words{broadwayd, "--unixsocket", socket, display};
    std::vector<char*> argv;

    for (std::string& one : words) argv.push_back(one.data());

    argv.push_back(nullptr);
    setenv("XDG_RUNTIME_DIR", folder.c_str(), 1);

    const pid_t parent = getpid();

    broadway = fork();

    if (broadway < 0) unavailable("tal-render-gtk could not start gtk4-broadwayd.");

    if (broadway == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        if (getppid() != parent) std::_Exit(1);

        const int nothing = open("/dev/null", O_RDWR);

        dup2(nothing, 0);
        dup2(nothing, 1);
        dup2(nothing, 2);
        execv(argv.front(), argv.data());
        std::_Exit(1);
    }

    const std::string broadway_socket = (folder / ("broadway" + display.substr(1) + ".socket")).string();

    for (int tries = 0; tries < 400; ++tries) {
        struct stat seen {};

        if (stat(broadway_socket.c_str(), &seen) == 0) break;

        usleep(5000);
    }

    setenv("GDK_BACKEND", "broadway", 1);
    setenv("BROADWAY_DISPLAY", display.c_str(), 1);

    if (!gtk_init_check()) unavailable("GTK could not open the private display of tal-render-gtk.");

    const std::string rules = R"([{"trigger":{"url-filter":".*"},"action":{"type":"block"}},)"
                              R"({"trigger":{"url-filter":"^file:///"},"action":{"type":"ignore-previous-rules"}},)"
                              R"({"trigger":{"url-filter":"^file://localhost/"},"action":{"type":"ignore-previous-rules"}}])";
    GBytes* source = g_bytes_new(rules.data(), rules.size());
    WebKitUserContentFilterStore* store = webkit_user_content_filter_store_new((folder / "filters").c_str());

    webkit_user_content_filter_store_save(store, "local-only", source, nullptr, filtered, nullptr);
    g_bytes_unref(source);

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    g_main_loop_run(loop);
    gone(0);
}
