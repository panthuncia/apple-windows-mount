// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <winternl.h>
using PNTSTATUS = NTSTATUS*;
#pragma warning(push)
#pragma warning(disable:4324)
#include <winfsp/winfsp.h>
#pragma warning(pop)
#include <sddl.h>
#include "volume.h"
#include "afc_session.h"
#include "names.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <limits>

namespace awm {
namespace {
constexpr uint64_t windows_epoch = UINT64_C(116444736000000000);
struct StatusError { NTSTATUS value; };
void require(bool condition, NTSTATUS failure) { if (!condition) throw StatusError{failure}; }
NTSTATUS afc_status(int code)
{
    switch (code) {
    case AFC_E_SUCCESS: return STATUS_SUCCESS;
    case AFC_E_OBJECT_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
    case AFC_E_OBJECT_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
    case AFC_E_DIR_NOT_EMPTY: return STATUS_DIRECTORY_NOT_EMPTY;
    case AFC_E_OBJECT_IS_DIR: return STATUS_FILE_IS_A_DIRECTORY;
    case AFC_E_PERM_DENIED: return STATUS_ACCESS_DENIED;
    case AFC_E_NO_SPACE_LEFT: return STATUS_DISK_FULL;
    case AFC_E_OBJECT_BUSY: return STATUS_SHARING_VIOLATION;
    case AFC_E_INVALID_ARG: return STATUS_INVALID_PARAMETER;
    case AFC_E_OP_NOT_SUPPORTED: return STATUS_NOT_SUPPORTED;
    case AFC_E_OP_TIMEOUT: return STATUS_IO_TIMEOUT;
    case AFC_E_SERVICE_NOT_CONNECTED: case AFC_E_MUX_ERROR: return STATUS_DEVICE_NOT_CONNECTED;
    case AFC_E_NO_MEM: case AFC_E_NO_RESOURCES: return STATUS_INSUFFICIENT_RESOURCES;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}
struct Route { std::string source; std::string path = "/"; };
std::string child_path(const std::string& parent, const std::string& name) { return parent == "/" ? parent + name : parent + "/" + name; }
std::string key(const Route& route) { return route.source + ":" + route.path; }
Metadata directory_metadata() { Metadata value; value.directory = true; return value; }
void file_info(const Metadata& metadata, uint64_t identity, FSP_FSCTL_FILE_INFO* output, bool read_only)
{
    *output = {};
    output->FileAttributes = metadata.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (read_only) output->FileAttributes |= FILE_ATTRIBUTE_READONLY;
    output->FileSize = metadata.directory ? 0 : metadata.size;
    output->AllocationSize = (output->FileSize + 4095) & ~UINT64_C(4095);
    output->CreationTime = metadata.created_ns / 100 + windows_epoch;
    output->LastWriteTime = output->ChangeTime = metadata.modified_ns / 100 + windows_epoch;
    output->LastAccessTime = output->LastWriteTime;
    output->IndexNumber = identity;
    output->HardLinks = 1;
}
}

struct Volume::Impl {
    struct Node { Route route; uint64_t identity; bool deleted = false; };
    struct Handle {
        std::shared_ptr<Node> node;
        std::shared_ptr<FileSource> session;
        uint64_t remote = 0;
        bool remote_open = false;
        bool writable = false;
        PVOID directory_buffer = nullptr;
        ~Handle() { FspFileSystemDeleteDirectoryBuffer(&directory_buffer); }
    };
    struct CachedDirectory {
        std::chrono::steady_clock::time_point expires;
        std::vector<DirectoryName> names;
    };
    std::string udid;
    std::wstring drive;
    bool read_only, media_only;
    std::atomic<bool> fault{false};
    std::atomic<uint64_t> operation_started{0};
    std::recursive_mutex mutex;
    std::map<std::string, std::shared_ptr<FileSource>> sessions;
    SourceFactory factory;
    AppEnumerator app_enumerator;
    std::map<std::string, std::weak_ptr<Node>> nodes;
    std::map<std::string, CachedDirectory> directories;
    std::vector<std::string> apps;
    uint64_t next_identity = 1;
    FSP_FILE_SYSTEM* filesystem = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    DWORD security_size = 0;

