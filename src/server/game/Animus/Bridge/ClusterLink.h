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

#ifndef ANIMUS_FORGE_CLUSTER_LINK_H
#define ANIMUS_FORGE_CLUSTER_LINK_H

#include "Define.h"
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace AnimusForge
{
    /// The control channel of a cluster (AnimusForge.Cluster.Role): the host tells its workers which scenario to run
    /// and when to stop, and learns where each worker's sim listens for the learner. The data -- STEP and ACT -- goes
    /// straight between the host's learner and each worker's sim (LockstepServer over TCP); this carries only orders.
    ///
    /// Plain lines over TCP, non-blocking, polled from the world thread:
    ///   worker -> host   REGISTER <data port> <address the host should reach it at, or "-">
    ///   host -> worker   START <scenario> <resume 0|1> <fast 0|1>
    ///   host -> worker   STOP
    /// A worker that loses the host reconnects every few seconds and registers again; a host that loses a worker
    /// drops it, and it joins the next scenario the host starts after it is back.
    class ClusterLink
    {
    public:
        ClusterLink() = default;
        ~ClusterLink();

        ClusterLink(ClusterLink const&) = delete;
        ClusterLink& operator=(ClusterLink const&) = delete;

        /// Host: accept workers on `port`.
        bool Listen(uint16 port);
        /// Worker: reach the host at `host` ("address:port") and register `dataPort` (and `advertise`, the address
        /// the host should use for it; empty = whatever address the connection comes from).
        void Join(std::string const& host, uint16 dataPort, std::string const& advertise);

        /// Accept, register, read and reconnect: cheap, called every tick and while the world thread waits.
        void Poll();

        /// Host: the registered workers' sims, as the learner reaches them ("tcp://address:port").
        [[nodiscard]] std::vector<std::string> WorkerSims() const;
        /// Host: an order to every worker.
        void Broadcast(std::string const& line);

        /// Worker: the next order from the host, if one has come.
        std::optional<std::string> NextOrder();

    private:
        struct Peer
        {
            int Fd = -1;
            std::string In;
            std::string Sim;        // host: "tcp://address:port" once registered
            std::string Address;    // host: where the connection came from
        };

        void Close(Peer& peer);
        bool Send(Peer& peer, std::string const& line);
        bool ReadLines(Peer& peer, std::vector<std::string>& lines);
        void ConnectToHost();

        int _listener = -1;
        std::vector<Peer> _workers;

        Peer _host;
        std::string _hostAddress;
        uint16 _dataPort = 0;
        std::string _advertise;
        std::vector<std::string> _orders;
        std::chrono::steady_clock::time_point _nextAttempt{};
    };
}

#endif
