/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "LockstepServer.h"
#include "Log.h"
#include "World.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    /// How long a blocking wait sleeps before re-checking World::IsStopped().
    constexpr int POLL_INTERVAL_MS = 200;
}

AnimusForge::LockstepServer::~LockstepServer()
{
    Shutdown();
}

bool AnimusForge::LockstepServer::Listen(std::string const& path)
{
    if (_listener >= 0 && _path == path)
        return true;

    Shutdown();

    // "tcp://address:port": a cluster worker's sim, which the host's learner reaches over the network.
    if (path.rfind("tcp://", 0) == 0)
    {
        std::string const rest = path.substr(6);
        std::size_t const colon = rest.rfind(':');
        int const port = colon == std::string::npos ? 0 : std::atoi(rest.c_str() + colon + 1);
        if (port <= 0 || port >= 65536)
        {
            LOG_ERROR("module.animus", "Socket '{}' is not tcp://address:port", path);
            return false;
        }

        _listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int const on = 1;
        if (_listener >= 0)
            ::setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in inet{};
        inet.sin_family = AF_INET;
        inet.sin_port = htons(uint16(port));
        std::string const host = rest.substr(0, colon);
        inet.sin_addr.s_addr = host.empty() || host == "0.0.0.0" ? htonl(INADDR_ANY) : ::inet_addr(host.c_str());
        if (_listener < 0 || ::bind(_listener, reinterpret_cast<sockaddr*>(&inet), sizeof(inet)) < 0
            || ::listen(_listener, 1) < 0)
        {
            LOG_ERROR("module.animus", "Cannot listen on '{}': {}", path, std::strerror(errno));
            if (_listener >= 0)
                ::close(_listener);
            _listener = -1;
            return false;
        }

        _path = path;
        LOG_INFO("module.animus", "Listening for the learner on {}", path);
        return true;
    }

    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path))
    {
        LOG_ERROR("module.animus", "Socket path '{}' is too long", path);
        return false;
    }

    _listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (_listener < 0)
    {
        LOG_ERROR("module.animus", "socket() failed: {}", std::strerror(errno));
        return false;
    }

    // A stale socket file from a crashed run would make bind() fail, so it is removed -- unless a sim is listening on
    // it: two sims on one machine (a cluster worker beside its host) must not take each other's learner.
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int const probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool const live = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (probe >= 0)
        ::close(probe);
    if (live)
    {
        LOG_ERROR("module.animus", "Another sim is listening on '{}': give this one its own AnimusForge.Socket", path);
        ::close(_listener);
        _listener = -1;
        return false;
    }
    ::unlink(path.c_str());

    if (::bind(_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(_listener, 1) < 0)
    {
        LOG_ERROR("module.animus", "Cannot listen on '{}': {}", path, std::strerror(errno));
        ::close(_listener);
        _listener = -1;
        return false;
    }

    _path = path;
    LOG_DEBUG("module.animus", "Listening for the learner on {}", path);
    return true;
}

void AnimusForge::LockstepServer::Shutdown()
{
    DropClient();

    if (_listener >= 0)
    {
        ::close(_listener);
        _listener = -1;
        if (_path.rfind("tcp://", 0) != 0)
            ::unlink(_path.c_str());
    }
}

