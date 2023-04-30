#include "server.h"
#include <iostream>
#include "events.h"
#include "gt.hpp"
#include "proton/hash.hpp"
#include "proton/rtparam.hpp"
#include "utils.h"
#include "pathfinder.h"

void server::handle_outgoing() {
    ENetEvent evt;
    while (enet_host_service(m_proxy_server, &evt, 0) > 0) {
        m_gt_peer = evt.peer;

        switch (evt.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (!this->connect())
                    return;
            } break;
            case ENET_EVENT_TYPE_RECEIVE: {
                int packet_type = get_packet_type(evt.packet);
                switch (packet_type) {
                    case NET_MESSAGE_GENERIC_TEXT:
                        if (events::out::generictext(utils::get_text(evt.packet))) {
                            enet_packet_destroy(evt.packet);
                            return;
                        }
                        break;
                    case NET_MESSAGE_GAME_MESSAGE:
                        if (events::out::gamemessage(utils::get_text(evt.packet))) {
                            enet_packet_destroy(evt.packet);
                            return;
                        }
                        break;
                    case NET_MESSAGE_GAME_PACKET: {
                        auto packet = utils::get_struct(evt.packet);
                        if (!packet)
                            break;

                        switch (packet->m_type) {
                            case PACKET_STATE:
                                if (events::out::state(packet)) {
                                    enet_packet_destroy(evt.packet);
                                    return;
                                }
                                break;
                            case PACKET_CALL_FUNCTION:
                                if (events::out::variantlist(packet)) {
                                    enet_packet_destroy(evt.packet);
                                    return;
                                }
                                break;

                            case PACKET_PING_REPLY:
                                if (events::out::pingreply(packet)) {
                                    enet_packet_destroy(evt.packet);
                                    return;
                                }
                                break;
                            case PACKET_DISCONNECT:
                            case PACKET_APP_INTEGRITY_FAIL:
                                if (gt::in_game)
                                    return;
                                break;

                            default: PRINTS("Packet Type: %d\n", packet->m_type);
                        }
                    } break;
                    case NET_MESSAGE_TRACK: //track one should never be used, but its not bad to have it in case.
                    case NET_MESSAGE_CLIENT_LOG_RESPONSE: return;

                    default: PRINTS("Got unknown packet of type %d.\n", packet_type); break;
                }

                if (!m_server_peer || !m_real_server)
                    return;
                enet_peer_send(m_server_peer, 0, evt.packet);
                enet_host_flush(m_real_server);
            } break;
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (gt::in_game)
                    return;
                if (gt::connecting) {
                    this->disconnect(false);
                    gt::connecting = false;
                    return;
                }

            } break;
            default: PRINTS("UNHANDLED\n"); break;
        }
    }
}
void SendString(ENetPeer* peer, ENetHost* host, int32_t type, std::string str);
void SendVarList(ENetPeer* peer, ENetHost* host, variantlist_t& varlist);
void SendString(ENetPeer* peer, ENetHost* host, int32_t type, std::string str) {
    if (str.size()) {
        ENetPacket* packet = enet_packet_create(0, str.size() + 5, ENET_PACKET_FLAG_RELIABLE);
        gametextpacket_t* game_packet = reinterpret_cast<gametextpacket_t*>(packet->data);
        game_packet->m_type = type;
        memcpy(&game_packet->m_data, str.data(), str.size());
        memset(&game_packet->m_data + str.size(), 0, 1);
        enet_peer_send(peer, 0, packet);
        enet_host_flush(host);
    }
}
void SendVarList(ENetPeer* peer, ENetHost* host, variantlist_t& varlist) {
    uint32_t data_size = 0;
    std::shared_ptr<BYTE> data(reinterpret_cast<PBYTE>(varlist.serialize_to_mem(&data_size, nullptr)));
    std::shared_ptr<gameupdatepacket_t> update_packet(reinterpret_cast<gameupdatepacket_t*>(malloc(sizeof(gameupdatepacket_t) + data_size)), free);
    std::shared_ptr<gametextpacket_t> game_packet(reinterpret_cast<gametextpacket_t*>(malloc(sizeof(gametextpacket_t) + sizeof(gameupdatepacket_t) + data_size)), free);
    if (game_packet.get() && update_packet.get()) {
        ZeroMemory(update_packet.get(), sizeof(gameupdatepacket_t) + data_size);
        ZeroMemory(game_packet.get(), sizeof(gametextpacket_t) + sizeof(gameupdatepacket_t) + data_size);
        game_packet->m_type = NET_MESSAGE_GAME_PACKET;
        update_packet->m_type = PACKET_CALL_FUNCTION;
        update_packet->m_player_flags = -1;
        update_packet->m_packet_flags |= 8;
        update_packet->m_int_data = 0;
        memcpy(&update_packet->m_data, data.get(), data_size);
        update_packet->m_data_size = data_size;
        memcpy(&game_packet->m_data, update_packet.get(), sizeof(gameupdatepacket_t) + data_size);
        ENetPacket* packet = enet_packet_create(game_packet.get(), data_size + sizeof(gameupdatepacket_t), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, packet);
        enet_host_flush(host);
    }
}
std::vector<server::Item> server::inventory;

