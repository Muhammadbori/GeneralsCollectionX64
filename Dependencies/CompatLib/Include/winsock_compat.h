// TheSuperHackers @build O154 (headless port) Winsock names on macOS and Linux, mapped to BSD sockets. The calls are the
// same ones (socket, bind, sendto, recvfrom ...); only the names of the handle type, the error codes and the start and
// clean-up calls differ. Network play is not part of the headless port, but the code that holds it must compile.
#pragma once

#include "windows_compat.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <cstddef>
#include <netdb.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
typedef struct hostent HOSTENT;
typedef struct hostent * LPHOSTENT;
typedef struct sockaddr SOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct in_addr IN_ADDR;
typedef struct timeval TIMEVAL;

typedef struct WSAData
{
	WORD wVersion;
	WORD wHighVersion;
	char szDescription[257];
	char szSystemStatus[129];
	unsigned short iMaxSockets;
	unsigned short iMaxUdpDg;
	char * lpVendorInfo;
} WSADATA, * LPWSADATA;

inline int WSAStartup(WORD version, WSADATA * data)
{
	if (data)
	{
		memset(data, 0, sizeof(*data));
		data->wVersion = version;
		data->wHighVersion = version;
	}
	return 0;
}
inline int WSACleanup(void) { return 0; }
inline int WSAGetLastError(void) { return errno; }
inline void WSASetLastError(int error) { errno = error; }
inline int closesocket(SOCKET s) { return close(s); }
inline int ioctlsocket(SOCKET s, long command, unsigned long * argument)
{
	int value = argument ? (int)*argument : 0;
	int result = ioctl(s, (unsigned long)command, &value);
	if (argument)
		*argument = (unsigned long)value;
	return result;
}

// The Winsock length arguments are int *; these overloads take them.
inline int getsockname(SOCKET s, struct sockaddr * name, int * length)
{
	socklen_t n = length ? (socklen_t)*length : 0;
	int result = ::getsockname(s, name, &n);
	if (length) *length = (int)n;
	return result;
}
inline int getpeername(SOCKET s, struct sockaddr * name, int * length)
{
	socklen_t n = length ? (socklen_t)*length : 0;
	int result = ::getpeername(s, name, &n);
	if (length) *length = (int)n;
	return result;
}
inline int getsockopt(SOCKET s, int level, int option, void * value, int * length)
{
	socklen_t n = length ? (socklen_t)*length : 0;
	int result = ::getsockopt(s, level, option, value, &n);
	if (length) *length = (int)n;
	return result;
}
inline ssize_t recvfrom(SOCKET s, void * buffer, size_t size, int flags, struct sockaddr * from, int * length)
{
	socklen_t n = length ? (socklen_t)*length : 0;
	ssize_t result = ::recvfrom(s, buffer, size, flags, from, length ? &n : nullptr);
	if (length) *length = (int)n;
	return result;
}
// A null length matches these exactly (it would be ambiguous between the int * and the socklen_t * forms).
inline ssize_t recvfrom(SOCKET s, void * buffer, size_t size, int flags, struct sockaddr * from, std::nullptr_t)
{
	return ::recvfrom(s, buffer, size, flags, from, (socklen_t *)nullptr);
}
inline SOCKET accept(SOCKET s, struct sockaddr * address, std::nullptr_t)
{
	return ::accept(s, address, (socklen_t *)nullptr);
}
inline SOCKET accept(SOCKET s, struct sockaddr * address, int * length)
{
	socklen_t n = length ? (socklen_t)*length : 0;
	SOCKET result = ::accept(s, address, length ? &n : nullptr);
	if (length) *length = (int)n;
	return result;
}

#define WSAEINTR EINTR
#define WSAEBADF EBADF
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEINVAL EINVAL
#define WSAEMFILE EMFILE
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAEOPNOTSUPP EOPNOTSUPP
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAEHOSTDOWN EHOSTDOWN
#define WSANOTINITIALISED 10093
#define WSAHOST_NOT_FOUND 11001
#define WSANO_DATA 11004
