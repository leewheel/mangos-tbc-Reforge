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

//By leewheel 2026-07-25 BNet TLS 会话实现
#include "Hotfix/BnetSocket.h"
#include "Hotfix/BnetHeader.h"
#include "Hotfix/BnetConstants.h"
#include "Hotfix/BnetMessages.h"
#include "Database/DatabaseEnv.h"
#include "Database/QueryResult.h"
#include "Log/Log.h"
#include "Config/Config.h"
#include "Util/Util.h"

#include <openssl/rand.h>
#include <ctime>
#include <sstream>
#include <iomanip>

// ============================================================================
// BnetSessionMgr 实现（票据管理）
// ============================================================================

BnetSessionMgr& BnetSessionMgr::Instance()
{
    static BnetSessionMgr inst;
    return inst;
}

void BnetSessionMgr::AddTicket(const std::string& ticket, const BnetSessionTicket& data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tickets[ticket] = data;
}

bool BnetSessionMgr::PopTicket(const std::string& ticket, BnetSessionTicket& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tickets.find(ticket);
    if (it == m_tickets.end())
        return false;
    out = std::move(it->second);
    m_tickets.erase(it);
    return true;
}

bool BnetSessionMgr::HasTicket(const std::string& ticket) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tickets.count(ticket) > 0;
}

// ============================================================================
// BnetSocket 实现
// ============================================================================

BnetSocket::BnetSocket(boost::asio::io_context& io, boost::asio::ssl::context& sslCtx)
    : m_sslStream(io, sslCtx)
{
}

BnetSocket::~BnetSocket()
{
    Close();
}

void BnetSocket::Start()
{
    // 异步 TLS 握手
    auto self = shared_from_this();
    m_sslStream.async_handshake(boost::asio::ssl::stream_base::server,
        [self](const boost::system::error_code& ec)
        {
            self->OnHandshakeComplete(ec);
        });
}

void BnetSocket::Close()
{
    if (m_closed)
        return;
    m_closed = true;
    boost::system::error_code ec;
    m_sslStream.lowest_layer().close(ec);
}

bool BnetSocket::IsClosed() const
{
    return m_closed || !m_sslStream.lowest_layer().is_open();
}

std::string BnetSocket::GetRemoteAddress() const
{
    try
    {
        return m_sslStream.lowest_layer().remote_endpoint().address().to_string();
    }
    catch (...)
    {
        return "0.0.0.0";
    }
}

uint16 BnetSocket::GetRemotePort() const
{
    try
    {
        return m_sslStream.lowest_layer().remote_endpoint().port();
    }
    catch (...)
    {
        return 0;
    }
}

void BnetSocket::OnHandshakeComplete(const boost::system::error_code& ec)
{
    if (ec)
    {
        sLog.outError("[BNet] TLS握手失败 来自 %s: %s", GetRemoteAddress().c_str(), ec.message().c_str());
        Close();
        return;
    }
    sLog.outBasic("[BNet] TLS连接建立 来自 %s:%u", GetRemoteAddress().c_str(), GetRemotePort());
    AsyncReadFrameHeader();
}

// 读取 BNet 帧头（2字节大端 = Header protobuf 长度）
void BnetSocket::AsyncReadFrameHeader()
{
    if (IsClosed())
        return;

    m_readBuffer.resize(2);
    auto self = shared_from_this();
    boost::asio::async_read(m_sslStream,
        boost::asio::buffer(m_readBuffer.data(), 2),
        [self](const boost::system::error_code& ec, std::size_t bytes)
        {
            self->OnFrameHeaderLength(ec, bytes);
        });
}

