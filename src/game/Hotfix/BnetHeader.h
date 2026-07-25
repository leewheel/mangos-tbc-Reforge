/*
 * This file is part of the LWCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//By leewheel 2026-07-25 BNet RPC 头（Bgs.Protocol.Header）解析与构造
//帧格式：[2字节头长度(大端)] [Header protobuf] [payload]
#ifndef MANGOS_HFX_BNETHEADER_H
#define MANGOS_HFX_BNETHEADER_H

#include "Common.h"
#include "Hotfix/ProtobufStream.h"

// Bgs.Protocol.Header 字段编号（取自 HermesProxy RpcTypes.cs 的 Header 类，逐字核对）
enum BnetHeaderField
{
    BNET_HDR_SERVICE_ID     = 1,   // uint32 服务实例ID（客户端绑定ID；BnetTcp 回包固定 0xFE）
    BNET_HDR_METHOD_ID      = 2,   // uint32 方法ID
    BNET_HDR_TOKEN          = 3,   // uint32 请求令牌（响应需原样回填）
    BNET_HDR_OBJECT_ID      = 4,   // uint64 对象ID（WorldSocket 回包填 serviceId）
    BNET_HDR_SIZE           = 5,   // uint32 payload 字节数
    BNET_HDR_STATUS         = 6,   // uint32 BattlenetRpcErrorCode
    BNET_HDR_ERROR          = 7,   // string 错误描述（可忽略）
    BNET_HDR_TIMEOUT        = 8,   // uint32 超时（可忽略）
    BNET_HDR_IS_RESPONSE    = 9,   // bool   是否响应（可忽略）
    BNET_HDR_FORWARD_TARGETS= 10,  // repeated 转发目标（可忽略）
    BNET_HDR_SERVICE_HASH   = 11,  // uint32 服务哈希（OriginalHash，路由用）
    BNET_HDR_CLIENT_ID      = 13,  // ProcessId 客户端ID（可忽略）
};

// BNet RPC 头
struct BnetHeader
{
    uint32 serviceId = 0;
    uint32 methodId = 0;
    uint32 token = 0;
    uint64 objectId = 0;
    uint32 size = 0;          // payload 长度
    uint32 status = 0;
    uint32 serviceHash = 0;   // 路由用（OriginalHash）
    std::string error;        // 错误描述（可忽略）

    // 从 protobuf 字节解析；成功返回 true
    bool Read(const uint8* data, size_t len)
    {
        ProtobufReader reader(data, len);
        while (!reader.Eof())
        {
            uint32 field = 0, wire = 0;
            if (!reader.ReadTag(field, wire))
                return false;
            uint64 v = 0;
            switch (field)
            {
                case BNET_HDR_SERVICE_ID:   if (!reader.ReadVarint(v)) return false; serviceId = uint32(v); break;
                case BNET_HDR_METHOD_ID:    if (!reader.ReadVarint(v)) return false; methodId = uint32(v); break;
                case BNET_HDR_TOKEN:        if (!reader.ReadVarint(v)) return false; token = uint32(v); break;
                case BNET_HDR_OBJECT_ID:    if (!reader.ReadVarint(v)) return false; objectId = v; break;
                case BNET_HDR_SIZE:         if (!reader.ReadVarint(v)) return false; size = uint32(v); break;
                case BNET_HDR_STATUS:       if (!reader.ReadVarint(v)) return false; status = uint32(v); break;
                case BNET_HDR_SERVICE_HASH: if (!reader.ReadVarint(v)) return false; serviceHash = uint32(v); break;
                case BNET_HDR_ERROR:        if (!reader.ReadString(error)) return false; break;
                default:
                    if (!reader.SkipField(wire))
                        return false;
                    break;
            }
        }
        return true;
    }

    // 序列化为 protobuf 字节
    void Write(ProtobufWriter& writer) const
    {
        if (serviceId)   writer.WriteUInt32(BNET_HDR_SERVICE_ID, serviceId);
        if (methodId)    writer.WriteUInt32(BNET_HDR_METHOD_ID, methodId);
        if (token)       writer.WriteUInt32(BNET_HDR_TOKEN, token);
        if (objectId)    writer.WriteUInt64(BNET_HDR_OBJECT_ID, objectId);
        if (size)        writer.WriteUInt32(BNET_HDR_SIZE, size);
        if (status)      writer.WriteUInt32(BNET_HDR_STATUS, status);
        if (!error.empty()) writer.WriteString(BNET_HDR_ERROR, error);
        if (serviceHash) writer.WriteUInt32(BNET_HDR_SERVICE_HASH, serviceHash);
    }
};

// BNet 帧解析辅助：从缓冲区尝试解析一个完整帧
struct BnetFrameParseResult
{
    bool complete = false;     // 缓冲区是否已含完整帧
    size_t totalLength = 0;    // 完整帧总长度（2 + 头长 + payload）
    BnetHeader header;         // 解析出的头
};

// 尝试从缓冲区解析一帧；数据不足时 complete=false
inline BnetFrameParseResult ParseBnetFrame(const uint8* buffer, size_t bufferLen)
{
    BnetFrameParseResult result;
    if (bufferLen < 2)
        return result;

    // 头长度为2字节大端
    uint16 headerLength = uint16((uint16(buffer[0]) << 8) | buffer[1]);
    if (bufferLen < size_t(2 + headerLength))
        return result;

    if (!result.header.Read(buffer + 2, headerLength))
        return result;

    size_t total = size_t(2 + headerLength) + result.header.size;
    if (bufferLen < total)
        return result;

    result.complete = true;
    result.totalLength = total;
    return result;
}

#endif
//End By leewheel
