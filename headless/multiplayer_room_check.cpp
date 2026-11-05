// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real upstream room server and the derived client over loopback. Guest sockets
// (ProxySocket and the BSD service) are upstream's own since the Lichii core update.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "common/logging.h"
#include "multiplayer_logging.h"
#include "common/assert.h"
#include "common/settings.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "network/network.h"
#include "multiplayer_proxy.h"
#include "multiplayer.h"
#include "multiplayer_session.h"
#include "enet/enet.h"
#include "network/packet.h"
#include "core/internal_network/network_interface.h"
#include "core/hle/service/ldn/lan_discovery.h"

static std::atomic<bool> reject_host_lookup{};
extern "C" int __real_enet_address_set_host(ENetAddress*, const char*);
extern "C" int __wrap_enet_address_set_host(ENetAddress* address, const char* name) {
    if (reject_host_lookup.load()) return -1;
    return __real_enet_address_set_host(address, name);
}

template <typename Predicate>
void Wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    assert(predicate());
}

void CheckController(u16 port) {
    using namespace Eden::Multiplayer;
    RoomClient client;
    Network::Room room;
    assert(room.Create("Controller test", "", "127.0.0.1", port, "secret", 2,
                      "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
    Connection connection{"localhost", port, "PlayerOne", "wrong"};
    auto invalid = connection;
    invalid.nickname = "x";
    assert(!client.Connect(invalid));
    assert(client.Connect(connection));
    assert(!client.BeginGame());
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "Incorrect password. Enter the room password again; it is case-sensitive.");
    connection.password = "secret";
    assert(client.Connect(connection));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    const auto snapshot = client.GetSnapshot();
    assert(snapshot.room == "Controller test" && snapshot.members == std::vector<std::string>{"PlayerOne"});
    auto member = Network::GetRoomMember().lock();
    assert(member);
    Wait([&] { return member->GetPingMilliseconds() >= 0; });
    assert(member->GetPingMilliseconds() <= 9999);
    const auto previous_interface = Settings::values.network_interface.GetValue();
    Settings::values.network_interface.SetValue("missing-host-adapter-for-room-test");
    const auto room_interface = Network::GetSelectedNetworkInterface();
    assert(room_interface && room_interface->kind == Network::HostAdapterKind::Wifi);
    assert(Network::TranslateIPv4(room_interface->ip_address) == member->GetFakeIpAddress());
    assert((Network::TranslateIPv4(room_interface->subnet_mask) == Network::IPv4Address{255, 255, 0, 0}));
    {
        Service::LDN::LANDiscovery ldn;
        assert(ldn.Initialize([] {}).IsSuccess());
        assert(ldn.OpenAccessPoint().IsSuccess());
        Service::LDN::NetworkConfig config{};
        config.node_count_max = 8;
        assert(ldn.CreateNetwork({}, {}, config).IsSuccess());
        Service::LDN::NetworkInfo info{};
        assert(ldn.GetNetworkInfo(info).IsSuccess());
        auto ip = member->GetFakeIpAddress();
        std::reverse(ip.begin(), ip.end());
        assert(info.ldn.nodes[0].ipv4_address == ip);
        assert(info.ldn.node_count == 1);
        const auto receive = member->BindOnLdnPacketReceived(
            [&](const auto& packet) { ldn.ReceivePacket(packet); });
        Network::RoomMember peer;
        std::atomic<bool> discovered{}, connected{}, destroyed{};
        const auto peer_receive = peer.BindOnLdnPacketReceived([&](const auto& packet) {
            if (packet.type == Network::LDNPacketType::DestroyNetwork) destroyed = true;
            if (packet.type != Network::LDNPacketType::ScanResp &&
                packet.type != Network::LDNPacketType::SyncNetwork) return;
            assert(packet.data.size() == sizeof(Service::LDN::NetworkInfo));
            Service::LDN::NetworkInfo received{};
            std::memcpy(&received, packet.data.data(), sizeof(received));
            assert(received.ldn.nodes[0].ipv4_address == ip);
            if (packet.type == Network::LDNPacketType::ScanResp) discovered = true;
            if (packet.type == Network::LDNPacketType::SyncNetwork && received.ldn.node_count == 2)
                connected = true;
        });
        peer.Join("LdnPeer", "127.0.0.1", port, 0, Network::NoPreferredIP, "secret");
        Wait([&] { return peer.GetState() == Network::RoomMember::State::Joined; });
        peer.SendLdnPacket({Network::LDNPacketType::Scan, peer.GetFakeIpAddress(), {}, true, {}});
        Wait([&] { return discovered.load(); });
        Service::LDN::NodeInfo node{};
        node.ipv4_address = peer.GetFakeIpAddress();
        std::reverse(node.ipv4_address.begin(), node.ipv4_address.end());
        node.mac_address.raw = {2, 0, 1, 2, 3, 4};
        Network::LDNPacket connect{Network::LDNPacketType::Connect, peer.GetFakeIpAddress(),
                                   member->GetFakeIpAddress(), false, {}};
        connect.data.resize(sizeof(node));
        std::memcpy(connect.data.data(), &node, sizeof(node));
        peer.SendLdnPacket(connect);
        Wait([&] { return connected.load(); });
        assert(ldn.Finalize().IsSuccess());
        Wait([&] { return destroyed.load(); });
        member->Unbind(receive);
        peer.Unbind(peer_receive);
        peer.Leave();
        Wait([&] { return client.GetSnapshot().members == std::vector<std::string>{"PlayerOne"}; });
    }
    member->SendGameInfo({"Example game", 1, "1.2.3"});
    Wait([&] { return client.GetSnapshot().members ==
        std::vector<std::string>{"PlayerOne - Example game (1.2.3)"}; });
    member->SendGameInfo({});
    Wait([&] { return client.GetSnapshot().members == std::vector<std::string>{"PlayerOne"}; });
    assert(!client.Connect(connection));
    assert(client.BeginGame());
    assert(guest_socket_mode == 1 && !client.GameConnectionLost());
    client.Leave();
    assert(client.GetSnapshot().phase == Phase::Connected);
    client.EndGame();
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    assert(!Network::GetSelectedNetworkInterface());
    assert(member->GetPingMilliseconds() == -1);
    Settings::values.network_interface.SetValue(previous_interface);
    assert(client.GetSnapshot().members.empty());
    assert(client.BeginGame());
    assert(guest_socket_mode == 0 && !client.Connect(connection));
    client.EndGame();
    assert(client.Connect(connection));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    assert(client.BeginGame());
    room.Destroy();
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "Connection lost. Check your internet connection, then rejoin the room before restarting Local Wireless in the game.");
    assert(client.GameConnectionLost() && guest_socket_mode == 1);
    assert(member->GetPingMilliseconds() == -1);
    assert(!client.Connect(connection));
    client.EndGame();
    connection.host = "127.0.0.1";
    assert(client.Connect(connection));
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto start = std::chrono::steady_clock::now();
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
    assert(client.Connect(connection));
    // Destruction must also cancel a join without waiting for its five-second timeout.
}

