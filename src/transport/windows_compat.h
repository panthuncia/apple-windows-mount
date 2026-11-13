// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

typedef intptr_t mux_socket_t;
typedef int socklen_t;
#define __attribute__(value)
#define F_GETFL 1
#define F_SETFL 2
#define O_NONBLOCK 1
#define usleep(microseconds) Sleep(((microseconds) + 999) / 1000)

// The upstream mux protocol uses the BSD TCP header wire layout.
#pragma pack(push, 1)
struct tcphdr {
    uint16_t th_sport, th_dport;
    uint32_t th_seq, th_ack;
    uint8_t th_x2 : 4;
    uint8_t th_off : 4;
    uint8_t th_flags;
    uint16_t th_win, th_sum, th_urp;
};
#pragma pack(pop)
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_ACK 0x10

#ifdef __cplusplus
extern "C" {
#endif
extern int awm_usb_backend_usbdk;
int awm_recv(mux_socket_t socket, void *buffer, int length, int flags);
int awm_send(mux_socket_t socket, const void *buffer, int length, int flags);
mux_socket_t awm_accept(mux_socket_t socket, struct sockaddr *address, int *length);
int awm_fcntl(mux_socket_t socket, int operation, int flags);
int awm_setsockopt(mux_socket_t socket, int level, int option, const void *value, int size);
int awm_authorize_client(mux_socket_t socket);
#ifdef __cplusplus
}
#endif

#ifndef AWM_COMPAT_IMPLEMENTATION
#define recv awm_recv
#define send awm_send
#define accept awm_accept
#define fcntl awm_fcntl
#define setsockopt awm_setsockopt
#define close closesocket
#endif