void BnetSocket::OnFrameHeaderLength(const boost::system::error_code& ec, std::size_t /*bytes*/)
{
    if (ec)
    {
        if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted)
            sLog.outError("[BNet] 读取帧头长度失败 来自 %s: %s", GetRemoteAddress().c_str(), ec.message().c_str());
        Close();
        return;
    }

    // 2字节大端 = Header protobuf 长度
    uint16 headerLen = uint16((uint16(m_readBuffer[0]) << 8) | m_readBuffer[1]);
    if (headerLen == 0 || headerLen > 4096)
    {
        sLog.outError("[BNet] 非法帧头长度 %u 来自 %s", headerLen, GetRemoteAddress().c_str());
        Close();
        return;
    }

    // 先读取 Header protobuf
    m_readBuffer.resize(headerLen);
    auto self = shared_from_this();
    boost::asio::async_read(m_sslStream,
        boost::asio::buffer(m_readBuffer.data(), headerLen),
        [self, headerLen](const boost::system::error_code& ec2, std::size_t /*bytes2*/)
        {
            if (ec2)
            {
                if (ec2 != boost::asio::error::eof)
                    sLog.outError("[BNet] 读取帧头失败 来自 %s: %s", self->GetRemoteAddress().c_str(), ec2.message().c_str());
                self->Close();
                return;
            }

            BnetHeader header;
            if (!header.Read(self->m_readBuffer.data(), headerLen))
            {
                sLog.outError("[BNet] 解析帧头失败 来自 %s", self->GetRemoteAddress().c_str());
                self->Close();
                return;
            }

            if (header.size == 0)
            {
                // 无 payload，直接分发
                self->DispatchRpc(header, nullptr, 0);
                self->AsyncReadFrameHeader();
                return;
            }

            if (header.size > 0x10000)
            {
                sLog.outError("[BNet] Payload过大 %u 来自 %s", header.size, self->GetRemoteAddress().c_str());
                self->Close();
                return;
            }

            // 读取 payload
            self->m_readBuffer.resize(header.size);
            boost::asio::async_read(self->m_sslStream,
                boost::asio::buffer(self->m_readBuffer.data(), header.size),
                [self, header](const boost::system::error_code& ec3, std::size_t bytes3)
                {
                    self->OnFrameComplete(ec3, bytes3, header, 0);
                });
        });
}

void BnetSocket::OnFrameComplete(const boost::system::error_code& ec, std::size_t /*bytes*/,
                                  BnetHeader header, size_t /*payloadOffset*/)
{
    if (ec)
    {
        if (ec != boost::asio::error::eof)
            sLog.outError("[BNet] 读取payload失败 来自 %s: %s", GetRemoteAddress().c_str(), ec.message().c_str());
        Close();
        return;
    }

    DispatchRpc(header, m_readBuffer.data(), header.size);
    AsyncReadFrameHeader();
}

// ============================================================================
// RPC 分发
// ============================================================================

void BnetSocket::DispatchRpc(const BnetHeader& header, const uint8* payload, size_t payloadLen)
{
    // 响应帧（serviceId == 0xFE）忽略
    if (header.serviceId == 0xFE)
        return;

    switch (header.serviceHash)
    {
        case BnetServiceHash::ConnectionService:
            if (header.methodId == BnetMethod::Connect)
                HandleConnectionConnect(header, payload, payloadLen);
            else if (header.methodId == BnetMethod::KeepAlive)
                SendResponse(header.token, BNET_RPC_OK); // KeepAlive 直接回 Ok
            else
                SendResponse(header.token, BNET_RPC_NOT_IMPLEMENTED);
            break;

        case BnetServiceHash::AuthenticationService:
            if (header.methodId == BnetMethod::Logon)
                HandleAuthLogon(header, payload, payloadLen);
            else if (header.methodId == BnetMethod::VerifyWebCredentials)
                HandleAuthVerifyWebCredentials(header, payload, payloadLen);
            else
                SendResponse(header.token, BNET_RPC_NOT_IMPLEMENTED);
            break;

        case BnetServiceHash::AccountService:
            if (header.methodId == BnetMethod::GetAccountState)
                HandleAccountGetAccountState(header, payload, payloadLen);
            else if (header.methodId == BnetMethod::GetGameAccountState)
                HandleAccountGetGameAccountState(header, payload, payloadLen);
            else
                SendResponse(header.token, BNET_RPC_NOT_IMPLEMENTED);
            break;

        case BnetServiceHash::GameUtilitiesService:
            if (header.methodId == BnetMethod::GenericClientRequest)
                HandleGameUtilitiesClientRequest(header, payload, payloadLen);
            else if (header.methodId == BnetMethod::GetAllValuesForAttribute)
                HandleGameUtilitiesGetAllValues(header, payload, payloadLen);
            else
                SendResponse(header.token, BNET_RPC_NOT_IMPLEMENTED);
            break;

        default:
            // 未知服务，回 Ok 空包（某些服务客户端只需知道服务端不拒绝）
            SendResponse(header.token, BNET_RPC_OK);
            break;
    }
}

