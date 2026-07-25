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

//By leewheel 2026-07-25 现代(2.5.3)客户端 World 连接处理
//用途：在 mangosd 进程内监听 ModernWorldPort，接受 2.5.3 客户端 TCP 连接，
//      处理现代认证握手（AUTH_CHALLENGE/AUTH_SESSION/ENTER_ENCRYPTED_MODE），
//      启用 AES-128-GCM 加密后，进行 opcode 翻译并桥接到已有 WorldSession。
//设计：独立 TCP socket（不使用 TLS，不复用 AsyncSocket），自行管理生命周期。
#ifndef MANGOS_HFX_MODERNWORLDSOCKET_H
#define MANGOS_HFX_MODERNWORLDSOCKET_H

#include "Common.h"
#include "Hotfix/ModernWorldCrypt.h"
#include "Hotfix/ModernCrypto.h"

#include <boost/asio.hpp>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class WorldSession;
class WorldPacket;

// 现代客户端 world 连接状态
enum ModernWorldState
{
    MODERN_STATE_CONNECTED = 0,        // 刚连接，等待发送 AUTH_CHALLENGE
    MODERN_STATE_CHALLENGE_SENT,       // 已发送 AUTH_CHALLENGE，等待 AUTH_SESSION
    MODERN_STATE_ENCRYPTING,           // 已发送 ENTER_ENCRYPTED_MODE，等待 ACK
    MODERN_STATE_AUTHED,               // 加密已启用，正常收发包
    MODERN_STATE_CLOSED                // 已关闭
};

// 现代(2.5.3)客户端专用 opcode 数值（不在 legacy 映射中的握手/系统/hotfix包）
namespace ModernWorldOpcode
{
    // 握手包
    const uint32 SMSG_AUTH_CHALLENGE       = 0x3048;
    const uint32 CMSG_AUTH_SESSION         = 0x3765;
    const uint32 SMSG_ENTER_ENCRYPTED_MODE = 0x3049;
    const uint32 CMSG_ENTER_ENCRYPTED_MODE_ACK = 0x3767;
    const uint32 SMSG_AUTH_RESPONSE        = 0x256D;
    const uint32 CMSG_PING                 = 0x3768;
    const uint32 SMSG_PONG                 = 0x304E;
    const uint32 CMSG_KEEP_ALIVE           = 0x3681;
    // 认证后系统包
    const uint32 SMSG_SET_TIME_ZONE_INFORMATION = 0x2670;
    const uint32 SMSG_FEATURE_SYSTEM_STATUS_GLUE_SCREEN = 0x25BD;
    const uint32 SMSG_CACHE_VERSION        = 0x291C;
    const uint32 SMSG_AVAILABLE_HOTFIXES   = 0x290F;
    const uint32 SMSG_BATTLE_NET_CONNECTION_STATUS = 0x2801;
    // Hotfix 相关
    const uint32 CMSG_DB_QUERY_BULK        = 0x35E5;
    const uint32 CMSG_HOTFIX_REQUEST       = 0x35E6;
    const uint32 SMSG_DB_REPLY             = 0x290E;
    const uint32 SMSG_HOTFIX_CONNECT       = 0x2911;
    // 角色列表
    const uint32 SMSG_ENUM_CHARACTERS_RESULT = 0x2583;
    // 登录
    const uint32 CMSG_PLAYER_LOGIN          = 0x35EB;
    const uint32 CMSG_CHAR_DELETE           = 0x369C;
}

// DB2 表哈希值（用于 hotfix 请求/响应）
namespace DB2Hash
{
    const uint32 BroadcastText       = 0x021826BB;
    const uint32 Item                = 0x50238EC2;
    const uint32 ItemSparse          = 0x919BE54E;
    const uint32 ItemEffect          = 0x4002A5B1;
    const uint32 ItemAppearance      = 0x42261B89;
    const uint32 ItemModifiedAppearance = 0xE491AC55;
}

// 现代 World Socket（处理 2.5.3 客户端直连）
class ModernWorldSocket : public std::enable_shared_from_this<ModernWorldSocket>
{
    public:
        ModernWorldSocket(boost::asio::io_context& io);
        ~ModernWorldSocket();

