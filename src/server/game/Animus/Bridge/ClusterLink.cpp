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

#include "ClusterLink.h"
#include "Log.h"
#include "StringFormat.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace
{
    constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(3);
    /// A worker the host refused tries again after this: long enough not to flood the host's log, short enough that
    /// fixing the worker's copy and restarting nothing else brings it in.
    constexpr auto REFUSED_INTERVAL = std::chrono::seconds(60);
    /// How long a host waits for a registered worker's fingerprint (an older build sends none).
    constexpr auto FINGERPRINT_WAIT = std::chrono::seconds(15);

    /// "k=v k=v" -> the keys whose values differ between the two, or that only one has.
    std::string Differences(std::string const& mine, std::string const& theirs)
    {
        auto const parse = [](std::string const& text)
        {
            std::vector<std::pair<std::string, std::string>> fields;
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find(' ', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string const token = text.substr(start, end - start);
                std::size_t const equals = token.find('=');
                if (!token.empty())
                    fields.emplace_back(token.substr(0, equals),
                        equals == std::string::npos ? "" : token.substr(equals + 1));
                start = end + 1;
            }
            return fields;
        };
        auto const a = parse(mine);
        auto const b = parse(theirs);
        std::string differ;
        auto const find = [](auto const& fields, std::string const& key) -> std::string const*
        {
            for (auto const& [k, v] : fields)
                if (k == key)
                    return &v;
            return nullptr;
        };
        for (auto const& [key, value] : a)
        {
            std::string const* other = find(b, key);
            if (!other || *other != value)
                differ += (differ.empty() ? "" : " ") + key + "(host " + value + ", worker " + (other ? *other : "none")
                    + ")";
        }
        for (auto const& [key, value] : b)
            if (!find(a, key))
                differ += (differ.empty() ? "" : " ") + key + "(host none, worker " + value + ")";
        return differ;
    }

    void NonBlocking(int fd)
    {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int const on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    /// "address:port" -> (address, port); the port is after the last colon.
    bool SplitAddress(std::string const& text, std::string& address, std::string& port)
    {
        std::size_t const colon = text.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
            return false;
        address = text.substr(0, colon);
        port = text.substr(colon + 1);
        return true;
    }
}

AnimusForge::ClusterLink::~ClusterLink()
{
    for (Peer& worker : _workers)
        Close(worker);
    Close(_host);
    if (_listener >= 0)
        ::close(_listener);
}

bool AnimusForge::ClusterLink::Listen(uint16 port)
{
    _listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (_listener < 0)
        return false;

    int const on = 1;
    ::setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(_listener, 16) < 0)
    {
        LOG_ERROR("module.animus", "Cluster: cannot listen for workers on port {}: {}", port, std::strerror(errno));
        ::close(_listener);
        _listener = -1;
        return false;
    }

    NonBlocking(_listener);
    LOG_INFO("module.animus", "Cluster: host, listening for workers on port {}", port);
    return true;
}

void AnimusForge::ClusterLink::Join(std::string const& host, uint16 dataPort, std::string const& advertise)
{
    _hostAddress = host;
    _dataPort = dataPort;
    _advertise = advertise;
    LOG_INFO("module.animus", "Cluster: worker of {}, sim listening for its learner on port {}", host, dataPort);
}

void AnimusForge::ClusterLink::Close(Peer& peer)
{
    if (peer.Fd >= 0)
        ::close(peer.Fd);
    peer = Peer();
}

bool AnimusForge::ClusterLink::Send(Peer& peer, std::string const& line)
{
    std::string const out = line + "\n";
    // Orders are a few bytes; a socket that cannot take them at once is a peer that is gone.
    return peer.Fd >= 0 && ::send(peer.Fd, out.data(), out.size(), MSG_NOSIGNAL) == ssize_t(out.size());
}

bool AnimusForge::ClusterLink::ReadLines(Peer& peer, std::vector<std::string>& lines)
{
    char buffer[512];
    bool open = true;
    for (;;)
    {
        ssize_t const got = ::recv(peer.Fd, buffer, sizeof(buffer), 0);
        if (got > 0)
        {
            peer.In.append(buffer, std::size_t(got));
            continue;
        }
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            open = false;
        break;
    }

    // Whole lines, even from a peer that has closed: what it said last (a host's REFUSED) is why it closed.
    for (std::size_t end; (end = peer.In.find('\n')) != std::string::npos;)
    {
        lines.push_back(peer.In.substr(0, end));
        peer.In.erase(0, end + 1);
    }
    return open;
}

