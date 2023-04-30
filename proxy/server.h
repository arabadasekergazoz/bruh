#pragma once
#include <string>
#include "proton/variant.hpp"
#include "enet/include/enet.h"
#include "world.h"
#include <mutex>
using namespace std;
class server {
private:
    ENetHost* m_server_host;
    ENetHost* m_proxy_server;
    ENetHost* m_real_server;
    ENetPeer* m_server_peer;
    ENetPeer* m_gt_peer;


    void handle_outgoing();
    void handle_incoming();
    bool connect();
    void disconnect(bool reset);
public:
    vector<string> playerName;
    static vector<int> adminNetID;
    mutex mtx;
    static condition_variable cv;
public:

    struct Item {
        uint16_t id;
        uint8_t count;
        uint8_t type;
    };
    static std::vector<server::Item> inventory;
    int m_user = 0;
    int m_token = 0;
    std::string m_server = "213.179.209.168";
    std::string meta = "NULL";
    std::string ipserver = "127.0.0.1";
    std::string create = "0.0.0.0";
    int m_port = 17198;
    int m_proxyport = 17191;
    World m_world;
    bool start();
    void quit();
    bool setup_client();
    bool reconnect();
    void MoveXY(int x, int y);
    BYTE* packPlayerMovings(PlayerMovings* dataStruct);
    PlayerMovings* unpackPlayerMoving(BYTE* data);
    void punch(int x, int y);
    void wrench(int x, int y);
    void placeBlock(int itemID, int x, int y);
    void SendPacketRaw(bool client, int a1, void* packetData, size_t packetDataSize, void* a4, int packetFlag);
    void redirect_server(variantlist_t& varlist);
    void send(bool client, int32_t type, uint8_t* data, int32_t len);
    void send(bool client, variantlist_t& list, int32_t netid = -1, int32_t delay = 0);
    void send(bool client, std::string packet, int32_t type = 2);
    void poll();
    vector2_t lastPos{};
};
extern server* g_server;