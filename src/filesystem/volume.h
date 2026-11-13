// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <vector>
namespace awm {
class FileSource;
using SourceFactory = std::function<std::shared_ptr<FileSource>(const std::string&)>;
using AppEnumerator = std::function<std::vector<std::string>()>;
class Volume {
public:
    Volume(std::string udid, std::wstring drive, bool read_only, bool media_only,
        SourceFactory source_factory = {}, AppEnumerator app_enumerator = {});
    ~Volume();
    void start();
    void stop();
    void refresh_apps();
    bool failed() const;
    bool stalled() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