// ============================================================================
// ConnectionService::Connect
// ============================================================================

void BnetSocket::HandleConnectionConnect(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::ConnectRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    // 构造响应
    BnetMsg::ConnectResponse resp;
    resp.serverId.label = 1;
    resp.serverId.epoch = uint32(std::time(nullptr));
    resp.clientId = req.clientId;
    resp.useBindlessRpc = req.useBindlessRpc;
    resp.serverTime = uint64(std::time(nullptr)) * 1000; // 毫秒

    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(header.token, BNET_RPC_OK, pw);
}

// ============================================================================
// AuthenticationService::Logon
// ============================================================================

void BnetSocket::HandleAuthLogon(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::LogonRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    m_locale = req.locale;
    m_platform = req.platform;

    // 先回空响应（Ok）
    SendResponse(header.token, BNET_RPC_OK);

    // 推送 ChallengeExternalRequest（web_auth_url）到客户端
    // URL 格式：https://<host>:<port>/bnetserver/login/
    std::string host = sConfig.GetStringDefault("BNet.ExternalAddress", "127.0.0.1");
    int restPort = sConfig.GetIntDefault("BNet.RestPort", 8081);

    std::ostringstream url;
    url << "https://" << host << ":" << restPort << "/bnetserver/login/";

    BnetMsg::ChallengeExternalRequest challenge;
    challenge.payloadType = "web_auth_url";
    std::string urlStr = url.str();
    challenge.payload.assign(urlStr.begin(), urlStr.end());

    ProtobufWriter pw;
    challenge.Write(pw);
    SendServerRequest(BnetServiceHash::ChallengeListener, BnetMethod::ChallengeExternal, pw);
}

// ============================================================================
// AuthenticationService::VerifyWebCredentials
// ============================================================================

void BnetSocket::HandleAuthVerifyWebCredentials(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::VerifyWebCredentialsRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    // webCredentials = login ticket 字符串
    std::string ticket(req.webCredentials.begin(), req.webCredentials.end());

    // 从票据管理器取出
    BnetSessionTicket ticketData;
    if (!sBnetSessionMgr.PopTicket(ticket, ticketData))
    {
        sLog.outError("[BNet] VerifyWebCredentials 票据无效: %s 来自 %s", ticket.c_str(), GetRemoteAddress().c_str());
        // 推送 LogonResult 失败
        BnetMsg::LogonResult result;
        result.errorCode = BNET_RPC_DENIED;
        ProtobufWriter pw;
        result.Write(pw);
        SendServerRequest(BnetServiceHash::AuthenticationListener, BnetMethod::LogonResult, pw);
        SendResponse(header.token, BNET_RPC_OK);
        return;
    }

    // 保存到会话
    m_accountId = ticketData.accountId;
    m_accountName = ticketData.accountName;
    m_expansion = ticketData.expansion;
    m_sessionKey = ticketData.sessionKey;
    m_locale = ticketData.locale;
    m_platform = ticketData.platform;
    m_loginTicket = ticket;

    // 先回 Ok
    SendResponse(header.token, BNET_RPC_OK);

    // 推送 LogonResult 给客户端
    BnetMsg::LogonResult result;
    result.errorCode = 0; // 成功
    result.accountId.high = BnetEntityHigh::Account;
    result.accountId.low = uint64(m_accountId);
    // 游戏账号（与账号ID相同）
    BnetMsg::EntityId gameAcct;
    gameAcct.high = BnetEntityHigh::GameAccount;
    gameAcct.low = uint64(m_accountId);
    result.gameAccountId.push_back(gameAcct);
    result.sessionKey = m_sessionKey;

    ProtobufWriter pw;
    result.Write(pw);
    SendServerRequest(BnetServiceHash::AuthenticationListener, BnetMethod::LogonResult, pw);

    sLog.outBasic("[BNet] 账号 %s (id:%u) 认证成功 来自 %s", m_accountName.c_str(), m_accountId, GetRemoteAddress().c_str());
}