void AnimusForge::ClusterLink::ConnectToHost()
{
    auto const now = std::chrono::steady_clock::now();
    if (now < _nextAttempt)
        return;
    _nextAttempt = now + RECONNECT_INTERVAL;

    std::string address, port;
    if (!SplitAddress(_hostAddress, address, port))
    {
        LOG_ERROR("module.animus", "Cluster: AnimusForge.Cluster.Host '{}' is not address:port", _hostAddress);
        _nextAttempt = now + std::chrono::hours(24);
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(address.c_str(), port.c_str(), &hints, &found) != 0 || !found)
        return;

    int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // A blocking connect with the host on the same network is milliseconds; one that fails is retried later.
    bool const connected = fd >= 0 && ::connect(fd, found->ai_addr, found->ai_addrlen) == 0;
    ::freeaddrinfo(found);
    if (!connected)
    {
        if (fd >= 0)
            ::close(fd);
        return;
    }

    NonBlocking(fd);
    _host.Fd = fd;
    if (!Send(_host, Acore::StringFormat("REGISTER {} {}", _dataPort, _advertise.empty() ? "-" : _advertise))
        || !Send(_host, "FINGERPRINT " + _fingerprint)
        || !Send(_host, Acore::StringFormat("CAPS learner={}", _learnerCapable ? 1 : 0)))
    {
        Close(_host);
        return;
    }
    LOG_INFO("module.animus", "Cluster: joined the host at {}", _hostAddress);
}

void AnimusForge::ClusterLink::Poll()
{
    if (_listener >= 0)
    {
        for (;;)
        {
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            int const fd = ::accept4(_listener, reinterpret_cast<sockaddr*>(&from), &length, SOCK_CLOEXEC);
            if (fd < 0)
                break;

            NonBlocking(fd);
            char text[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
            Peer worker;
            worker.Fd = fd;
            worker.Address = text;
            // This machine's end of it: the address the worker reached the host at, which is where the worker's
            // learner will reach the host's.
            sockaddr_in local{};
            socklen_t localLength = sizeof(local);
            char localText[INET_ADDRSTRLEN] = {};
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) == 0
                && ::inet_ntop(AF_INET, &local.sin_addr, localText, sizeof(localText)))
                worker.LocalAddress = localText;
            _workers.push_back(std::move(worker));
        }

        for (auto it = _workers.begin(); it != _workers.end();)
        {
            std::vector<std::string> lines;
            if (!ReadLines(*it, lines))
            {
                if (!it->Sim.empty())
                    LOG_WARN("module.animus", "Cluster: lost the worker at {}", it->Sim);
                Close(*it);
                it = _workers.erase(it);
                continue;
            }
            if (!it->Pending.empty() && std::chrono::steady_clock::now() - it->PendingAt > FINGERPRINT_WAIT)
            {
                LOG_ERROR("module.animus", "Cluster: refused the worker at {}: it sent no fingerprint (a build from "
                    "before fingerprints). Copy the project to it again and rebuild.", it->Pending);
                Send(*it, "REFUSED no fingerprint");
                it->Refused = true;
            }

            for (std::string const& line : lines)
            {
                char advertise[256] = {};
                unsigned port = 0;
                if (std::sscanf(line.c_str(), "REGISTER %u %255s", &port, advertise) == 2 && port && port < 65536)
                {
                    std::string const address = std::strcmp(advertise, "-") ? advertise : it->Address;
                    it->Pending = Acore::StringFormat("tcp://{}:{}", address, port);
                    it->PendingAt = std::chrono::steady_clock::now();
                }
                else if (line.rfind("FINGERPRINT ", 0) == 0 && !it->Pending.empty())
                {
                    std::string const theirs = line.substr(12);
                    if (theirs == _fingerprint)
                    {
                        it->Sim = std::exchange(it->Pending, {});
                        _registered.push_back(it->Sim);
                        LOG_INFO("module.animus", "Cluster: worker registered, its sim at {}", it->Sim);
                    }
                    else
                    {
                        std::string const differ = Differences(_fingerprint, theirs);
                        LOG_ERROR("module.animus", "Cluster: refused the worker at {}: it does not run what this host "
                            "runs: {}. Copy the project (and the probe data) to it again, match its AnimusForge "
                            "settings, and rebuild.", it->Pending, differ);
                        Send(*it, "REFUSED " + differ);
                        it->Pending.clear();
                        it->Refused = true;
                    }
                }
                else if (line.rfind("CAPS ", 0) == 0)
                    it->Learner = line.find("learner=1") != std::string::npos;
                else if (line.rfind("PROGRESS ", 0) == 0)
                {
                    it->Progress = line.substr(9);
                    it->ProgressAt = std::chrono::steady_clock::now();
                }
            }
            if (it->Refused)
            {
                Close(*it);
                it = _workers.erase(it);
                continue;
            }
            ++it;
        }
    }

    if (!_hostAddress.empty())
    {
        if (_host.Fd < 0)
            ConnectToHost();
        if (_host.Fd >= 0)
        {
            std::vector<std::string> lines;
            bool const open = ReadLines(_host, lines);
            bool refused = false;
            for (std::string& line : lines)
            {
                if (line.rfind("REFUSED", 0) == 0)
                {
                    LOG_ERROR("module.animus", "Cluster: the host at {} refused this worker: {}. It differs from the "
                        "host in these; copy the project (and the probe data) again, match the settings, rebuild. "
                        "Trying again in a minute.", _hostAddress, line.size() > 8 ? line.substr(8) : "");
                    refused = true;
                    break;
                }
                _orders.push_back(std::move(line));
            }
            if (refused)
            {
                Close(_host);
                _nextAttempt = std::chrono::steady_clock::now() + REFUSED_INTERVAL;
            }
            else if (!open)
            {
                LOG_WARN("module.animus", "Cluster: lost the host at {}; reconnecting", _hostAddress);
                Close(_host);
                _nextAttempt = std::chrono::steady_clock::now() + RECONNECT_INTERVAL;
            }
        }
    }
}

