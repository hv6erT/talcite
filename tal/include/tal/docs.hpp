#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/meta.hpp"
#include "tal/pixels.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"

namespace tal::docs {

struct Page {
    double width = 0;
    double height = 0;

    friend bool operator==(const Page&, const Page&) = default;
};

struct Region {
    double left = 0;
    double top = 0;
    double width = 1;
    double height = 1;

    friend bool operator==(const Region&, const Region&) = default;
};

class Document {
public:
    Document() = default;
    virtual ~Document() = default;

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    [[nodiscard]] virtual const std::vector<Page>& pages() const = 0;

    [[nodiscard]] virtual Task<media::Pixels> rendered(std::size_t page, media::Size size, Region region) = 0;
};

class Renderer : public Adapter {
public:
    static constexpr std::string_view kind = "renderer";

    [[nodiscard]] static std::shared_ptr<Renderer> builtin();

    [[nodiscard]] virtual bool takes(std::span<const std::byte> head) const = 0;

    [[nodiscard]] virtual Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from, std::string password) = 0;
};

struct Browsing {
    bool scripts = false;
    bool network = false;
};

struct Shot {
    media::Pixels pixels;
    std::string title;
    std::string address;
};

class Browsed {
public:
    Browsed() = default;
    virtual ~Browsed() = default;

    Browsed(const Browsed&) = delete;
    Browsed& operator=(const Browsed&) = delete;

    [[nodiscard]] virtual Task<Shot> shot() = 0;

    [[nodiscard]] virtual Task<void> resized(media::Size size) = 0;
};

class Browser : public Adapter {
public:
    static constexpr std::string_view kind = "browser";

    [[nodiscard]] virtual Task<std::shared_ptr<Browsed>> opened(std::string address, Browsing how, media::Size size) = 0;
};

class Renderers final : public Renderer {
public:
    explicit Renderers(std::vector<std::shared_ptr<Renderer>> links, std::string name = "renderers");

    [[nodiscard]] std::string_view name() const override { return name_; }

    [[nodiscard]] bool takes(std::span<const std::byte> head) const override;

    [[nodiscard]] Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from, std::string password) override;

private:
    std::vector<std::shared_ptr<Renderer>> links_;
    std::string name_;
};

class Browsers final : public Browser {
public:
    explicit Browsers(std::vector<std::shared_ptr<Browser>> links, std::string name = "browsers");

    [[nodiscard]] std::string_view name() const override { return name_; }

    [[nodiscard]] Task<std::shared_ptr<Browsed>> opened(std::string address, Browsing how, media::Size size) override;

private:
    std::vector<std::shared_ptr<Browser>> links_;
    std::string name_;
};

struct Limits {
    std::chrono::milliseconds greeting{10000};
    std::chrono::milliseconds answer{10000};
    std::chrono::milliseconds browsing{30000};
};

using Recognizing = std::function<bool(std::span<const std::byte> head)>;

[[nodiscard]] std::shared_ptr<Renderer> renderer(Command program, Recognizing takes, Limits limits = {}, Port<Processes> processes = {});

[[nodiscard]] std::shared_ptr<Browser> browser(Command program, Limits limits = {}, Port<Processes> processes = {});

[[nodiscard]] std::shared_ptr<Renderer> pdfium(Limits limits = {}, Port<Processes> processes = {});

enum class Encoding : std::uint8_t { windows_1252, windows_1250, iso_8859_2 };

struct Anchor {
    std::size_t cell = 0;
    std::size_t from = 0;
    std::size_t upto = 0;
    std::string address;

    friend bool operator==(const Anchor&, const Anchor&) = default;
};

struct Block {
    enum class Kind : std::uint8_t { paragraph, heading, item, quote, preformatted, row, rule, page };

    Kind kind = Kind::paragraph;
    int level = 0;
    std::string mark;
    std::string text;
    std::vector<std::string> cells;
    std::vector<Anchor> anchors;

    friend bool operator==(const Block&, const Block&) = default;
};

struct Plain {
    std::string title;
    std::vector<Block> blocks;
    bool cut = false;

    friend bool operator==(const Plain&, const Plain&) = default;
};

struct Bounds {
    std::size_t bytes = std::size_t{16} << 20;
    std::size_t text = std::size_t{16} << 20;
    std::size_t blocks = 100000;
    std::size_t anchors = 100000;
    std::size_t depth = 256;
};

[[nodiscard]] Plain plain(std::span<const std::byte> html, std::string_view address = {}, Encoding fallback = Encoding::windows_1252, Bounds bounds = {});

class Prose {
public:
    Prose() = default;
    virtual ~Prose() = default;

    Prose(const Prose&) = delete;
    Prose& operator=(const Prose&) = delete;

    [[nodiscard]] virtual std::size_t pages() const = 0;

    [[nodiscard]] virtual Task<Plain> page(std::size_t which) = 0;
};

[[nodiscard]] Task<std::shared_ptr<Prose>> prose(std::shared_ptr<meta::Reader> from);

class ScriptedRenderer : public Renderer {
public:
    explicit ScriptedRenderer(std::vector<Page> pages = {Page{612, 792}});

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] bool takes(std::span<const std::byte> head) const override;

    [[nodiscard]] Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from, std::string password) override;

    std::vector<Page> pages;
    std::string password;
    std::size_t opening = 0;
};

class ScriptedBrowser : public Browser {
public:
    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] Task<std::shared_ptr<Browsed>> opened(std::string address, Browsing how, media::Size size) override;

    std::vector<std::string> addresses;
    std::vector<Browsing> ways;
};

}
