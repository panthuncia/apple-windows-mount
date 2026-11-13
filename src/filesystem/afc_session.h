// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "device.h"
#include <mutex>
#include <vector>
#include <atomic>
#include <libimobiledevice/house_arrest.h>
namespace awm {
struct Metadata {
    bool directory = false;
    bool symlink = false;
    uint64_t size = 0, modified_ns = 0, created_ns = 0;
};
class FileSource {
public:
    virtual ~FileSource() = default;
    virtual Metadata stat(const std::string&) = 0;
    virtual std::vector<std::string> list(const std::string&) = 0;
    virtual std::pair<uint64_t, uint64_t> capacity() = 0;
    virtual uint64_t open(const std::string&, bool) = 0;
    virtual void close(uint64_t) = 0;
    virtual uint32_t read(uint64_t, uint64_t, void*, uint32_t) = 0;
    virtual uint32_t write(uint64_t, uint64_t, const void*, uint32_t) = 0;
    virtual void resize(uint64_t, uint64_t) = 0;
    virtual void mkdir(const std::string&) = 0;
    virtual void remove(const std::string&) = 0;
    virtual void rename(const std::string&, const std::string&) = 0;
    virtual void set_time(const std::string&, uint64_t) = 0;
    virtual bool failed() const { return false; }
};
class AfcSession : public FileSource {
public:
    AfcSession(const std::string& udid, const std::string& app = {});
    Metadata stat(const std::string& path);
    std::vector<std::string> list(const std::string& path);
    std::pair<uint64_t, uint64_t> capacity();
    uint64_t open(const std::string& path, bool writable);
    void close(uint64_t handle);
    uint32_t read(uint64_t handle, uint64_t offset, void* buffer, uint32_t length);
    uint32_t write(uint64_t handle, uint64_t offset, const void* buffer, uint32_t length);
    void resize(uint64_t handle, uint64_t size);
    void mkdir(const std::string& path);
    void remove(const std::string& path);
    void rename(const std::string& from, const std::string& to);
    void set_time(const std::string& path, uint64_t ns);
    bool failed() const override { return failed_; }
private:
    void check(afc_error_t status, const char* operation);
    void ensure();
    std::string rooted(const std::string& path) const;
    DeviceHandle device_;
    LockdownHandle lockdown_;
    std::unique_ptr<house_arrest_client_private, decltype(&house_arrest_client_free)> house_{nullptr, house_arrest_client_free};
    AfcHandle afc_;
    std::string root_;
    std::mutex mutex_;
    std::atomic<bool> failed_{false};
};
}