bool AnimusForge::LockstepServer::AcceptClient(std::function<bool()> const& onIdle)
{
    DropClient();

    LOG_DEBUG("module.animus", "Waiting for the learner to connect...");

    while (WaitReadable(_listener, onIdle))
    {
        int const fd = ::accept4(_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            continue;

        // Over TCP a STEP must not wait for Nagle to fill a segment: a decision is a request and its answer.
        if (_path.rfind("tcp://", 0) == 0)
        {
            int const on = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        }

        _client = fd;

        MsgType type;
        HelloMsg hello{};
        if (!Receive(type, &hello, sizeof(hello)) || type != MsgType::Hello)
        {
            LOG_WARN("module.animus", "Learner connection did not start with HELLO, dropping it");
            DropClient();
            continue;
        }

        if (hello.Version != PROTOCOL_VERSION)
        {
            LOG_ERROR("module.animus", "Learner speaks protocol {}, sim speaks {}", hello.Version, PROTOCOL_VERSION);
            DropClient();
            continue;
        }

        LOG_DEBUG("module.animus", "Learner connected");
        return true;
    }

    return false;
}

void AnimusForge::LockstepServer::DropClient()
{
    if (_client >= 0)
    {
        ::close(_client);
        _client = -1;
    }
}

bool AnimusForge::LockstepServer::Send(MsgType type, std::vector<Chunk> const& chunks)
{
    if (_client < 0)
        return false;

    std::size_t payload = 0;
    for (Chunk const& chunk : chunks)
        payload += chunk.Size;

    MsgHeader header{ static_cast<uint32>(type), static_cast<uint32>(payload) };

    std::vector<iovec> iov;
    iov.reserve(chunks.size() + 1);
    iov.push_back({ &header, sizeof(header) });
    for (Chunk const& chunk : chunks)
        if (chunk.Size)
            iov.push_back({ const_cast<void*>(chunk.Data), chunk.Size });

    // writev may stop part-way through a large message; advance the vector and keep going.
    std::size_t first = 0;
    while (first < iov.size())
    {
        ssize_t written = ::writev(_client, iov.data() + first, static_cast<int>(iov.size() - first));
        if (written < 0)
        {
            if (errno == EINTR)
                continue;

            LOG_WARN("module.animus", "Send to learner failed: {}", std::strerror(errno));
            DropClient();
            return false;
        }

        while (written > 0)
        {
            iovec& vec = iov[first];
            if (static_cast<std::size_t>(written) >= vec.iov_len)
            {
                written -= static_cast<ssize_t>(vec.iov_len);
                ++first;
            }
            else
            {
                vec.iov_base = static_cast<char*>(vec.iov_base) + written;
                vec.iov_len -= static_cast<std::size_t>(written);
                written = 0;
            }
        }
    }

    return true;
}

bool AnimusForge::LockstepServer::Receive(MsgType& type, void* dst, std::size_t size)
{
    MsgHeader header{};
    if (!ReadExact(&header, sizeof(header)))
        return false;

    type = static_cast<MsgType>(header.Type);

    // CLOSE carries no payload; the caller sees the type and the connection is already gone.
    if (type == MsgType::Close)
    {
        LOG_DEBUG("module.animus", "Learner closed the session");
        DropClient();
        return true;
    }

    if (header.Length != size)
    {
        LOG_ERROR("module.animus", "Learner sent message type {} with {} bytes, expected {}", header.Type,
            header.Length, size);
        DropClient();
        return false;
    }

    return ReadExact(dst, size);
}

bool AnimusForge::LockstepServer::ReceiveAny(MsgType& type, std::vector<char>& payload, std::size_t maxSize,
    std::function<bool()> const& onIdle)
{
    MsgHeader header{};
    if (!ReadExact(&header, sizeof(header), onIdle))
        return false;

    type = static_cast<MsgType>(header.Type);
    payload.clear();

    if (type == MsgType::Close)
    {
        LOG_DEBUG("module.animus", "Learner closed the session");
        DropClient();
        return true;
    }

    if (header.Length > maxSize)
    {
        LOG_ERROR("module.animus", "Learner sent message type {} with {} bytes, at most {} expected", header.Type,
            header.Length, maxSize);
        DropClient();
        return false;
    }

    payload.resize(header.Length);
    return ReadExact(payload.data(), payload.size(), onIdle);
}

bool AnimusForge::LockstepServer::WaitReadable(int fd, std::function<bool()> const& onIdle)
{
    pollfd pfd{ fd, POLLIN, 0 };

    while (!World::IsStopped())
    {
        int const ready = ::poll(&pfd, 1, POLL_INTERVAL_MS);
        if (ready > 0)
            return true;

        if (ready == 0 && onIdle && !onIdle())
            return false;

        if (ready < 0 && errno != EINTR)
        {
            LOG_ERROR("module.animus", "poll() failed: {}", std::strerror(errno));
            return false;
        }
    }

    return false;
}

bool AnimusForge::LockstepServer::ReadExact(void* dst, std::size_t size, std::function<bool()> const& onIdle)
{
    char* out = static_cast<char*>(dst);

    while (size)
    {
        if (_client < 0 || !WaitReadable(_client, onIdle))
            return false;

        ssize_t const got = ::recv(_client, out, size, 0);
        if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN))
        {
            LOG_WARN("module.animus", "Learner disconnected");
            DropClient();
            return false;
        }

        if (got > 0)
        {
            out += got;
            size -= static_cast<std::size_t>(got);
        }
    }

    return true;
}