    Impl(std::string identifier, std::wstring mountpoint, bool ro, bool media, SourceFactory source_factory, AppEnumerator enumerate)
        : udid(std::move(identifier)), drive(std::move(mountpoint)), read_only(ro), media_only(media), factory(std::move(source_factory)), app_enumerator(std::move(enumerate)) {
        if (!factory) factory = [this](const std::string& source) { return std::make_shared<AfcSession>(udid, source == "media" ? "" : source.substr(4)); };
        if (!app_enumerator) app_enumerator = [this] {
            Device device(udid);
            std::vector<std::string> identifiers;
            for (const auto& app : device.apps()) identifiers.push_back(app.at("bundle_id").get<std::string>());
            return identifiers;
        };
        sessions["media"] = factory("media");
        if (!media_only) refresh_apps();
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), STATUS_ACCESS_DENIED);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> bytes(size);
        BOOL queried = GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
        CloseHandle(token);
        require(queried, STATUS_ACCESS_DENIED);
        LPWSTR sid = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid), STATUS_ACCESS_DENIED);
        std::wstring sddl = L"O:" + std::wstring(sid) + L"G:" + sid + L"D:P(A;;FA;;;" + sid + L")(A;;FA;;;SY)";
        LocalFree(sid);
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, &security_size), STATUS_ACCESS_DENIED);
    }
    ~Impl() { stop(); if (security) LocalFree(security); }
    void refresh_apps() {
        std::lock_guard lock(mutex);
        apps = app_enumerator();
        directories.clear();
    }
    std::shared_ptr<FileSource> session(const std::string& source) {
        auto found = sessions.find(source);
        if (found != sessions.end() && !found->second->failed()) return found->second;
        if (source == "media") {
            auto created = factory(source);
            sessions[source] = created;
            return created;
        }
        require(source.rfind("app:", 0) == 0, STATUS_OBJECT_PATH_NOT_FOUND);
        auto created = factory(source);
        sessions[source] = created;
        return created;
    }
    std::vector<DirectoryName> children(const Route& route) {
        if (route.source == "@root") return {{"Media", L"Media"}, {"Apps", L"Apps"}};
        if (route.source == "@apps") return directory_names(apps);
        auto cache_key = key(route);
        auto found = directories.find(cache_key);
        auto now = std::chrono::steady_clock::now();
        if (found != directories.end() && found->second.expires > now) return found->second.names;
        auto names = directory_names(session(route.source)->list(route.path));
        if (directories.size() >= 64) directories.clear();
        directories[cache_key] = {now + std::chrono::seconds(1), names};
        return names;
    }
    Route child(const Route& route, const std::string& original) {
        if (route.source == "@root") return {original == "Media" ? "media" : "@apps", "/"};
        if (route.source == "@apps") return {"app:" + original, "/"};
        return {route.source, child_path(route.path, original)};
    }
    Metadata stat(const Route& route) {
        if (route.path == "/") return directory_metadata();
        auto info = session(route.source)->stat(route.path);
        require(!info.symlink, STATUS_NOT_SUPPORTED);
        require(info.size <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 4095, STATUS_FILE_TOO_LARGE);
        return info;
    }
    Route resolve(const std::wstring& path, bool allow_new = false) {
        std::vector<std::wstring> components;
        try { components = path_components(path); } catch (...) { throw StatusError{STATUS_OBJECT_NAME_INVALID}; }
        Route route{media_only ? "media" : "@root", "/"};
        for (size_t index = 0; index < components.size(); ++index) {
            require(stat(route).directory, STATUS_NOT_A_DIRECTORY);
            auto names = children(route);
            auto found = std::find_if(names.begin(), names.end(), [&](const auto& entry) { return equal_name(entry.visible, components[index]); });
            if (found == names.end()) {
                require(allow_new && index + 1 == components.size() && route.source.front() != '@', STATUS_OBJECT_NAME_NOT_FOUND);
                std::string decoded;
                try { decoded = decode_name(components[index]); } catch (...) { throw StatusError{STATUS_OBJECT_NAME_INVALID}; }
                route = child(route, decoded);
            } else route = child(route, found->original);
        }
        return route;
    }
    std::shared_ptr<Node> node(const Route& route) {
        if (nodes.size() > 8192) std::erase_if(nodes, [](const auto& entry) { return entry.second.expired(); });
        auto cache_key = key(route);
        auto existing = nodes[cache_key].lock();
        if (existing && !existing->deleted) return existing;
        auto created = std::make_shared<Node>(Node{route, next_identity++});
        nodes[cache_key] = created;
        return created;
    }
    void writable(const Route& route) {
        require(!read_only, STATUS_MEDIA_WRITE_PROTECTED);
        require(route.source.front() != '@' && route.path != "/", STATUS_ACCESS_DENIED);
    }
    void info(Handle* handle, FSP_FSCTL_FILE_INFO* output) { file_info(stat(handle->node->route), handle->node->identity, output, read_only); }
    static Impl& self(FSP_FILE_SYSTEM* fs) { return *static_cast<Impl*>(fs->UserContext); }
    template<class F> static NTSTATUS guard(FSP_FILE_SYSTEM* fs, F&& function) noexcept {
        auto& volume = self(fs);
        std::lock_guard lock(volume.mutex);
        if (volume.fault) return STATUS_DEVICE_NOT_CONNECTED;
        volume.operation_started = GetTickCount64();
        struct Finish { std::atomic<uint64_t>& time; ~Finish() { time = 0; } } finish{volume.operation_started};
        try { function(volume); return STATUS_SUCCESS; }
        catch (const StatusError& error) { return error.value; }
        catch (const DeviceError& error) {
            std::cerr << error.what() << '\n';
            auto result = afc_status(error.code());
            return result;
        }
        catch (const std::bad_alloc&) { return STATUS_INSUFFICIENT_RESOURCES; }
        catch (...) { return STATUS_IO_DEVICE_ERROR; }
    }
    static NTSTATUS volume_info(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* output) {
        return guard(fs, [&](Impl& v) {
            *output = {};
            auto [total, free] = v.session("media")->capacity();
            output->TotalSize = total; output->FreeSize = free;
            std::wstring label = L"iOS " + widen(v.udid.substr(v.udid.size() > 8 ? v.udid.size() - 8 : 0));
            output->VolumeLabelLength = static_cast<UINT16>(label.size() * sizeof(wchar_t));
            memcpy(output->VolumeLabel, label.data(), output->VolumeLabelLength);
        });
    }
    static NTSTATUS security_by_name(FSP_FILE_SYSTEM* fs, PWSTR path, PUINT32 attributes, PSECURITY_DESCRIPTOR descriptor, SIZE_T* size) {
        return guard(fs, [&](Impl& v) {
            auto metadata = v.stat(v.resolve(path));
            if (attributes) *attributes = metadata.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
            v.copy_security(descriptor, size);
        });
    }
    void copy_security(PSECURITY_DESCRIPTOR descriptor, SIZE_T* size) {
        if (!size) return;
        auto supplied = *size;
        *size = security_size;
        require(supplied >= security_size, STATUS_BUFFER_OVERFLOW);
        if (descriptor) memcpy(descriptor, security, security_size);
    }
    static NTSTATUS open(FSP_FILE_SYSTEM* fs, PWSTR path, UINT32 options, UINT32 access, PVOID* context, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) {
            auto route = v.resolve(path);
            auto metadata = v.stat(route);
            require(!(options & FILE_DIRECTORY_FILE) || metadata.directory, STATUS_NOT_A_DIRECTORY);
            require(!(options & FILE_NON_DIRECTORY_FILE) || !metadata.directory, STATUS_FILE_IS_A_DIRECTORY);
            auto handle = std::make_unique<Handle>();
            handle->node = v.node(route);
            handle->writable = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
            if (handle->writable && !metadata.directory) v.writable(route);
            if (!metadata.directory) {
                handle->session = v.session(route.source);
                handle->remote = handle->session->open(route.path, handle->writable);
                handle->remote_open = true;
            }
            file_info(metadata, handle->node->identity, output, v.read_only);
            *context = handle.release();
        });
    }
    static NTSTATUS create(FSP_FILE_SYSTEM* fs, PWSTR path, UINT32 options, UINT32 access, UINT32 attributes,
        PSECURITY_DESCRIPTOR, UINT64, PVOID* context, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) {
            auto route = v.resolve(path, true);
            v.writable(route);
            require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), STATUS_NOT_SUPPORTED);
            try { v.stat(route); throw StatusError{STATUS_OBJECT_NAME_COLLISION}; }
            catch (const DeviceError& error) { if (error.code() != AFC_E_OBJECT_NOT_FOUND) throw; }
            auto handle = std::make_unique<Handle>();
            handle->node = v.node(route);
            handle->session = v.session(route.source);
            bool directory = (options & FILE_DIRECTORY_FILE) != 0;
            if (directory) handle->session->mkdir(route.path);
            else {
                handle->remote = handle->session->open(route.path, true);
                handle->remote_open = true;
            }
            handle->writable = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
            v.directories.clear();
            Metadata metadata;
            metadata.directory = directory;
            file_info(metadata, handle->node->identity, output, false);
            *context = handle.release();
        });
    }
    static NTSTATUS overwrite(FSP_FILE_SYSTEM* fs, PVOID context, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            v.writable(h->node->route);
            require(h->remote_open && h->writable, STATUS_ACCESS_DENIED);
            h->session->resize(h->remote, 0);
            v.directories.clear();
            v.info(h, output);
        });
    }
    static VOID cleanup(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR, ULONG flags) {
        if (!(flags & FspCleanupDelete)) return;
        auto result = guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            v.writable(h->node->route);
            v.session(h->node->route.source)->remove(h->node->route.path);
            h->node->deleted = true;
            v.directories.clear();
        });
        if (!NT_SUCCESS(result)) { self(fs).fault = true; std::cerr << "Device deletion failed during cleanup, status " << std::hex << result << '\n'; }
    }
    static VOID close(FSP_FILE_SYSTEM* fs, PVOID context) {
        auto& v = self(fs);
        std::lock_guard lock(v.mutex);
        std::unique_ptr<Handle> h(static_cast<Handle*>(context));
        if (h->remote_open) try { h->session->close(h->remote); }
            catch (const std::exception& error) { std::cerr << "Remote close failed: " << error.what() << '\n'; }
    }
    static NTSTATUS read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length, PULONG transferred) {
        *transferred = 0;
        return guard(fs, [&](Impl&) {
            auto h = static_cast<Handle*>(context);
            require(h->remote_open, STATUS_FILE_IS_A_DIRECTORY);
            require(offset <= INT64_MAX, STATUS_INVALID_PARAMETER);
            while (*transferred < length) {
                auto count = h->session->read(h->remote, offset + *transferred, static_cast<BYTE*>(buffer) + *transferred, length - *transferred);
                require(count <= length - *transferred, STATUS_IO_DEVICE_ERROR);
                if (!count) break;
                *transferred += count;
            }
            require(*transferred || !length, STATUS_END_OF_FILE);
        });
    }
    static NTSTATUS write(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
        BOOLEAN append, BOOLEAN constrained, PULONG transferred, FSP_FSCTL_FILE_INFO* output) {
        *transferred = 0;
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            v.writable(h->node->route);
            require(h->remote_open && h->writable, STATUS_ACCESS_DENIED);
            auto metadata = v.stat(h->node->route);
            if (append) offset = metadata.size;
            require(offset <= INT64_MAX && length <= INT64_MAX - offset, STATUS_FILE_TOO_LARGE);
            if (constrained) length = offset >= metadata.size ? 0 : static_cast<ULONG>(std::min<uint64_t>(length, metadata.size - offset));
            while (*transferred < length) {
                auto count = h->session->write(h->remote, offset + *transferred, static_cast<const BYTE*>(buffer) + *transferred, length - *transferred);
                require(count && count <= length - *transferred, STATUS_IO_DEVICE_ERROR);
                *transferred += count;
            }
            v.directories.clear();
            v.info(h, output);
        });
    }
    static NTSTATUS flush(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) { if (context && output) v.info(static_cast<Handle*>(context), output); });
    }
    static NTSTATUS get_info(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) { v.info(static_cast<Handle*>(context), output); });
    }
    static NTSTATUS basic_info(FSP_FILE_SYSTEM* fs, PVOID context, UINT32 attributes, UINT64 creation, UINT64 access,
        UINT64 modified, UINT64 change, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            // Creation/access/change times are reported from device metadata.
            // Windows copy tools commonly request all timestamps in one call.
            (void)creation; (void)access; (void)change;
            if (modified) {
                v.writable(h->node->route);
                require(modified >= windows_epoch && modified - windows_epoch <= UINT64_MAX / 100, STATUS_INVALID_PARAMETER);
                v.session(h->node->route.source)->set_time(h->node->route.path, (modified - windows_epoch) * 100);
            }
            if (attributes != INVALID_FILE_ATTRIBUTES)
                require(!(attributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_DIRECTORY)), STATUS_NOT_SUPPORTED);
            v.info(h, output);
        });
    }
    static NTSTATUS set_size(FSP_FILE_SYSTEM* fs, PVOID context, UINT64 size, BOOLEAN allocation, FSP_FSCTL_FILE_INFO* output) {
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            v.writable(h->node->route);
            require(size <= INT64_MAX && h->remote_open && h->writable, STATUS_INVALID_PARAMETER);
            auto metadata = v.stat(h->node->route);
            if (!allocation || size < metadata.size) h->session->resize(h->remote, size);
            v.directories.clear();
            v.info(h, output);
        });
    }
    static NTSTATUS can_delete(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR) {
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            v.writable(h->node->route);
            if (v.stat(h->node->route).directory) require(v.session(h->node->route.source)->list(h->node->route.path).empty(), STATUS_DIRECTORY_NOT_EMPTY);
        });
    }
    static NTSTATUS rename(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR, PWSTR destination, BOOLEAN replace) {
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            auto from = h->node->route;
            auto to = v.resolve(destination, true);
            v.writable(from); v.writable(to);
            require(from.source == to.source, STATUS_NOT_SAME_DEVICE);
            if (from.path == to.path) {
                auto components = path_components(destination);
                if (components.back().rfind(L"~H", 0) == 0 || components.back().rfind(L"~h", 0) == 0) return;
                auto requested = decode_name(components.back());
                to.path = from.path.substr(0, from.path.rfind('/') + 1) + requested;
                if (from.path == to.path) return;
            }
            try { v.stat(to); throw StatusError{replace ? STATUS_NOT_SUPPORTED : STATUS_OBJECT_NAME_COLLISION}; }
            catch (const DeviceError& error) { if (error.code() != AFC_E_OBJECT_NOT_FOUND) throw; }
            struct Relocated { std::shared_ptr<Node> node; std::string path; };
            std::vector<Relocated> relocated;
            std::map<std::string, std::weak_ptr<Node>> replacement;
            for (auto& [unused, reference] : v.nodes) if (auto n = reference.lock()) {
                auto path = n->route.path;
                if (n->route.source == from.source && (path == from.path || path.rfind(from.path + "/", 0) == 0)) path.replace(0, from.path.size(), to.path);
                replacement[key({n->route.source, path})] = n;
                relocated.push_back({std::move(n), std::move(path)});
            }
            v.session(from.source)->rename(from.path, to.path);
            for (auto& entry : relocated) entry.node->route.path.swap(entry.path);
            v.nodes.swap(replacement);
            v.directories.clear();
        });
    }
    static NTSTATUS get_security(FSP_FILE_SYSTEM* fs, PVOID, PSECURITY_DESCRIPTOR output, SIZE_T* size) {
        return guard(fs, [&](Impl& v) { v.copy_security(output, size); });
    }
    static NTSTATUS read_directory(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR, PWSTR marker, PVOID buffer, ULONG length, PULONG transferred) {
        *transferred = 0;
        return guard(fs, [&](Impl& v) {
            auto h = static_cast<Handle*>(context);
            require(v.stat(h->node->route).directory, STATUS_NOT_A_DIRECTORY);
            NTSTATUS result = STATUS_SUCCESS;
            if (FspFileSystemAcquireDirectoryBuffer(&h->directory_buffer, marker == nullptr, &result)) {
                try {
                    auto entries = v.children(h->node->route);
                    for (const auto& entry : entries) {
                        Route route = v.child(h->node->route, entry.original);
                        Metadata metadata;
                        try { metadata = v.stat(route); }
                        catch (const StatusError& error) { if (error.value == STATUS_NOT_SUPPORTED) continue; throw; }
                        catch (const DeviceError& error) { if (error.code() == AFC_E_OBJECT_NOT_FOUND) continue; throw; }
                        v.operation_started = GetTickCount64();
                        auto n = v.node(route);
                        std::vector<BYTE> storage(sizeof(FSP_FSCTL_DIR_INFO) + entry.visible.size() * sizeof(wchar_t));
                        auto info = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage.data());
                        info->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + entry.visible.size() * sizeof(wchar_t));
                        file_info(metadata, n->identity, &info->FileInfo, v.read_only);
                        memcpy(info->FileNameBuf, entry.visible.data(), entry.visible.size() * sizeof(wchar_t));
                        if (!FspFileSystemFillDirectoryBuffer(&h->directory_buffer, info, &result)) break;
                    }
                } catch (...) { FspFileSystemReleaseDirectoryBuffer(&h->directory_buffer); throw; }
                FspFileSystemReleaseDirectoryBuffer(&h->directory_buffer);
            }
            require(NT_SUCCESS(result), result);
            FspFileSystemReadDirectoryBuffer(&h->directory_buffer, marker, buffer, length, transferred);
        });
    }
    void start() {
        FSP_FSCTL_VOLUME_PARAMS parameters{};
        parameters.Version = sizeof(parameters);
        parameters.SectorSize = 512;
        parameters.SectorsPerAllocationUnit = 8;
        parameters.VolumeSerialNumber = static_cast<UINT32>(std::hash<std::string>{}(udid));
        parameters.FileInfoTimeout = 1000;
        parameters.CasePreservedNames = 1;
        parameters.UnicodeOnDisk = 1;
        parameters.ReadOnlyVolume = read_only;
        parameters.UmFileContextIsUserContext2 = 1;
        parameters.FlushAndPurgeOnCleanup = 1;
        wcscpy_s(parameters.FileSystemName, L"AFC");
        static FSP_FILE_SYSTEM_INTERFACE interface = [] {
            FSP_FILE_SYSTEM_INTERFACE table{};
            table.GetVolumeInfo = volume_info; table.GetSecurityByName = security_by_name;
            table.Create = create; table.Open = open; table.Overwrite = overwrite;
            table.Cleanup = cleanup; table.Close = close; table.Read = read; table.Write = write;
            table.Flush = flush; table.GetFileInfo = get_info; table.SetBasicInfo = basic_info;
            table.SetFileSize = set_size; table.CanDelete = can_delete; table.Rename = rename;
            table.GetSecurity = get_security; table.ReadDirectory = read_directory;
            return table;
        }();
        NTSTATUS result = FspFileSystemCreate(const_cast<PWSTR>(L"WinFsp.Disk"), &parameters, &interface, &filesystem);
        require(NT_SUCCESS(result), result);
        filesystem->UserContext = this;
        result = FspFileSystemSetMountPoint(filesystem, drive.data());
        require(NT_SUCCESS(result), result);
        result = FspFileSystemStartDispatcher(filesystem, 4);
        require(NT_SUCCESS(result), result);
    }
    void stop() {
        if (!filesystem) return;
        FspFileSystemStopDispatcher(filesystem);
        FspFileSystemDelete(filesystem);
        filesystem = nullptr;
    }
};
Volume::Volume(std::string udid, std::wstring drive, bool ro, bool media, SourceFactory factory, AppEnumerator enumerate) {
    try { impl_ = std::make_unique<Impl>(std::move(udid), std::move(drive), ro, media, std::move(factory), std::move(enumerate)); }
    catch (const StatusError& error) { throw std::runtime_error("Volume initialization failed, NTSTATUS " + std::to_string(static_cast<uint32_t>(error.value))); }
}
Volume::~Volume() = default;
void Volume::start() { try { impl_->start(); } catch (const StatusError& error) { throw std::runtime_error("WinFsp mount failed, NTSTATUS " + std::to_string(static_cast<uint32_t>(error.value))); } }
void Volume::stop() { impl_->stop(); }
void Volume::refresh_apps() { impl_->refresh_apps(); }
bool Volume::failed() const { return impl_->fault; }
bool Volume::stalled() const { auto started = impl_->operation_started.load(); return started && GetTickCount64() - started > 30000; }
}