// ============================================================================
// AccountService::GetAccountState
// ============================================================================

void BnetSocket::HandleAccountGetAccountState(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::GetAccountStateRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    BnetMsg::GetAccountStateResponse resp;
    if (req.options.fieldPrivacyInfo)
    {
        resp.state.privacyInfo.isUsingRid = false;
        resp.state.privacyInfo.isVisibleForViewFriends = false;
        resp.state.privacyInfo.isHiddenFromFriendFinder = true;
        resp.tags.privacyInfoTag = BnetFieldTag::PrivacyInfo;
    }

    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(header.token, BNET_RPC_OK, pw);
}

// ============================================================================
// AccountService::GetGameAccountState
// ============================================================================

void BnetSocket::HandleAccountGetGameAccountState(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::GetGameAccountStateRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    BnetMsg::GetGameAccountStateResponse resp;
    if (req.options.fieldGameLevelInfo)
    {
        resp.state.hasGameLevelInfo = true;
        resp.state.gameLevelInfo.name = m_accountName;
        resp.state.gameLevelInfo.program = BNET_PROGRAM_WOW;
        resp.tags.gameLevelInfoTag = BnetFieldTag::GameLevelInfo;
    }
    if (req.options.fieldGameStatus)
    {
        resp.state.hasGameStatus = true;
        resp.state.gameStatus.isSuspended = false;
        resp.state.gameStatus.isBanned = false;
        resp.state.gameStatus.program = BNET_PROGRAM_WOW;
        resp.tags.gameStatusTag = BnetFieldTag::GameStatus;
    }

    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(header.token, BNET_RPC_OK, pw);
}

// ============================================================================
// GameUtilitiesService::ProcessClientRequest
// ============================================================================

void BnetSocket::HandleGameUtilitiesClientRequest(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::ClientRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    // 根据 Command_ 属性判断请求类型
    const BnetMsg::Attribute* cmdAttr = req.FindAttribute("Command_RealmListRequest_v1_b9");
    if (cmdAttr)
    {
        // Realm 列表请求
        BnetMsg::ClientResponse resp;
        BuildRealmListResponse(resp);
        ProtobufWriter pw;
        resp.Write(pw);
        SendResponse(header.token, BNET_RPC_OK, pw);
        return;
    }

    // Realm join 请求
    const BnetMsg::Attribute* joinAttr = req.FindAttribute("Command_RealmJoinRequest_v1_b9");
    if (joinAttr)
    {
        HandleRealmJoin(req, header.token);
        return;
    }

    // 其他 GameUtilities 请求直接回空 Ok
    BnetMsg::ClientResponse resp;
    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(header.token, BNET_RPC_OK, pw);
}

// ============================================================================
// GameUtilitiesService::GetAllValuesForAttribute
// ============================================================================

void BnetSocket::HandleGameUtilitiesGetAllValues(const BnetHeader& header, const uint8* payload, size_t len)
{
    BnetMsg::GetAllValuesForAttributeRequest req;
    if (payload && len > 0)
        req.Read(payload, len);

    // 客户端请求 "Param_RealmList" 时返回空列表即可
    BnetMsg::GetAllValuesForAttributeResponse resp;
    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(header.token, BNET_RPC_OK, pw);
}