void server::handle_incoming() {
    ENetEvent event;

    while (enet_host_service(m_real_server, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: PRINTC("connection event\n"); break;
            case ENET_EVENT_TYPE_DISCONNECT: this->disconnect(true); return;
            case ENET_EVENT_TYPE_RECEIVE: {
                if (event.packet->data) {
                    int packet_type = get_packet_type(event.packet);
                    switch (packet_type) {
                    case NET_MESSAGE_GENERIC_TEXT:
                        if (events::in::generictext(utils::get_text(event.packet))) {
                            enet_packet_destroy(event.packet);
                            return;
                        }
                        break;
                    case NET_MESSAGE_GAME_MESSAGE:
                        if (events::in::gamemessage(utils::get_text(event.packet))) {
                            enet_packet_destroy(event.packet);
                            return;
                        }
                        break;
                    case NET_MESSAGE_GAME_PACKET: {
                        auto packet = utils::get_struct(event.packet);
                        if (!packet)
                            break;

                        switch (packet->m_type) {
                        case 25: {
                            gt::send_log("`9Ignoring Packet Type `c25 `9(Maybe `4Autoban `9packet)");
                        } break;
                        case PACKET_SEND_INVENTORY_STATE: {
                            server::inventory.clear();
                            auto extended_ptr = utils::get_extended(packet);
                            inventory.resize(*reinterpret_cast<short*>(extended_ptr + 9));
                            memcpy(inventory.data(), extended_ptr + 11, server::inventory.capacity() * sizeof(Item));
                            //for (Item& item : inventory) {
                            //    std::cout << "Id: "<< (int)item.id << std::endl;
                            //    std::cout << "Count: "<< (int)item.count << std::endl;
                            //    std::cout << "type: "<< (int)item.type << std::endl;
                            //}
                        }break;
                                case PACKET_CALL_FUNCTION:
                                    if (events::in::variantlist(packet)) {
                                        enet_packet_destroy(event.packet);
                                        return;
                                    }
                                    break;

                                case PACKET_SEND_MAP_DATA:
                                    
                                    if (events::in::sendmapdata(packet)) {
                                        enet_packet_destroy(event.packet);
                                        return;
                                    }
                                    break;
                               
                                /*case PACKET_ITEM_CHANGE_OBJECT:
                                    if (events::in::OnChangeObject(packet)) {
                                        enet_packet_destroy(event.packet);
                                        return;
                                    }
                                    break;*/
                                case PACKET_STATE:
                                    if (events::in::state(packet)) {
                                        enet_packet_destroy(event.packet);
                                        
                                        return;
                                    }
                                    break;
                                //no need to print this for handled packet types such as func call, because we know its 1
                                default: PRINTC("Packet Type: %d\n", packet->m_type); break;
                            }
                        } break;

                        //ignore tracking packet, and request of client crash log
                        case NET_MESSAGE_TRACK:
                            if (events::in::tracking(utils::get_text(event.packet))) {
                                enet_packet_destroy(event.packet);
                                return;
                            }
                            break;
                        case NET_MESSAGE_CLIENT_LOG_REQUEST: return;

                        default: PRINTS("Got unknown packet of type %d.\n", packet_type); break;
                    }
                }

                if (!m_gt_peer || !m_proxy_server)
                    return;
                enet_peer_send(m_gt_peer, 0, event.packet);
                enet_host_flush(m_proxy_server);

            } break;

            default: PRINTC("UNKNOWN event: %d\n", event.type); break;
        }
    }
}

