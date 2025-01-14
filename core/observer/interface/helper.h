/*
 * Copyright 2022 iLogtail Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "network.h"
#include <arpa/inet.h>
#include <json/value.h>
#include <log_pb/sls_logs.pb.h>
#include <sstream>
#include <string>
#include <unordered_map>

#include "metas/ServiceMetaCache.h"
#include "common/xxhash/xxhash.h"

inline std::string SockAddressToString(SockAddress address) {
    char addr[INET6_ADDRSTRLEN + 1] = {'\0'};

    if (address.Type == SockAddressType_IPV4) {
        inet_ntop(AF_INET, &address.Addr.IPV4, addr, INET6_ADDRSTRLEN);
    } else {
        inet_ntop(AF_INET6, address.Addr.IPV6, addr, INET6_ADDRSTRLEN);
    }
    return {addr};
}

inline SockAddress SockAddressFromString(const std::string& ipV4V6) {
    SockAddress addr;
    if (ipV4V6.find('.') != std::string::npos) {
        addr.Type = SockAddressType_IPV4;
        inet_pton(AF_INET, ipV4V6.c_str(), &addr.Addr.IPV4);
    } else {
        addr.Type = SockAddressType_IPV6;
        inet_pton(AF_INET6, ipV4V6.c_str(), addr.Addr.IPV6);
    }
    return addr;
}

struct NetStatisticsKeyHash {
    size_t operator()(const NetStatisticsKey& key) const {
        return size_t(((uint64_t)key.PID << 32) | (uint64_t)key.SockHash);
    }
};

struct NetStatisticsKeyEqual {
    bool operator()(const NetStatisticsKey& a, const NetStatisticsKey& b) const {
        return a.PID == b.PID && a.SockHash == b.SockHash;
    }
};

// hash by connection
typedef std::unordered_map<NetStatisticsKey, NetStatisticsTCP, NetStatisticsKeyHash, NetStatisticsKeyEqual>
    NetStatisticsHashMap;

struct MergedNetStatisticsKeyHash {
    size_t operator()(const NetStatisticsKey& key) const {
        uint32_t hash = XXH32(&key.DstAddr, sizeof(key.DstAddr), 0);
        hash = XXH32(&key.DstPort, sizeof(key.DstPort), hash);
        hash = XXH32(&key.PID, sizeof(key.PID), hash);
        return hash;
    }
};
struct MergedNetStatisticsKeyEqual {
    bool operator()(const NetStatisticsKey& a, const NetStatisticsKey& b) const {
        return a.PID == b.PID && a.DstPort == b.DstPort && a.RoleType == b.RoleType && a.DstAddr == b.DstAddr;
    }
};

// hash by process and remote addr
typedef std::unordered_map<NetStatisticsKey, NetStatisticsTCP, MergedNetStatisticsKeyHash, MergedNetStatisticsKeyEqual>
    MergedNetStatisticsHashMap;

struct NetStaticticsMap {
    NetStatisticsTCP& GetStatisticsItem(const NetStatisticsKey& key) {
        auto findRst = mHashMap.find(key);
        if (findRst != mHashMap.end()) {
            return findRst->second;
        }
        static NetStatisticsTCP sNewTcp;
        auto insertIter = mHashMap.insert(std::make_pair(key, sNewTcp));
        return insertIter.first->second;
    }

    static inline void StatisticsKeyToPB(const NetStatisticsKey& key, sls_logs::Log* log, bool withLocalPort) {
        static auto sHostnameManager = logtail::ServiceMetaManager::GetInstance();
        auto remoteIp = SockAddressToString(key.DstAddr);
        Json::Value root;
        root["remote_ip"] = remoteIp;
        root["remote_port"] = key.RoleType == PacketRoleType::Server ? "0" : std::to_string(key.DstPort);
        if (key.RoleType == PacketRoleType::Client) {
            auto& serviceMeta = sHostnameManager->GetServiceMeta(key.PID, remoteIp);
            root["remote_type"]
                = ServiceCategoryToString(serviceMeta.Empty() ? ServiceCategory::Server : serviceMeta.Category);
            if (!serviceMeta.Host.empty()) {
                root["remote_host"] = serviceMeta.Host;
            }
        }
        auto content = log->add_contents();
        content->set_key("remote_info");
        content->set_value(Json::FastWriter().write(root));

        // ebpf data don't have local port
        if (withLocalPort) {
            content = log->add_contents();
            content->set_key("local_port");
            content->set_value(std::to_string(key.SrcPort));
        }

        content = log->add_contents();
        content->set_key("socket_type");
        content->set_value(SocketCategoryToString(key.SockCategory));

        content = log->add_contents();
        content->set_key("role");
        content->set_value(PacketRoleTypeToString(key.RoleType));
    }

    static inline void StatisticsTCPToPB(const NetStatisticsTCP& tcp, sls_logs::Log* log) {
        auto content = log->add_contents();
        content->set_key("send_bytes");
        content->set_value(std::to_string(tcp.Base.SendBytes));
        content = log->add_contents();
        content->set_key("recv_bytes");
        content->set_value(std::to_string(tcp.Base.RecvBytes));

        content = log->add_contents();
        content->set_key("send_packets");
        content->set_value(std::to_string(tcp.Base.SendPackets));
        content = log->add_contents();
        content->set_key("recv_packets");
        content->set_value(std::to_string(tcp.Base.RecvPackets));

        //        content = log->add_contents();
        //        content->set_key("protocol_matched");
        //        content->set_value(std::to_string(tcp.Base.ProtocolMatched));
        //        content = log->add_contents();
        //        content->set_key("protocol_unmatched");
        //        content->set_value(std::to_string(tcp.Base.ProtocolUnMatched));
        //        content = log->add_contents();
        //        content->set_key("protocol");
        //        content->set_value(ProtocolTypeToString(tcp.Base.LastInferedProtocolType));

        content = log->add_contents();
        content->set_key("send_total_latency");
        content->set_value(std::to_string(tcp.SendTotalLatency));
        content = log->add_contents();
        content->set_key("recv_total_latency");
        content->set_value(std::to_string(tcp.RecvTotalLatency));

        //        content = log->add_contents();
        //        content->set_key("send_retran_count");
        //        content->set_value(std::to_string(tcp.SendRetranCount));
        //        content = log->add_contents();
        //        content->set_key("recv_retran_count");
        //        content->set_value(std::to_string(tcp.RecvRetranCount));
        //
        //        content = log->add_contents();
        //        content->set_key("send_zerowin_count");
        //        content->set_value(std::to_string(tcp.SendZeroWinCount));
        //        content = log->add_contents();
        //        content->set_key("recv_zerowin_count");
        //        content->set_value(std::to_string(tcp.RecvZeroWinCount));
    }

    static inline void StatisticsPairToPB(const NetStatisticsKey& key,
                                          const NetStatisticsTCP& tcp,
                                          sls_logs::Log* log,
                                          bool withLocalPort) {
        StatisticsKeyToPB(key, log, withLocalPort);
        StatisticsTCPToPB(tcp, log);
    }

    inline void Clear() { mHashMap.clear(); }

    NetStatisticsHashMap mHashMap;
};

inline std::string PacketEventHeaderToString(PacketEventHeader* header) {
    std::string str;
    str.append("EventType : ").append(PacketEventTypeToString(header->EventType)).append("\n");
    str.append("PID : ").append(std::to_string(header->PID)).append("\n");
    str.append("SocketHash : ").append(std::to_string(header->SockHash)).append("\n");
    str.append("Time : ").append(std::to_string(header->TimeNano)).append("\n");
    str.append("SrcAddress : ").append(SockAddressToString(header->SrcAddr)).append("\n");
    str.append("SrcPort : ").append(std::to_string(header->SrcPort)).append("\n");
    str.append("DstAddress : ").append(SockAddressToString(header->DstAddr)).append("\n");
    str.append("DstPort : ").append(std::to_string(header->DstPort)).append("\n");
    return str;
}

inline std::string PacketEventDataToString(PacketEventData* data) {
    std::string str;
    str.append("PacketType : ").append(PacketTypeToString(data->PktType)).append("\n");
    str.append("ProtocolType : ").append(ProtocolTypeToString(data->PtlType)).append("\n");
    str.append("MessageType : ").append(MessageTypeToString(data->MsgType)).append("\n");
    str.append("RealLen : ").append(std::to_string(int(data->RealLen))).append("\n");
    str.append("BufferLen : ").append(std::to_string(int(data->BufferLen))).append("\n");
    std::stringstream ss;
    char const hex_chars[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    ss << "###############################" << std::endl;
    for (int i = 0; i < data->BufferLen; ++i) {
        if (i % 32 == 0) {
            char buf[64] = {0};
            snprintf(buf, 63, "%06d - %06d : ", i, i + 32);
            ss << buf;
        }
        if (i % 4 == 0) {
            ss << "0x";
        }
        ss << hex_chars[uint8_t(data->Buffer[i]) >> 4] << hex_chars[uint8_t(data->Buffer[i]) & 0x0F];
        if (i % 4 == 3) {
            ss << " ";
        }
        if (i % 32 == 31) {
            ss << std::endl;
        }
    }
    ss << std::endl << "###############################" << std::endl;
    str.append("Data : ").append(ss.str()).append("\n");
    str.append("Data String : \n").append(std::string(data->Buffer, data->BufferLen)).append("\n");

    return str;
}

inline std::string PacketEventToString(void* event, int32_t len) {
    if ((uint32_t)len < sizeof(PacketEventHeader)) {
        return "ErrorLength";
    }
    PacketEventHeader* header = static_cast<PacketEventHeader*>(event);
    switch (header->EventType) {
        case PacketEventType_Data: {
            PacketEventData* data = reinterpret_cast<PacketEventData*>((char*)event + sizeof(PacketEventHeader));
            return PacketEventHeaderToString(header).append("\n").append(PacketEventDataToString(data));
        }
        default:
            return PacketEventHeaderToString(header);
    }
}

inline void BufferToPacketEvent(char* buffer, int32_t buffer_len, void*& event, int32_t& len) {
    event = NULL;
    len = buffer_len;
    if ((uint32_t)buffer_len < sizeof(PacketEventHeader)) {
        return;
    }
    event = buffer;
    if (buffer_len == sizeof(PacketEventHeader)) {
        return;
    }
    PacketEventData* data = (PacketEventData*)((uint8_t*)event + sizeof(PacketEventHeader));
    data->Buffer = (char*)event + sizeof(PacketEventHeader) + sizeof(PacketEventData);
    return;
}

inline void PacketEventToBuffer(void* event, int32_t len, std::string& buffer) {
    buffer.clear();
    if ((uint32_t)len < sizeof(PacketEventHeader)) {
        return;
    }
    PacketEventHeader* header = static_cast<PacketEventHeader*>(event);

    switch (header->EventType) {
        case PacketEventType_Data: {
            PacketEventData* data = reinterpret_cast<PacketEventData*>((char*)event + sizeof(PacketEventHeader));
            buffer.resize(4 + sizeof(PacketEventHeader) + sizeof(PacketEventData) + data->BufferLen);
            *(uint32_t*)&buffer.at(0) = buffer.size() - 4;
            memcpy(&buffer.at(4), event, sizeof(PacketEventHeader) + sizeof(PacketEventData));
            memcpy(&buffer.at(4) + sizeof(PacketEventHeader) + sizeof(PacketEventData), data->Buffer, data->BufferLen);
            return;
        }
        default:
            buffer.resize(4 + sizeof(PacketEventHeader));
            *(uint32_t*)&buffer.at(0) = buffer.size() - 4;
            memcpy(&buffer.at(4), event, sizeof(PacketEventHeader));
            return;
    }
}

// true means request and false means response.
inline MessageType InferRequestOrResponse(PacketType pktType, PacketEventHeader* header) {
    // 根据端口大小粗略判断 MessageType_Response or MessageType_Request
    if (pktType == PacketType_In) {
        if (header->SrcPort > header->DstPort || header->DstPort < 30000) {
            return MessageType_Response;
        } else {
            return MessageType_Request;
        }
    } else if (pktType == PacketType_Out) {
        if (header->SrcPort > header->DstPort || header->DstPort < 30000) {
            return MessageType_Request;
        } else {
            return MessageType_Response;
        }
    }
    return MessageType_None;
}

inline PacketRoleType InferServerOrClient(PacketType pktType, MessageType messageType) {
    if (pktType == PacketType_None || messageType == MessageType_None) {
        return PacketRoleType::Unknown;
    }
    if (pktType == PacketType_In) {
        return messageType == MessageType_Request ? PacketRoleType::Server : PacketRoleType::Client;
    } else if (pktType == PacketType_Out) {
        return messageType == MessageType_Request ? PacketRoleType::Client : PacketRoleType::Server;
    }
    return PacketRoleType::Unknown;
}
