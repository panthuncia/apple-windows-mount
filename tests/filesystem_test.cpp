// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.h"
#include "afc_session.h"
#include "names.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <thread>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class FakeSource : public awm::FileSource {
    struct Entry {
        bool directory = false;
        uint64_t size = 0;
        std::map<uint64_t, std::array<unsigned char, 4096>> blocks;
    };
    std::map<std::string, std::shared_ptr<Entry>> files_;
    std::map<uint64_t, std::shared_ptr<Entry>> handles_;
    uint64_t next_ = 1;
    void connected() { if (disconnected) throw awm::DeviceError("Injected disconnect", AFC_E_MUX_ERROR); }
    std::shared_ptr<Entry> lookup(const std::string& path) {
        connected();
        if (!files_.contains(path)) throw awm::DeviceError("Missing fake path", AFC_E_OBJECT_NOT_FOUND);
        return files_.at(path);
    }
public:
    std::atomic<bool> disconnected{false}, full{false}, rename_failure{false};
    FakeSource() { auto root = std::make_shared<Entry>(); root->directory = true; files_["/"] = root; }
    awm::Metadata stat(const std::string& path) override {
        auto entry = lookup(path);
        awm::Metadata output;
        output.directory = entry->directory; output.size = entry->size;
        return output;
    }
    std::vector<std::string> list(const std::string& path) override {
        require(lookup(path)->directory, "list regular fake file");
        std::string prefix = path == "/" ? path : path + "/";
        std::vector<std::string> result;
        for (const auto& [name, unused] : files_) {
            if (name.rfind(prefix, 0) != 0 || name == path) continue;
            auto suffix = name.substr(prefix.size());
            if (suffix.find('/') == std::string::npos) result.push_back(suffix);
        }
        return result;
    }
    std::pair<uint64_t, uint64_t> capacity() override { connected(); return {UINT64_C(1) << 40, UINT64_C(1) << 39}; }
    uint64_t open(const std::string& path, bool writable) override {
        connected();
        if (!files_.contains(path)) {
            if (!writable) throw awm::DeviceError("Missing fake file", AFC_E_OBJECT_NOT_FOUND);
            files_[path] = std::make_shared<Entry>();
        }
        auto handle = next_++; handles_[handle] = files_.at(path); return handle;
    }
    void close(uint64_t handle) override { require(handles_.erase(handle) == 1, "backend handle closed twice"); }
    uint32_t read(uint64_t handle, uint64_t offset, void* buffer, uint32_t length) override {
        connected();
        length = std::min<uint32_t>(length, 7);
        auto entry = handles_.at(handle);
        auto count = offset >= entry->size ? 0 : static_cast<uint32_t>(std::min<uint64_t>(length, entry->size - offset));
        auto bytes = static_cast<unsigned char*>(buffer);
        for (uint32_t index = 0; index < count; ++index) {
            auto position = offset + index;
            auto block = entry->blocks.find(position / 4096);
            bytes[index] = block == entry->blocks.end() ? 0 : block->second[position % 4096];
        }
        return count;
    }
    uint32_t write(uint64_t handle, uint64_t offset, const void* buffer, uint32_t length) override {
        connected();
        if (full) throw awm::DeviceError("Injected disk full", AFC_E_NO_SPACE_LEFT);
        length = std::min<uint32_t>(length, 11);
        auto entry = handles_.at(handle);
        auto bytes = static_cast<const unsigned char*>(buffer);
        for (uint32_t index = 0; index < length; ++index) entry->blocks[(offset + index) / 4096][(offset + index) % 4096] = bytes[index];
        entry->size = std::max(entry->size, offset + length);
        return length;
    }
    void resize(uint64_t handle, uint64_t size) override {
        connected();
        auto entry = handles_.at(handle); entry->size = size;
        std::erase_if(entry->blocks, [size](const auto& block) { return block.first * 4096 >= size; });
        if (size % 4096 && entry->blocks.contains(size / 4096))
            std::fill(entry->blocks.at(size / 4096).begin() + size % 4096, entry->blocks.at(size / 4096).end(), 0);
    }
    void mkdir(const std::string& path) override {
        connected();
        if (files_.contains(path)) throw awm::DeviceError("Existing fake directory", AFC_E_OBJECT_EXISTS);
        auto entry = std::make_shared<Entry>(); entry->directory = true; files_[path] = entry;
    }
    void remove(const std::string& path) override {
        auto entry = lookup(path);
        if (entry->directory && !list(path).empty()) throw awm::DeviceError("Nonempty fake directory", AFC_E_DIR_NOT_EMPTY);
        files_.erase(path);
    }
    void rename(const std::string& from, const std::string& to) override {
        if (rename_failure) throw awm::DeviceError("Injected rename failure", AFC_E_IO_ERROR);
        auto entry = lookup(from);
        require(!files_.contains(to), "unexpected replacement"); files_[to] = entry; files_.erase(from);
    }
    void set_time(const std::string&, uint64_t) override { connected(); }
};
class File {
public:
    HANDLE value;
    explicit File(HANDLE handle) : value(handle) {}
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
int main()
{
    try {
        auto available = GetLogicalDrives();
        std::wstring drive;
        for (wchar_t letter : std::wstring(L"ZYXWVUTSRQPONMLKJIHGFE")) if (!(available & (1UL << (letter - 'A')))) { drive = std::wstring(1, letter) + L":"; break; }
        require(!drive.empty(), "No test drive letter available");
        auto source = std::make_shared<FakeSource>();
        auto factory = [source](const std::string&) -> std::shared_ptr<awm::FileSource> { return source; };
        auto apps = [] { return std::vector<std::string>{"example.documents"}; };
        {
            awm::Volume volume("test-generation", drive, false, false, factory, apps);
            volume.start();
            auto directory = drive + L"\\Media\\test";
            require(CreateDirectoryW(directory.c_str(), nullptr), "Create directory through WinFsp");
            for (unsigned index = 0; index < 20; ++index) {
                File first(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                File second(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                require(first.value != INVALID_HANDLE_VALUE && second.value != INVALID_HANDLE_VALUE, "Independent directory handles");
            }
            auto path = directory + L"\\payload.bin";
            {
                File file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, 0, nullptr));
                require(file.value != INVALID_HANDLE_VALUE, "Create file");
                std::array<unsigned char, 4096> expected{}, actual{};
                for (size_t index = 0; index < expected.size(); ++index) expected[index] = static_cast<unsigned char>(index);
                DWORD count;
                require(WriteFile(file.value, expected.data(), static_cast<DWORD>(expected.size()), &count, nullptr) && count == expected.size(), "Write payload");
                LARGE_INTEGER position{};
                require(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN), "Seek to beginning");
                require(ReadFile(file.value, actual.data(), static_cast<DWORD>(actual.size()), &count, nullptr) && expected == actual, "Read payload");
                position.QuadPart = (INT64_C(1) << 32) + 17;
                require(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN), "64-bit seek");
                require(WriteFile(file.value, expected.data(), 3, &count, nullptr) && count == 3, "Write beyond 4GiB");
                require(FlushFileBuffers(file.value), "Flush acknowledged writes");
                LARGE_INTEGER size;
                require(GetFileSizeEx(file.value, &size) && size.QuadPart == position.QuadPart + 3, "64-bit metadata");
                auto renamed = directory + L"\\renamed.bin";
                require(MoveFileExW(path.c_str(), renamed.c_str(), 0), "Rename an open file");
                position.QuadPart = 0;
                require(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN), "Seek renamed handle");
                require(ReadFile(file.value, actual.data(), 3, &count, nullptr) && count == 3 && actual[1] == 1, "Read through renamed handle");
                position.QuadPart = 10;
                require(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN) && SetEndOfFile(file.value), "Truncate open handle");
                path = renamed;
            }
            {
                File exclusive(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
                require(exclusive.value != INVALID_HANDLE_VALUE, "Exclusive open");
                File conflict(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
                require(conflict.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "Windows sharing modes");
            }
            auto cross_source = drive + L"\\Apps\\example.documents\\moved.bin";
            require(!MoveFileExW(path.c_str(), cross_source.c_str(), 0) && GetLastError() == ERROR_NOT_SAME_DEVICE, "Reject cross-source rename");
            source->rename_failure = true;
            require(!MoveFileExW(path.c_str(), (directory + L"\\failed.bin").c_str(), 0), "Injected rename failure is reported");
            source->rename_failure = false;
            require(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, "Rename failure preserves the source");
            {
                File writer(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr));
                require(writer.value != INVALID_HANDLE_VALUE, "Open fault-injection writer");
                source->full = true;
                DWORD count;
                BYTE value = 42;
                require(!WriteFile(writer.value, &value, 1, &count, nullptr) && GetLastError() == ERROR_DISK_FULL, "Disk full is reported");
                source->full = false;
            }
            source->disconnected = true;
            {
                File lost(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
                require(lost.value == INVALID_HANDLE_VALUE, "Disconnected service rejects new opens");
            }
            source->disconnected = false;
            require(DeleteFileW(path.c_str()), "Delete file");
            require(RemoveDirectoryW(directory.c_str()), "Delete directory");
            volume.stop();
        }
        {
            awm::Volume read_only("test-read-only", drive, true, true, factory, apps);
            read_only.start();
            File denied(CreateFileW((drive + L"\\denied.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
            require(denied.value == INVALID_HANDLE_VALUE, "Read-only volume rejects creation");
            read_only.stop();
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << " (Win32 " << GetLastError() << ")\n"; return 1; }
}