void server::poll() {
    //outgoing packets going to real server that are intercepted by our proxy server
    this->handle_outgoing();

    if (!m_real_server)
        return;

    //ingoing packets coming to gt client intercepted by our proxy client
    this->handle_incoming();
}

bool server::start() {
    ENetAddress address;
    enet_address_set_host(&address, "0.0.0.0");
    address.port = m_proxyport;
    m_proxy_server = enet_host_create(&address, 1024, 10, 0, 0);
    m_proxy_server->usingNewPacket = false;

    if (!m_proxy_server) {
        PRINTS("failed to start the proxy server!\n");
        return false;
    }
    m_proxy_server->checksum = enet_crc32;
    auto code = enet_host_compress_with_range_coder(m_proxy_server);
    if (code != 0)
        PRINTS("enet host compressing failed\n");
    PRINTS("started the enet server.\n");
    return setup_client();
}

void server::quit() {
    gt::in_game = false;
    this->disconnect(true);
}

bool server::setup_client() {
    m_real_server = enet_host_create(0, 1, 2, 0, 0);
    m_real_server->usingNewPacket = true;
    if (!m_real_server) {
        PRINTC("failed to start the client\n");
        return false;
    }
    m_real_server->checksum = enet_crc32;
    auto code = enet_host_compress_with_range_coder(m_real_server);
    if (code != 0)
        PRINTC("enet host compressing failed\n");
    enet_host_flush(m_real_server);
    PRINTC("Started enet client\n");
    return true;
}

void server::redirect_server(variantlist_t& varlist) {
    m_port = varlist[1].get_uint32();
    m_token = varlist[2].get_uint32();
    m_user = varlist[3].get_uint32();
    auto str = varlist[4].get_string();
   
    auto doorid = str.substr(str.find("|"));
    m_server = str.erase(str.find("|")); //remove | and doorid from end
    PRINTC("port: %d token %d user %d server %s doorid %s\n", m_port, m_token, m_user, m_server.c_str(), doorid.c_str());
    varlist[1] = m_proxyport;
    varlist[4] = "127.0.0.1" + doorid;

    gt::connecting = true;
    send(true, varlist);
    if (m_real_server) {
        enet_host_destroy(m_real_server);
        m_real_server = nullptr;
    }
}

void server::disconnect(bool reset) {
    m_world.connected = false;
    m_world.local = {};
    m_world.players.clear();
    if (m_server_peer) {
        enet_peer_disconnect(m_server_peer, 0);
        m_server_peer = nullptr;
        enet_host_destroy(m_real_server);
        m_real_server = nullptr;
    }
    if (reset) {
        m_user = 0;
        m_token = 0;
        m_server = "213.179.209.168";
        m_port = 17198;
    }
}

bool server::connect() {
    PRINTS("Connecting to server.\n");
    ENetAddress address;
    enet_address_set_host(&address, m_server.c_str());
    address.port = m_port;
    PRINTS("port is %d and server is %s\n", m_port, m_server.c_str());
    if (!this->setup_client()) {
        PRINTS("Failed to setup client when trying to connect to server!\n");
        return false;
    }
    m_server_peer = enet_host_connect(m_real_server, &address, 2, 0);
    if (!m_server_peer) {
        PRINTS("Failed to connect to real server.\n");
        return false;
    }
    return true;
}