        // 连接建立后调用，发送 AUTH_CHALLENGE 并开始异步读取
        void Start();
        void Close();
        bool IsClosed() const;

        // 发送已翻译的 WorldPacket（由 WorldSession 回调）
        void SendPacket(const WorldPacket& pkt);

        std::string GetRemoteAddress() const;
        uint16 GetRemotePort() const;

        // 获取底层 socket 供 acceptor 使用
        boost::asio::ip::tcp::socket& GetSocket() { return m_socket; }

        // 获取关联的 WorldSession
        WorldSession* GetSession() const { return m_session; }

    private:
        // 发送现代格式包（opcode + data 合并为 payload，经加密后发送）
        void SendModernPacket(uint32 modernOpcode, const uint8* data, size_t len);
        // 直接发送原始帧（header + payload），不经翻译
        void SendRawFrame(const uint8* headerBuf, const uint8* payload, size_t payloadLen);

        // 异步读取16字节现代包头
        void AsyncReadHeader();
        // 包头读取完成
        void OnHeaderRead(const boost::system::error_code& ec, std::size_t bytes);
        // 包体读取完成
        void OnBodyRead(const boost::system::error_code& ec, std::size_t bytes);

        // 握手处理
        void SendAuthChallenge();
        void HandleAuthSession(const uint8* body, size_t len);
        void SendEnterEncryptedMode();
        void HandleEnterEncryptedModeAck();
        void SendAuthResponse();

        // 认证后系统包序列
        void SendPostAuthPackets();
        void SendSetTimeZoneInformation();
        void SendFeatureSystemStatusGlueScreen();
        void SendCacheVersion();
        void SendAvailableHotfixes();
        void SendBnetConnectionState();

        // Hotfix 处理
        void HandleDbQueryBulk(const uint8* data, size_t len);
        void HandleHotfixRequest(const uint8* data, size_t len);
        void SendDbReply(uint32 tableHash, uint32 recordId, uint32 timestamp, uint8 status,
                         const uint8* data, size_t dataLen);
        
        // 角色列表包体翻译（legacy 2.4.3 → modern 2.5.3）
        void TranslateCharEnum(const WorldPacket& pkt);
        // PackedGuid128 读写辅助
        void WritePackedGuid128(std::vector<uint8>& buf, uint64 low, uint64 high);
        bool ReadPackedGuid128(const uint8* data, size_t len, size_t& offset, uint64& outLow, uint64& outHigh);
        // CMSG 包体翻译（modern → legacy）
        void HandlePlayerLogin(const uint8* data, size_t len);
        // 通用: 仅含 PackedGuid128 的 CMSG 翻译为 uint64
        void TranslateGuidOnlyCmsg(uint16 legacyOpcode, const uint8* data, size_t len);

        // 正常包处理（加密启用后）
        void HandleIncomingPacket(uint32 modernOpcode, const uint8* data, size_t len);
        void HandlePing(const uint8* data, size_t len);

        // Bit 打包辅助
        void WriteBits(std::vector<uint8>& buf, uint32 value, int bitCount, int& bitPos, uint8& curByte);
        void FlushBits(std::vector<uint8>& buf, int& bitPos, uint8& curByte);

        boost::asio::ip::tcp::socket m_socket;
        ModernWorldState m_state = MODERN_STATE_CONNECTED;
        std::mutex m_sendMutex;

        // 加密层
        ModernWorldCrypt m_crypt;
        uint8 m_encryptKey[16] = { 0 };

        // 认证状态
        uint8 m_serverChallenge[16] = { 0 };
        uint32 m_accountId = 0;
        std::string m_accountName;
        uint8 m_expansion = 0;
        std::vector<uint8> m_sessionKey;       // 64字节（来自 BNet 流程）

        // 读缓冲区
        uint8 m_headerBuf[ModernPacketHeader::Size];
        std::vector<uint8> m_bodyBuf;

        // 桥接到已有 WorldSession
        WorldSession* m_session = nullptr;
};

#endif
//End By leewheel