// ============================================================================
// Realm 列表 / Join
// ============================================================================

void BnetSocket::BuildRealmListResponse(BnetMsg::ClientResponse& response)
{
    // 构造一个最小的 realm 列表给客户端
    // 格式参考代理的 RealmListHandler：返回 Param_RealmList 属性
    // 这里构造最简化的 JSON-like 二进制不行，实际需要按代理的 CompressedData 格式
    // 暂时构造简单的 realm 信息
    std::string realmName = sConfig.GetStringDefault("BNet.RealmName", "LWCore TBC");
    std::string realmAddr = sConfig.GetStringDefault("BNet.ExternalAddress", "127.0.0.1");
    int worldPort = sConfig.GetIntDefault("WorldServerPort", 8085);

    // 按代理格式构造 RealmListUpdates（嵌套 protobuf）
    // 简化：只返回一个 Realm
    // 实际格式较复杂，暂用简化版让客户端能看到 realm
    ProtobufWriter realmEntry;
    // WowRealmAddress = (realmId << 8) | site | region
    realmEntry.WriteUInt32(1, 1);           // update 数组标记
    // 嵌套 RealmState
    ProtobufWriter realmState;
    // RealmEntry 嵌套
    ProtobufWriter entry;
    entry.WriteFixed32(1, 0x010001);    // wowRealmAddress (region=1, site=0, realm=1)
    entry.WriteUInt32(2, 7);             // cfgRealmsID
    entry.WriteUInt32(3, 0);             // flags
    entry.WriteString(4, realmName);     // name
    entry.WriteUInt32(5, 7);             // cfgCategoriesID
    entry.WriteUInt32(6, 1);             // cfgRealmsCategory（PVE=1）
    entry.WriteUInt32(7, 0);             // populationState
    entry.WriteUInt32(8, 2);             // cfgConfigsID（TBC=2）
    entry.WriteUInt32(10, 0x00000002);   // version build = 2 (expansion TBC flag)

    BnetMsg::WriteSubMessage(realmState, 1, entry);
    realmState.WriteUInt32(2, 0); // deleting = false

    BnetMsg::WriteSubMessage(realmEntry, 1, realmState);

    // RealmListUpdates wrapper
    ProtobufWriter updates;
    BnetMsg::WriteSubMessage(updates, 1, realmEntry);

    // 序列化并放入属性
    const std::vector<uint8>& updatesData = updates.Data();
    response.AddAttribute("Param_RealmList",
        BnetMsg::Variant::MakeBlob(updatesData.data(), updatesData.size()));

    // CharacterCountList（空列表）
    ProtobufWriter charCount;
    response.AddAttribute("Param_CharacterCountList",
        BnetMsg::Variant::MakeBlob(charCount.Data().data(), charCount.Data().size()));
}

