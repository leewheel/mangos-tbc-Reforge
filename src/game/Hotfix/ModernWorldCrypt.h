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

//By leewheel 2026-07-25 现代(2.5.3)客户端 World 协议包头与加密层
//参考 HermesProxy: World/Packet.cs(PacketHeader) + Framework/Cryptography/PacketCrypt.cs(WorldCrypt)
//现代包头16字节 = int32 Size(小端) + 12字节 Tag(AES-GCM认证标签)；opcode 在加密的包体内。
//加密算法：AES-128-GCM，nonce = 计数器(8字节小端) + 方向后缀("SRVR"加密/"CLNT"解密)。
#ifndef MANGOS_HFX_MODERNWORLDCRYPT_H
#define MANGOS_HFX_MODERNWORLDCRYPT_H

#include "Common.h"

#include <cstring>

// 现代客户端包头（客户端→服务端 与 服务端→客户端 通用，均16字节）
struct ModernPacketHeader
{
    static const size_t Size = 16;          // 包头总长度
    static const size_t TagSize = 12;       // AES-GCM 认证标签长度（96位）
    static const uint32 MaxPacketSize = 0x40000;  // 合法包体上限（参考代理 IsValidSize）

    uint32 packetSize = 0;                  // 包体长度（小端，不含包头）
    uint8  tag[TagSize] = { 0 };            // AES-GCM 认证标签

    // 从16字节原始数据解析（小端读Size）
    void Read(const uint8* buffer)
    {
        packetSize = uint32(buffer[0]) | (uint32(buffer[1]) << 8) |
                     (uint32(buffer[2]) << 16) | (uint32(buffer[3]) << 24);
        std::memcpy(tag, buffer + 4, TagSize);
    }

    // 写入16字节原始数据（小端写Size）
    void Write(uint8* buffer) const
    {
        buffer[0] = uint8(packetSize & 0xFF);
        buffer[1] = uint8((packetSize >> 8) & 0xFF);
        buffer[2] = uint8((packetSize >> 16) & 0xFF);
        buffer[3] = uint8((packetSize >> 24) & 0xFF);
        std::memcpy(buffer + 4, tag, TagSize);
    }

    bool IsValidSize() const { return packetSize < MaxPacketSize; }
};

// 现代 World 加密层（AES-128-GCM）
//服务端→客户端用 "SRVR" 后缀加密；客户端→服务端用 "CLNT" 后缀解密。
class ModernWorldCrypt
{
    public:
        ModernWorldCrypt();
        ~ModernWorldCrypt();

        // 用会话密钥初始化（16字节 AES-128 密钥）
        void Initialize(const uint8* key);

        bool IsInitialized() const { return m_initialized; }

        // 加密服务端发送的包体（原地），并输出12字节tag；成功后计数器自增
        bool Encrypt(uint8* data, size_t len, uint8* tag);

        // 解密客户端发来的包体（原地），校验12字节tag；成功后计数器自增
        bool Decrypt(uint8* data, size_t len, const uint8* tag);

    private:
        bool m_initialized = false;
        uint8 m_key[16] = { 0 };
        uint64 m_serverCounter = 0;         // 加密(发送)计数器
        uint64 m_clientCounter = 0;         // 解密(接收)计数器
};

#endif
//End By leewheel
