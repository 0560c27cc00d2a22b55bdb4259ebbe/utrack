/*
utrack is a very small an efficient BitTorrent tracker
Copyright (C) 2013-2014 Arvid Norberg

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "socket.hpp"
#include "config.hpp"

#include <stdio.h> // for stderr
#include <errno.h> // for errno
#include <string.h> // for strerror
#include <stdlib.h> // for exit
#include <fcntl.h> // for F_GETFL and F_SETFL

#include <atomic>
#include <assert.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern std::atomic<uint32_t> bytes_out;

packet_socket::packet_socket(int listen_port, bool receive)
	: m_socket(-1)
	, m_receive(receive)
{
	m_socket = socket(PF_INET, SOCK_DGRAM, 0);
	if (m_socket < 0)
	{
		fprintf(stderr, "failed to open socket (%d): %s\n"
			, errno, strerror(errno));
		exit(1);
	}

	int opt = socket_buffer_size;
	int r = setsockopt(m_socket, SOL_SOCKET, m_receive ? SO_RCVBUF : SO_SNDBUF
		, (char const*)&opt, sizeof(opt));
	if (r == -1)
	{
		fprintf(stderr, "failed to set socket %s buffer size (%d): %s\n"
			, m_receive ? "receive" : "send", errno, strerror(errno));
	}

	int one = 1;
	if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR
		, (char const*)&one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEADDR on socket (%d): %s\n"
			, errno, strerror(errno));
	}

#ifdef SO_REUSEPORT
	if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
	{
		fprintf(stderr, "failed to set SO_REUSEPORT on socket (%d): %s\n"
			, errno, strerror(errno));
	}
#endif

	// we cannot bind the sockets meant for just outgoing packets to the
	// IP and port, since then they will swallow incoming packets
	if (m_receive)
	{
		sockaddr_in bind_addr;
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_addr.s_addr = INADDR_ANY;
		bind_addr.sin_port = htons(listen_port);
		r = bind(m_socket, (sockaddr*)&bind_addr, sizeof(bind_addr));
		if (r < 0)
		{
			fprintf(stderr, "failed to bind socket to port %d (%d): %s\n"
				, listen_port, errno, strerror(errno));
			exit(1);
		}

#ifdef _WIN32
		unsigned long one = 1;
		r = ioctlsocket(m_socket, FIONBIO, &one);
		if (r < 0)
		{
			fprintf(stderr, "failed to set non-blocking mode (%d): %s\n"
				, errno, strerror(errno));
			exit(1);
		}
#else
		int flags = fcntl(m_socket, F_GETFL, 0);
		if (flags < 0)
		{
			fprintf(stderr, "failed to get file flags (%d): %s\n"
				, errno, strerror(errno));
			exit(1);
		}
		flags |= O_NONBLOCK;
		r = fcntl(m_socket, F_SETFL, flags);
		if (r < 0)
		{
			fprintf(stderr, "failed to set file flags (%d): %s\n"
				, errno, strerror(errno));
			exit(1);
		}
#endif
	}
}

packet_socket::~packet_socket()
{
	close();
}

void packet_socket::close()
{
	if (m_socket != -1)
#ifdef _WIN32
		::closesocket(m_socket);
#else
		::close(m_socket);
#endif
	m_socket = -1;
}

packet_socket::packet_socket(packet_socket&& s)
	: m_socket(s.m_socket)
	, m_receive(s.m_receive)
{
	s.m_socket = -1;
}

bool packet_socket::send(packet_buffer& packets)
{
	// This is NOP. packets are sent directly when added to packet_buffer.
	return true;
}

// send a packet and retry on EINTR
bool packet_buffer::append(iovec const* v, int num, sockaddr_in const* to)
{
#ifdef _WIN32
	// windows doesn't support the msghdr
	char buf[1500];
	char* ptr = buf;
	int len = 0;
	if (num == 1)
	{
		ptr = (char*)v->iov_base;
		len = v->iov_len;
	}
	else
	{
		for (int i = 0; i < num; ++i)
		{
			memcpy(ptr, v[i].iov_base, v[i].iov_len);
			ptr += v[i].iov_len;
			len += v[i].iov_len;
		}
		ptr = buf;
	}
#else
	msghdr msg;
	msg.msg_name = (void*)to;
	msg.msg_namelen = sizeof(sockaddr_in);
	msg.msg_iov = (iovec*)v;
	msg.msg_iovlen = num;
	msg.msg_control = 0;
	msg.msg_controllen = 0;
	msg.msg_flags = MSG_NOSIGNAL;
#endif
	// loop just to deal with the potential EINTR
	do
	{
#ifdef _WIN32
		int r = sendto(m_socket, ptr, len, 0
			, (sockaddr const*)to, sizeof(sockaddr_in));
#else
		int r = sendmsg(m_socket, &msg, 0);
#endif
		if (r < 0)
		{
			if (errno == EINTR) continue;
			fprintf(stderr, "sendmsg failed (%d): %s\n", errno, strerror(errno));
			return false;
		}
		bytes_out.fetch_add(r, std::memory_order_relaxed);
	} while (false);
	return true;
}

void packet_socket::local_endpoint(sockaddr_in* addr)
{
	socklen_t len;
	getpeername(m_socket, (sockaddr*)addr, &len);
}

