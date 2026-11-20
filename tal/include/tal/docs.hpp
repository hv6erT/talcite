#pragma once

#include <chrono>
#include <cstddef>
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

    [[nodiscard]] virtual bool takes(std::span<const std::byte> head) const = 0;

    [[nodiscard]] virtual Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from) = 0;
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

    [[nodiscard]] Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from) override;

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

class ScriptedRenderer : public Renderer {
public:
    explicit ScriptedRenderer(std::vector<Page> pages = {Page{612, 792}});

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] bool takes(std::span<const std::byte> head) const override;

    [[nodiscard]] Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from) override;

    std::vector<Page> pages;
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