std::vector<std::string> AnimusForge::ClusterLink::WorkerSims() const
{
    std::vector<std::string> sims;
    for (Peer const& worker : _workers)
        if (!worker.Sim.empty())
            sims.push_back(worker.Sim);
    return sims;
}

void AnimusForge::ClusterLink::Broadcast(std::string const& line)
{
    for (Peer& worker : _workers)
        if (!worker.Sim.empty() && !Send(worker, line))
            LOG_WARN("module.animus", "Cluster: could not reach the worker at {}", worker.Sim);
}

void AnimusForge::ClusterLink::SendTo(std::string const& sim, std::string const& line)
{
    for (Peer& worker : _workers)
        if (worker.Sim == sim && !Send(worker, line))
            LOG_WARN("module.animus", "Cluster: could not reach the worker at {}", worker.Sim);
}

std::vector<std::string> AnimusForge::ClusterLink::TakeRegistrations()
{
    return std::exchange(_registered, {});
}

std::optional<std::string> AnimusForge::ClusterLink::NextOrder()
{
    if (_orders.empty())
        return std::nullopt;
    std::string order = std::move(_orders.front());
    _orders.erase(_orders.begin());
    return order;
}

void AnimusForge::ClusterLink::Report(std::string const& fields)
{
    if (_host.Fd >= 0)
        Send(_host, "PROGRESS " + fields);
}

std::vector<AnimusForge::ClusterLink::WorkerInfo> AnimusForge::ClusterLink::RegisteredWorkers() const
{
    std::vector<WorkerInfo> workers;
    for (Peer const& peer : _workers)
        if (!peer.Sim.empty())
            workers.push_back({ peer.Sim, peer.Learner, peer.LocalAddress });
    return workers;
}

std::string AnimusForge::ClusterLink::LocalAddress() const
{
    if (_host.Fd < 0)
        return {};
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    char text[INET_ADDRSTRLEN] = {};
    if (::getsockname(_host.Fd, reinterpret_cast<sockaddr*>(&local), &length) != 0
        || !::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)))
        return {};
    return text;
}

std::string AnimusForge::ClusterLink::InterfaceOf(std::string const& address)
{
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0)
        return {};
    std::string name;
    for (ifaddrs* entry = interfaces; entry && name.empty(); entry = entry->ifa_next)
    {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        auto const* ipv4 = reinterpret_cast<sockaddr_in const*>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) && address == text)
            name = entry->ifa_name;
    }
    ::freeifaddrs(interfaces);
    return name;
}

std::vector<AnimusForge::ClusterLink::WorkerStatus> AnimusForge::ClusterLink::Workers() const
{
    auto const now = std::chrono::steady_clock::now();
    std::vector<WorkerStatus> workers;
    for (Peer const& peer : _workers)
    {
        WorkerStatus status;
        status.Sim = peer.Sim.empty() ? peer.Address : peer.Sim;
        status.Progress = peer.Progress;
        if (!peer.Progress.empty())
            status.SecondsAgo = std::chrono::duration<double>(now - peer.ProgressAt).count();
        workers.push_back(std::move(status));
    }
    return workers;
}