void CheckRejectedRoom(u16 port) {
    using namespace Eden::Multiplayer;
    RoomClient client;
    Network::Room room;
    assert(room.Create("Rejection test", "", "127.0.0.1", port, "", 2,
                      "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
    Network::RoomMember occupied, second;
    std::atomic<std::size_t> members{};
    const auto info = occupied.BindOnRoomInformationChanged([&](const auto&) {
        members = occupied.GetMemberInformation().size();
    });
    occupied.Join("Occupied", "127.0.0.1", port, 0, Network::NoPreferredIP, "");
    Wait([&] { return occupied.GetState() == Network::RoomMember::State::Joined; });
    assert(client.Connect({"127.0.0.1", port, "Occupied", ""}));
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "Nickname already in use. Enter a different nickname and try again.");
    assert(client.Connect({"127.0.0.1", port, "Available", ""}));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle && members == 1; });
    second.Join("SecondPlayer", "127.0.0.1", port, 0, Network::NoPreferredIP, "");
    Wait([&] { return members == 2; });
    assert(client.Connect({"127.0.0.1", port, "Available", ""}));
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == "Room full. Wait for a free place or choose another room.");
    second.Leave();
    Wait([&] { return members == 1; });
    assert(client.Connect({"127.0.0.1", port, "Available", ""}));
    Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    occupied.Leave();
    occupied.Unbind(info);
    room.Destroy();
}