//bool client: true - sends to growtopia client    false - sends to gt server
void server::send(bool client, int32_t type, uint8_t* data, int32_t len) {
    auto peer = client ? m_gt_peer : m_server_peer;
    auto host = client ? m_proxy_server : m_real_server;

    if (!peer || !host)
        return;
    auto packet = enet_packet_create(0, len + 5, ENET_PACKET_FLAG_RELIABLE);
    auto game_packet = (gametextpacket_t*)packet->data;
    game_packet->m_type = type;
    if (data)
        memcpy(&game_packet->m_data, data, len);

    memset(&game_packet->m_data + len, 0, 1);
    int code = enet_peer_send(peer, 0, packet);
    if (code != 0)
        PRINTS("Error sending packet! code: %d\n", code);
    enet_host_flush(host);
}
bool server::reconnect() {
    connect();
    return true;
}
//bool client: true - sends to growtopia client    false - sends to gt server
void server::send(bool client, variantlist_t& list, int32_t netid, int32_t delay) {
    auto peer = client ? m_gt_peer : m_server_peer;
    auto host = client ? m_proxy_server : m_real_server;

    if (!peer || !host)
        return;

    uint32_t data_size = 0;
    void* data = list.serialize_to_mem(&data_size, nullptr);

    //optionally we wouldnt allocate this much but i dont want to bother looking into it
    auto update_packet = MALLOC(gameupdatepacket_t, +data_size);
    auto game_packet = MALLOC(gametextpacket_t, +sizeof(gameupdatepacket_t) + data_size);

    if (!game_packet || !update_packet)
        return;

    memset(update_packet, 0, sizeof(gameupdatepacket_t) + data_size);
    memset(game_packet, 0, sizeof(gametextpacket_t) + sizeof(gameupdatepacket_t) + data_size);
    game_packet->m_type = NET_MESSAGE_GAME_PACKET;

    update_packet->m_type = PACKET_CALL_FUNCTION;
    update_packet->m_player_flags = netid;
    update_packet->m_packet_flags |= 8;
    update_packet->m_int_data = delay;
    memcpy(&update_packet->m_data, data, data_size);
    update_packet->m_data_size = data_size;
    memcpy(&game_packet->m_data, update_packet, sizeof(gameupdatepacket_t) + data_size);
    free(update_packet);

    auto packet = enet_packet_create(game_packet, data_size + sizeof(gameupdatepacket_t), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
    enet_host_flush(host);
    free(game_packet);
}

//bool client: true - sends to growtopia client    false - sends to gt server
void server::send(bool client, std::string text, int32_t type) {
    auto peer = client ? m_gt_peer : m_server_peer;
    auto host = client ? m_proxy_server : m_real_server;

    if (!peer || !host)
        return;
    auto packet = enet_packet_create(0, text.length() + 5, ENET_PACKET_FLAG_RELIABLE);
    auto game_packet = (gametextpacket_t*)packet->data;
    game_packet->m_type = type;
    memcpy(&game_packet->m_data, text.c_str(), text.length());

    memset(&game_packet->m_data + text.length(), 0, 1);
    int code = enet_peer_send(peer, 0, packet);
    if (code != 0)
        PRINTS("Error sending packet! code: %d\n", code);
    enet_host_flush(host);
}
void server::SendPacketRaw(bool client, int a1, void* packetData, size_t packetDataSize, void* a4, int packetFlag) {
    ENetPacket* p;
    auto peer = client ? m_gt_peer : m_server_peer;
    if (peer) {
        if (a1 == 4 && *((BYTE*)packetData + 12) & 8) {
            p = enet_packet_create(0, packetDataSize + *((DWORD*)packetData + 13) + 5, packetFlag);
            int four = 4;
            memcpy(p->data, &four, 4);
            memcpy((char*)p->data + 4, packetData, packetDataSize);
            memcpy((char*)p->data + packetDataSize + 4, a4, *((DWORD*)packetData + 13));
            enet_peer_send(peer, 0, p);
        }
        else {
            p = enet_packet_create(0, packetDataSize + 5, packetFlag);
            memcpy(p->data, &a1, 4);
            memcpy((char*)p->data + 4, packetData, packetDataSize);
            enet_peer_send(peer, 0, p);
        }
    }
    delete (char*)packetData;
}
BYTE* server::packPlayerMovings(PlayerMovings* dataStruct) {
    BYTE* data = new BYTE[56];
    for (int i = 0; i < 56; i++) {
        data[i] = 0;
    }
    memcpy(data, &dataStruct->packetType, 4);
    memcpy(data + 4, &dataStruct->netID, 4);
    memcpy(data + 8, &dataStruct->SecondaryNetID, 4);
    memcpy(data + 12, &dataStruct->characterState, 4);
    memcpy(data + 20, &dataStruct->plantingTree, 4);
    memcpy(data + 24, &dataStruct->x, 4);
    memcpy(data + 28, &dataStruct->y, 4);
    memcpy(data + 32, &dataStruct->XSpeed, 4);
    memcpy(data + 36, &dataStruct->YSpeed, 4);
    memcpy(data + 44, &dataStruct->punchX, 4);
    memcpy(data + 48, &dataStruct->punchY, 4);
    return data;
}

PlayerMovings* server::unpackPlayerMoving(BYTE* data)
{
    PlayerMovings* dataStruct = new PlayerMovings;
    memcpy(&dataStruct->packetType, data, 4);
    memcpy(&dataStruct->netID, data + 4, 4);
    memcpy(&dataStruct->SecondaryNetID, data + 8, 4);
    memcpy(&dataStruct->characterState, data + 12, 4);
    memcpy(&dataStruct->Padding, data + 16, 4);
    memcpy(&dataStruct->plantingTree, data + 20, 4);
    memcpy(&dataStruct->x, data + 24, 4);
    memcpy(&dataStruct->y, data + 28, 4);
    memcpy(&dataStruct->XSpeed, data + 32, 4);
    memcpy(&dataStruct->YSpeed, data + 36, 4);
    memcpy(&dataStruct->SecondaryPadding, data + 40, 4);
    memcpy(&dataStruct->punchX, data + 44, 4);
    memcpy(&dataStruct->punchY, data + 48, 4);
    return dataStruct;
}


void server::punch(int x, int y) {
    int xx = (g_server->m_world.local.pos.m_x / 32) + x;
    int yy = (g_server->m_world.local.pos.m_y / 32) + y;

    PlayerMovings data{};
    data.packetType = 0x3;
    data.characterState = (int(g_server->m_world.local.pos.m_x / 32) > xx ? UPDATE_PACKET_PUNCH_TILE_LEFT : UPDATE_PACKET_PUNCH_TILE_RIGHT);
    data.x = g_server->m_world.local.pos.m_x;
    data.y = g_server->m_world.local.pos.m_y;
    data.punchX = xx;
    data.punchY = yy;
    data.netID = g_server->m_world.local.netid;
    data.plantingTree = 18;
    g_server->SendPacketRaw(false, 4, packPlayerMovings(&data), 56, 0, ENET_PACKET_FLAG_RELIABLE);
}

void server::wrench(int x, int y) {
    {
        int xx = (g_server->m_world.local.pos.m_x / 32) + x;
        int yy = (g_server->m_world.local.pos.m_y / 32) + y;
        gameupdatepacket_t packet{};
        packet.m_type = PACKET_TILE_CHANGE_REQUEST;
        packet.m_int_data = 32;
        packet.m_packet_flags = (int(g_server->m_world.local.pos.m_x / 32) > xx ? UPDATE_PACKET_TILE_CHANGE_REQUEST_LEFT : UPDATE_PACKET_TILE_CHANGE_REQUEST_RIGHT);
        packet.m_state1 = xx;
        packet.m_state2 = yy;
        packet.m_vec_x = g_server->m_world.local.pos.m_x;
        packet.m_vec_y = g_server->m_world.local.pos.m_y;

        g_server->send(false, NET_MESSAGE_GAME_PACKET, (uint8_t*)&packet, sizeof(packet));
    }
}

void server::placeBlock(int itemID, int x, int y) {
    int xx = (g_server->m_world.local.pos.m_x / 32) + x;
    int yy = (g_server->m_world.local.pos.m_y / 32) + y;

    auto targetTile = g_server->m_world.tiles.operator[](HashCoord(xx, yy));
    auto item = g_server->m_world.itemDataContainer.item_map[itemID];
    if (item.actionType == 19 ? g_server->m_world.tiles.operator[](HashCoord(xx, yy + 1)).header.foreground == 0x00 : false) {

    }
    if (item.actionType == 18 ? targetTile.header.background == 0x00 : targetTile.header.foreground == 0x00) {
        gameupdatepacket_t packet{};
        packet.m_type = PACKET_TILE_CHANGE_REQUEST;
        packet.m_netid = g_server->m_world.local.netid;
        packet.m_vec_x = g_server->m_world.local.pos.m_x;
        packet.m_vec_y = g_server->m_world.local.pos.m_y;
        packet.m_packet_flags = (int(g_server->m_world.local.pos.m_x / 32) > xx ? UPDATE_PACKET_TILE_CHANGE_REQUEST_LEFT : UPDATE_PACKET_TILE_CHANGE_REQUEST_RIGHT);
        packet.m_state1 = xx;
        packet.m_state2 = yy;
        packet.m_int_data = itemID;

        g_server->send(false, NET_MESSAGE_GAME_PACKET, (uint8_t*)&packet, sizeof(packet));

    }
}
void server::MoveXY(int x, int y) {
    try {
        auto& world = g_server->m_world;

        PathFinder pf(100, 60);

        for (int xx = 0; xx < 100; xx++) {
            for (int yy = 0; yy < 60; yy++) {
                int inta = 0;
                auto tile = world.tiles.find(HashCoord(xx, yy));

                int collisionType = world.itemDataContainer.item_map.find(tile->second.header.foreground)->second.collisionType;
                if (collisionType == 0) {
                    inta = 0;
                }
                else if (collisionType == 1) {
                    inta = 1;
                }
                else if (collisionType == 2) {
                    inta = (yy < yy ? 1 : 0);
                }
                else if (collisionType == 3) {
                    inta = !world.hasAccessName() ? (tile->second.header.flags_1 == 0x90 ? 0 : 1) : 0;
                }
                /*else if (collisionType == 4) {
                    inta = tile->second.header.flags_1 == 64 ? 0 : 1;
                }*/
                else {
                    inta = tile->second.header.foreground == 0 ? 0 : 1;
                }

                if (inta == 1) {
                    pf.setBlocked(xx, yy);
                }
            }
        }

        pf.setNeighbors({ -1, 0, 1, 0 }, { 0, 1, 0, -1 });
        vector<pair<int, int>> path = pf.aStar(g_server->m_world.local.pos.m_x / 32, g_server->m_world.local.pos.m_y / 32, x, y);

        if (path.size() > 0) {
            if (path.size() < 150)
                for (auto& p : path) {
                    gameupdatepacket_t packet{ 0 };
                    packet.m_type = PACKET_STATE;
                    packet.m_int_data = 822;

                    packet.m_vec_x = p.first * 32;
                    packet.m_vec_y = p.second * 32;
                    packet.m_state1 = p.first;
                    packet.m_state2 = p.second;
                    packet.m_packet_flags = UPDATE_PACKET_PLAYER_MOVING_RIGHT;
                    g_server->send(false, NET_MESSAGE_GAME_PACKET, (uint8_t*)&packet, sizeof(gameupdatepacket_t));

                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            else {
                for (std::size_t i = 0; i < path.size(); i += 2) {
                    gameupdatepacket_t packet{ 0 };
                    packet.m_type = PACKET_STATE;
                    packet.m_int_data = 822;
                    packet.m_vec_x = path[i].first * 32;
                    packet.m_vec_y = path[i].second * 32;
                    packet.m_state1 = path[i].first;
                    packet.m_state2 = path[i].second;
                    packet.m_packet_flags = UPDATE_PACKET_PLAYER_MOVING_RIGHT;
                    g_server->send(false, NET_MESSAGE_GAME_PACKET, (uint8_t*)&packet, sizeof(gameupdatepacket_t));

                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));


            //gameupdatepacket_t packet{};
            //packet.m_vec_x = 128.f;
            variantlist_t lost{ "OnSetPos" };
            vector2_t pos;
            pos.m_x = x * 32;
            pos.m_y = y * 32;
            lost[1] = pos;
            g_server->send(true, lost, g_server->m_world.local.netid, -1);

            variantlist_t notif{ "OnTextOverlay" };
            notif[1] = "`2Traveling `9" + to_string(path.size()) + " `2Blocks";
            // notif[4] = 0;
            g_server->send(true, notif, -1, -1);
        }
    }
    /*else {
        variantlist_t notif{ "OnTextOverlay" };
        notif[1] = "`4Traveling Not Possible";
        //notif[4] = 0;
        g_server->send(true, notif, -1, -1);
    }
}*/
    catch (exception ex)
    {
        variantlist_t notif{ "OnTextOverlay" };
        notif[1] = "`8Something Goes Wrong";
        //notif[4] = 0;
        g_server->send(true, notif, -1, -1);
    }
}
