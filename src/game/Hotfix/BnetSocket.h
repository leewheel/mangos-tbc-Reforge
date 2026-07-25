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

//By leewheel 2026-07-25 BNet TLS 会话（现代客户端 Battle.net 认证入口）
//用途：在 mangosd 进程内监听 1119 端口，接受 2.5.3 客户端 TLS 连接，
//      处理 BNet RPC 认证流程（Connect/Logon/VerifyWebCredentials），
//      生成 64 字节 SessionKey 供后续世界连接使用。
//设计：不复用 AsyncSocket（它绑定原生 TCP），而是自行用 boost::asio::ssl::stream。
#ifndef MANGOS_HFX_BNETSOCKET_H
#define MANGOS_HFX_BNETSOCKET_H

#include "Common.h"
#include "Hotfix/BnetHeader.h"
#include "Hotfix/BnetConstants.h"
#include "Hotfix/BnetMessages.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

// 前置声明
class BnetSessionMgr;

// 单个 BNet TLS 会话
class BnetSocket : public std::enable_shared_from_this<BnetSocket>
{
    public:
        using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

        BnetSocket(boost::asio::io_context& io, boost::asio::ssl::context& sslCtx);
        ~BnetSocket();

        // 开始异步 TLS 握手，完成后开始读取 BNet 帧
        void Start();
        void Close();
        bool IsClosed() const;

        std::string GetRemoteAddress() const;
        uint16 GetRemotePort() const;

        // 获取账号信息（VerifyWebCredentials 后设置）
        uint32 GetAccountId() const { return m_accountId; }
        const std::string& GetAccountName() const { return m_accountName; }
        const std::vector<uint8>& GetSessionKey() const { return m_sessionKey; }

    private:
        // TLS 握手完成回调
        void OnHandshakeComplete(const boost::system::error_code& ec);

        // 异步读取帧头（2字节头长度）
        void AsyncReadFrameHeader();
        // 读取帧头 protobuf 部分
        void OnFrameHeaderLength(const boost::system::error_code& ec, std::size_t bytes);
        // 读取 payload 并分发
        void OnFrameComplete(const boost::system::error_code& ec, std::size_t bytes,
                             BnetHeader header, size_t payloadOffset);

        // RPC 分发
        void DispatchRpc(const BnetHeader& header, const uint8* payload, size_t payloadLen);

        // 服务处理器
        void HandleConnectionConnect(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleAuthLogon(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleAuthVerifyWebCredentials(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleAccountGetAccountState(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleAccountGetGameAccountState(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleGameUtilitiesClientRequest(const BnetHeader& header, const uint8* payload, size_t len);
        void HandleGameUtilitiesGetAllValues(const BnetHeader& header, const uint8* payload, size_t len);

        // 发送 RPC 响应（token 回填）
        void SendResponse(uint32 token, uint32 status, const ProtobufWriter& payload);
        void SendResponse(uint32 token, uint32 status);
        // 发送服务器推送（Request to client listener）
        void SendServerRequest(uint32 serviceHash, uint32 methodId, const ProtobufWriter& payload);

        // 物理发送帧
        void SendFrame(const BnetHeader& header, const uint8* payload, size_t payloadLen);

        // 生成 Realm 列表响应
        void BuildRealmListResponse(BnetMsg::ClientResponse& response);
        // 处理 realm join
        void HandleRealmJoin(const BnetMsg::ClientRequest& request, uint32 token);

        SslStream m_sslStream;
        friend class BnetServer;  // BnetServer需访问 m_sslStream.lowest_layer() 用于 accept
        bool m_closed = false;
        std::mutex m_sendMutex;

        // 读缓冲区
        std::vector<uint8> m_readBuffer;

        // 会话状态
        uint32 m_accountId = 0;
        std::string m_accountName;
        uint8 m_expansion = 0;
        uint32 m_accountFlags = 0;
        std::string m_locale;
        std::string m_platform;
        std::string m_loginTicket;       // "TC-"+随机hex
        std::vector<uint8> m_sessionKey; // 64 字节
        uint32 m_requestToken = 0;       // 自增 token（服务端主动推送用）
};

// BNet 会话票据存储（loginTicket → sessionKey + 账号信息）
struct BnetSessionTicket
{
    uint32 accountId = 0;
    std::string accountName;
    uint8 expansion = 0;
    std::vector<uint8> sessionKey;       // 64 字节
    std::string locale;
    std::string platform;
};

// BNet 会话管理器（单例）
class BnetSessionMgr
{
    public:
        static BnetSessionMgr& Instance();

        // 存入票据
        void AddTicket(const std::string& ticket, const BnetSessionTicket& data);
        // 取出票据（取出后删除）
        bool PopTicket(const std::string& ticket, BnetSessionTicket& out);
        // 查询票据是否存在
        bool HasTicket(const std::string& ticket) const;

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, BnetSessionTicket> m_tickets;
};

#define sBnetSessionMgr BnetSessionMgr::Instance()

#endif
//End By leewheel
