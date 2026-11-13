// SPDX-License-Identifier: GPL-3.0-or-later
#define AWM_COMPAT_IMPLEMENTATION
#include "windows_compat.h"
#undef recv
#undef send
#undef accept
#undef fcntl
#undef setsockopt
#undef close
#include <stdio.h>
#include <stdarg.h>
#include <iphlpapi.h>
#include "utils.h"
#include "log.h"
#include "preflight.h"

unsigned int log_level = LL_NOTICE;
int awm_usb_backend_usbdk = 0;

static int socket_result(int result)
{
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        errno = error == WSAEWOULDBLOCK ? EAGAIN : EIO;
    }
    return result;
}

int awm_recv(mux_socket_t socket, void *buffer, int length, int flags)
{
    return socket_result(recv((SOCKET)socket, buffer, length, flags));
}

int awm_send(mux_socket_t socket, const void *buffer, int length, int flags)
{
    return socket_result(send((SOCKET)socket, buffer, length, flags));
}

mux_socket_t awm_accept(mux_socket_t socket, struct sockaddr *address, int *length)
{
    return (mux_socket_t)accept((SOCKET)socket, address, length);
}

int awm_fcntl(mux_socket_t socket, int operation, int flags)
{
    if (operation == F_GETFL) return 0;
    u_long nonblocking = (flags & O_NONBLOCK) != 0;
    return socket_result(ioctlsocket((SOCKET)socket, FIONBIO, &nonblocking));
}

int awm_setsockopt(mux_socket_t socket, int level, int option, const void *value, int size)
{
    return socket_result(setsockopt((SOCKET)socket, level, option, value, size));
}

// Pair records are available over the mux protocol. Authenticate loopback peers
// against the process token before the upstream command parser sees any data.
int awm_authorize_client(mux_socket_t socket)
{
    struct sockaddr_in peer;
    int length = sizeof(peer);
    if (getpeername((SOCKET)socket, (struct sockaddr*)&peer, &length) != 0 ||
        peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
    DWORD size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) return 0;
    PMIB_TCPTABLE_OWNER_PID table = malloc(size);
    if (!table) return 0;
    DWORD pid = 0;
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        for (DWORD index = 0; index < table->dwNumEntries; ++index) {
            MIB_TCPROW_OWNER_PID *row = &table->table[index];
            if (row->dwLocalAddr == peer.sin_addr.s_addr && row->dwLocalPort == peer.sin_port) {
                struct sockaddr_in local;
                int local_size = sizeof(local);
                if (getsockname((SOCKET)socket, (struct sockaddr*)&local, &local_size) == 0 &&
                    row->dwRemoteAddr == local.sin_addr.s_addr && row->dwRemotePort == local.sin_port) {
                    pid = row->dwOwningPid;
                    break;
                }
            }
        }
    }
    free(table);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    HANDLE peer_token = NULL, own_token = NULL;
    int allowed = 0;
    if (process && OpenProcessToken(process, TOKEN_QUERY, &peer_token) &&
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own_token)) {
        BYTE peer_user[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
        BYTE own_user[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
        DWORD returned;
        if (GetTokenInformation(peer_token, TokenUser, peer_user, sizeof(peer_user), &returned) &&
            GetTokenInformation(own_token, TokenUser, own_user, sizeof(own_user), &returned)) {
            allowed = EqualSid(((TOKEN_USER*)peer_user)->User.Sid, ((TOKEN_USER*)own_user)->User.Sid);
        }
    }
    if (peer_token) CloseHandle(peer_token);
    if (own_token) CloseHandle(own_token);
    if (process) CloseHandle(process);
    return allowed;
}

void fdlist_create(struct fdlist *list)
{
    memset(list, 0, sizeof(*list));
}

void fdlist_add(struct fdlist *list, enum fdowner owner, mux_socket_t fd, short events)
{
    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 8;
        void *owners = realloc(list->owners, sizeof(*list->owners) * capacity);
        void *fds = realloc(list->fds, sizeof(*list->fds) * capacity);
        if (!owners || !fds) abort();
        list->owners = owners;
        list->fds = fds;
        list->capacity = capacity;
    }
    list->owners[list->count] = owner;
    list->fds[list->count].fd = (SOCKET)fd;
    list->fds[list->count].events = events;
    list->fds[list->count].revents = 0;
    ++list->count;
}

void fdlist_free(struct fdlist *list)
{
    free(list->owners);
    free(list->fds);
    memset(list, 0, sizeof(*list));
}

void fdlist_reset(struct fdlist *list) { list->count = 0; }
uint64_t mstime64(void) { return GetTickCount64(); }
void get_tick_count(struct timeval *value)
{
    uint64_t now = GetTickCount64();
    value->tv_sec = (long)(now / 1000);
    value->tv_usec = (long)((now % 1000) * 1000);
}

void usbmuxd_log(enum loglevel level, const char *format, ...)
{
    if ((unsigned int)level > log_level) return;
    fprintf(stderr, "[%llu][%d] ", GetTickCount64(), level);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void preflight_device_remove_cb(void *data) { (void)data; }
void preflight_worker_device_add(struct device_info *info) { client_device_add(info); }