enum class TestRoom { OutOfOrder, Stalled, VersionMismatch, JoinTimeout };

void CheckTestRoom(u16 port, TestRoom mode) {
    using namespace Eden::Multiplayer;
    const bool stall = mode == TestRoom::Stalled;
    RoomClient client;
    ENetAddress address{};
    assert(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    address.port = port;
    auto* server = enet_host_create(&address, 1, Network::NumChannels, 0, 0);
    assert(server);
    std::atomic<bool> paused{};
    std::jthread replies([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            ENetEvent event{};
            if (enet_host_service(server, &event, 10) <= 0) continue;
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                const bool join = event.packet->dataLength && event.packet->data[0] == Network::IdJoinRequest;
                enet_packet_destroy(event.packet);
                if (!join) continue;
                // Keep ENet responsive, but never accept the player's room join.
                if (mode == TestRoom::JoinTimeout) continue;
                if (mode == TestRoom::VersionMismatch) {
                    Network::Packet reply;
                    reply.Write(u8{Network::IdVersionMismatch}).Write(u32{Network::network_version + 1});
                    auto* packet = enet_packet_create(reply.GetData(), reply.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
                    assert(enet_peer_send(event.peer, 0, packet) == 0);
                    enet_host_flush(server);
                    continue;
                }
                if (stall) {
                    Network::Packet info;
                    info.Write(u8{Network::IdRoomInformation}).Write(std::string{"Test room"})
                        .Write(std::string{}).Write(u32{2}).Write(port)
                        .Write(std::string{}).Write(std::string{}).Write(u32{1})
                        .Write(std::string{"PlayerOne"}).Write(Network::IPv4Address{192, 168, 0, 1})
                        .Write(std::string{}).Write(u64{0}).Write(std::string{})
                        .Write(std::string{}).Write(std::string{}).Write(std::string{});
                    auto* packet = enet_packet_create(info.GetData(), info.GetDataSize(), ENET_PACKET_FLAG_RELIABLE);
                    assert(enet_peer_send(event.peer, 0, packet) == 0);
                }
                // Without room information, the otherwise valid success must be rejected.
                const u8 reply[]{Network::IdJoinSuccess, 192, 168, 0, 1};
                auto* packet = enet_packet_create(reply, sizeof(reply), ENET_PACKET_FLAG_RELIABLE);
                assert(enet_peer_send(event.peer, 0, packet) == 0);
                enet_host_flush(server);
                if (stall) paused = true;
            }
        }
    });
    assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
    if (stall) {
        Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
        auto member = Network::GetRoomMember().lock();
        Network::ProxyPacket packet{};
        packet.protocol = Network::Protocol::UDP;
        packet.data.resize(Eden::Multiplayer::MaxProxyPayloadBytes / 2, 7);
        // The peer has stopped processing ENet packets, including acknowledgements.
        for (unsigned i = 0; i < 20 && client.GetSnapshot().phase == Phase::Connected; ++i) {
            member->SendProxyPacket(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    Wait([&] { return client.GetSnapshot().phase == Phase::Failed; });
    assert(client.GetSnapshot().error == (stall ? "Connection lost. Check your internet connection, then rejoin the room before restarting Local Wireless in the game." :
        mode == TestRoom::JoinTimeout ? "Room join timed out. The server connection opened, but room acceptance did not finish. Retry; if it repeats, ask the host to check the server log." :
        mode == TestRoom::VersionMismatch ? "Incompatible room version. Update EglantinePS5 or choose a compatible room; the host may also need to update the server." : "Connection failed. Try again. If it repeats, enable detailed logging in Settings and share the log with support."));
    paused = false;
    client.Leave();
    Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
    replies.request_stop();
    replies.join();
    enet_host_destroy(server);
    if (mode == TestRoom::VersionMismatch) {
        Network::Room compatible;
        assert(compatible.Create("Compatible retry", "", "127.0.0.1", port, "", 2,
                                 "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
        assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
        Wait([&] { return client.GetSnapshot().phase == Phase::Connected; });
        client.Leave();
        Wait([&] { return client.GetSnapshot().phase == Phase::Idle; });
        compatible.Destroy();
    }
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    Eden::Multiplayer::SetDetailedLogging(argc == 3);
    const auto port = static_cast<u16>(std::strtoul(argv[1], nullptr, 10));
    Common::Log::Initialize();
    Network::NetworkInstance socket_network;
    assert(Network::Init());
    {
        Network::Room room;
        assert(room.Create("Offline test", "", "127.0.0.1", port, "secret", 2,
                           "", {}, std::make_unique<Network::VerifyUser::NullBackend>()));
        Network::RoomMember first, second;
        // Reproduce the console resolver failure even for a numeric address.
        reject_host_lookup = true;
        std::atomic<bool> rejected{}, scanned{}, proxied{}, lost{};
        const auto errors = first.BindOnError([&](const auto& error) {
            if (error == Network::RoomMember::Error::WrongPassword) rejected = true;
            if (error == Network::RoomMember::Error::LostConnection) lost = true;
        });
        first.Join("PlayerOne", "127.0.0.1", port, 0, Network::NoPreferredIP, "wrong");
        Wait([&] { return rejected.load(); });
        first.Leave();
        first.Join("PlayerOne", "127.0.0.1", port, 0, Network::NoPreferredIP, "secret");
        second.Join("PlayerTwo", "127.0.0.1", port, 0, Network::NoPreferredIP, "secret");
        const auto joined = [](const auto& member) {
            return member.GetState() == Network::RoomMember::State::Joined;
        };
        Wait([&] { return joined(first) && joined(second); });
        reject_host_lookup = false;
        assert(first.GetFakeIpAddress() != second.GetFakeIpAddress());
        const auto ldn_callback = second.BindOnLdnPacketReceived([&](const auto& packet) {
            scanned = packet.type == Network::LDNPacketType::Scan && packet.broadcast;
        });
        const auto proxy_callback = second.BindOnProxyPacketReceived([&](const auto& packet) {
            proxied = packet.data == std::vector<u8>{1, 2, 3} &&
                      packet.remote_endpoint.portno == 1234;
        });
        first.SendLdnPacket({Network::LDNPacketType::Scan, first.GetFakeIpAddress(), {}, true, {}});
        Network::ProxyPacket packet{};
        packet.local_endpoint = {Network::Domain::INET, first.GetFakeIpAddress(), 1234};
        packet.remote_endpoint = {Network::Domain::INET, second.GetFakeIpAddress(), 1234};
        packet.protocol = Network::Protocol::UDP;
        packet.data = {1, 2, 3};
        first.SendProxyPacket(packet);
        Wait([&] { return scanned.load() && proxied.load(); });
        second.Leave();
        second.Unbind(ldn_callback);
        second.Unbind(proxy_callback);
        room.Destroy();
        Wait([&] { return lost.load(); });
        first.Leave();
        first.Unbind(errors);
    }
    Network::Shutdown();
    CheckController(port);
    CheckRejectedRoom(port);
    CheckTestRoom(port, TestRoom::OutOfOrder);
    CheckTestRoom(port, TestRoom::Stalled);
    CheckTestRoom(port, TestRoom::VersionMismatch);
    CheckTestRoom(port, TestRoom::JoinTimeout);
    const auto start = std::chrono::steady_clock::now();
    {
        Eden::Multiplayer::RoomClient client;
        assert(client.Connect({"127.0.0.1", port, "PlayerOne", ""}));
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
}