void BnetSocket::HandleRealmJoin(const BnetMsg::ClientRequest& request, uint32 token)
{
    // 客户端要加入 realm，返回连接地址
    std::string realmAddr = sConfig.GetStringDefault("BNet.ExternalAddress", "127.0.0.1");
//By leewheel 2026-07-25 现代客户端连接到 ModernWorldPort（而非 legacy WorldServerPort）
    int worldPort = sConfig.GetIntDefault("ModernWorldPort", 8086);
//End By leewheel

    // 构造 ServerAddress 列表
    ProtobufWriter joinResp;
    // server_ip_address（嵌套 ServerIPAddress: ip=1 string, port=2 uint32）
    ProtobufWriter serverAddr;
    std::ostringstream addrStr;
    addrStr << realmAddr << ":" << worldPort;
    serverAddr.WriteString(1, addrStr.str());
    serverAddr.WriteUInt32(2, uint32(worldPort));

    // JoinResponse: server_address=1 repeated, join_ticket=2 bytes
    ProtobufWriter addrWrapper;
    BnetMsg::WriteSubMessage(addrWrapper, 1, serverAddr);

    BnetMsg::WriteSubMessage(joinResp, 1, addrWrapper);

    // join_ticket = login ticket bytes
    if (!m_loginTicket.empty())
        joinResp.WriteBytes(2, reinterpret_cast<const uint8*>(m_loginTicket.data()), m_loginTicket.size());

    // 存入票据（世界连接时需要验证）
    BnetSessionTicket ticketForWorld;
    ticketForWorld.accountId = m_accountId;
    ticketForWorld.accountName = m_accountName;
    ticketForWorld.expansion = m_expansion;
    ticketForWorld.sessionKey = m_sessionKey;
    ticketForWorld.locale = m_locale;
    ticketForWorld.platform = m_platform;
    sBnetSessionMgr.AddTicket(m_loginTicket, ticketForWorld);

    // 包装成 ClientResponse
    BnetMsg::ClientResponse resp;
    const std::vector<uint8>& joinData = joinResp.Data();
    resp.AddAttribute("Param_RealmJoinTicket",
        BnetMsg::Variant::MakeBlob(joinData.data(), joinData.size()));
    resp.AddAttribute("Param_ServerAddresses",
        BnetMsg::Variant::MakeString(addrStr.str()));

    ProtobufWriter pw;
    resp.Write(pw);
    SendResponse(token, BNET_RPC_OK, pw);
}

// ============================================================================
// 帧发送
// ============================================================================

void BnetSocket::SendResponse(uint32 token, uint32 status, const ProtobufWriter& payload)
{
    BnetHeader hdr;
    hdr.serviceId = 0xFE;  // 响应标记
    hdr.token = token;
    hdr.status = status;
    hdr.size = uint32(payload.Size());

    SendFrame(hdr, payload.Data().data(), payload.Size());
}

void BnetSocket::SendResponse(uint32 token, uint32 status)
{
    BnetHeader hdr;
    hdr.serviceId = 0xFE;
    hdr.token = token;
    hdr.status = status;
    hdr.size = 0;

    SendFrame(hdr, nullptr, 0);
}

void BnetSocket::SendServerRequest(uint32 serviceHash, uint32 methodId, const ProtobufWriter& payload)
{
    BnetHeader hdr;
    hdr.serviceId = 0;       // 服务端推送用0
    hdr.serviceHash = serviceHash;
    hdr.methodId = methodId;
    hdr.token = ++m_requestToken;
    hdr.size = uint32(payload.Size());

    SendFrame(hdr, payload.Data().data(), payload.Size());
}

void BnetSocket::SendFrame(const BnetHeader& header, const uint8* payload, size_t payloadLen)
{
    if (IsClosed())
        return;

    // 序列化 header protobuf
    ProtobufWriter headerPb;
    header.Write(headerPb);

    // 帧 = [2字节大端头长] + [Header protobuf] + [payload]
    uint16 headerLen = uint16(headerPb.Size());
    std::vector<uint8> frame;
    frame.reserve(2 + headerLen + payloadLen);
    frame.push_back(uint8((headerLen >> 8) & 0xFF));
    frame.push_back(uint8(headerLen & 0xFF));
    frame.insert(frame.end(), headerPb.Data().begin(), headerPb.Data().end());
    if (payload && payloadLen > 0)
        frame.insert(frame.end(), payload, payload + payloadLen);

    // 异步发送
    auto buf = std::make_shared<std::vector<uint8>>(std::move(frame));
    auto self = shared_from_this();
    std::lock_guard<std::mutex> lock(m_sendMutex);
    boost::asio::async_write(m_sslStream,
        boost::asio::buffer(buf->data(), buf->size()),
        [self, buf](const boost::system::error_code& ec, std::size_t /*written*/)
        {
            if (ec && ec != boost::asio::error::operation_aborted)
            {
                sLog.outError("[BNet] 发送帧失败 到 %s: %s", self->GetRemoteAddress().c_str(), ec.message().c_str());
                self->Close();
            }
        });
}
//End By leewheel
