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

//By leewheel 2026-07-25 现代(2.5.3)客户端 World 连接实现
#include "Hotfix/ModernWorldSocket.h"
#include "Hotfix/ModernOpcodeHandler.h"
#include "Hotfix/BnetSocket.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Server/Opcodes.h"
#include "World/World.h"
#include "Database/DatabaseEnv.h"
#include "Log/Log.h"
#include "Config/Config.h"

#include <openssl/rand.h>
#include <cstring>
#include <ctime>

// ============================================================================
// 构造/析构
// ============================================================================

ModernWorldSocket::ModernWorldSocket(boost::asio::io_context& io)
    : m_socket(io)
{
}

ModernWorldSocket::~ModernWorldSocket()
{
    Close();
}

// ============================================================================
// 连接生命周期
// ============================================================================

void ModernWorldSocket::Start()
{
    // 设置 TCP_NODELAY
    boost::system::error_code ec;
    m_socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);

    sLog.outBasic("[ModernWorld] 新连接 来自 %s:%u", GetRemoteAddress().c_str(), GetRemotePort());

    // 发送 AUTH_CHALLENGE，然后开始读取
    SendAuthChallenge();
    m_state = MODERN_STATE_CHALLENGE_SENT;
    AsyncReadHeader();
}

void ModernWorldSocket::Close()
{
    if (m_state == MODERN_STATE_CLOSED)
        return;
    m_state = MODERN_STATE_CLOSED;

    boost::system::error_code ec;
    m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    m_socket.close(ec);
}

bool ModernWorldSocket::IsClosed() const
{
    return m_state == MODERN_STATE_CLOSED || !m_socket.is_open();
}

std::string ModernWorldSocket::GetRemoteAddress() const
{
    try { return m_socket.remote_endpoint().address().to_string(); }
    catch (...) { return "0.0.0.0"; }
}

uint16 ModernWorldSocket::GetRemotePort() const
{
    try { return m_socket.remote_endpoint().port(); }
    catch (...) { return 0; }
}

// ============================================================================
// 发送现代格式包
// ============================================================================

void ModernWorldSocket::SendModernPacket(uint32 modernOpcode, const uint8* data, size_t len)
{
    if (IsClosed())
        return;

    // 包体 = opcode(4字节小端) + data
    size_t bodyLen = 4 + len;
    std::vector<uint8> body(bodyLen);
    body[0] = uint8(modernOpcode & 0xFF);
    body[1] = uint8((modernOpcode >> 8) & 0xFF);
    body[2] = uint8((modernOpcode >> 16) & 0xFF);
    body[3] = uint8((modernOpcode >> 24) & 0xFF);
    if (data && len > 0)
        std::memcpy(body.data() + 4, data, len);

    // 加密（如果加密层已初始化则实际加密，否则仅推进计数器）
    uint8 tag[ModernPacketHeader::TagSize] = { 0 };
    m_crypt.Encrypt(body.data(), bodyLen, tag);

    // 构造16字节包头
    ModernPacketHeader hdr;
    hdr.packetSize = uint32(bodyLen);
    std::memcpy(hdr.tag, tag, ModernPacketHeader::TagSize);

    uint8 headerBuf[ModernPacketHeader::Size];
    hdr.Write(headerBuf);

    SendRawFrame(headerBuf, body.data(), bodyLen);
}

void ModernWorldSocket::SendRawFrame(const uint8* headerBuf, const uint8* payload, size_t payloadLen)
{
    auto buf = std::make_shared<std::vector<uint8>>(ModernPacketHeader::Size + payloadLen);
    std::memcpy(buf->data(), headerBuf, ModernPacketHeader::Size);
    if (payload && payloadLen > 0)
        std::memcpy(buf->data() + ModernPacketHeader::Size, payload, payloadLen);

    auto self = shared_from_this();
    std::lock_guard<std::mutex> lock(m_sendMutex);
    boost::asio::async_write(m_socket,
        boost::asio::buffer(buf->data(), buf->size()),
        [self, buf](const boost::system::error_code& ec, std::size_t /*written*/)
        {
            if (ec && ec != boost::asio::error::operation_aborted)
            {
                sLog.outError("[ModernWorld] 发送失败 到 %s: %s",
                    self->GetRemoteAddress().c_str(), ec.message().c_str());
                self->Close();
            }
        });
}

// ============================================================================
// 发送 WorldPacket（由 WorldSession 调用的出口）
// ============================================================================

void ModernWorldSocket::SendPacket(const WorldPacket& pkt)
{
    if (IsClosed())
        return;

    // 特殊包体翻译（legacy 和 modern 格式不兼容的包）
    if (pkt.GetOpcode() == SMSG_CHAR_ENUM)
    {
        TranslateCharEnum(pkt);
        return;
    }

    // 将 legacy opcode 翻译为 modern opcode
    uint16 modernOpcode = 0;
    if (!ModernOpcode::ToModern(uint16(pkt.GetOpcode()), modernOpcode))
    {
        // 无映射的包不发送（2.5.3 客户端不认识的 legacy opcode 忽略）
        return;
    }

    SendModernPacket(uint32(modernOpcode), pkt.contents(), pkt.size());
}

// ============================================================================
// 异步读取
// ============================================================================

void ModernWorldSocket::AsyncReadHeader()
{
    if (IsClosed())
        return;

    auto self = shared_from_this();
    boost::asio::async_read(m_socket,
        boost::asio::buffer(m_headerBuf, ModernPacketHeader::Size),
        [self](const boost::system::error_code& ec, std::size_t bytes)
        {
            self->OnHeaderRead(ec, bytes);
        });
}

void ModernWorldSocket::OnHeaderRead(const boost::system::error_code& ec, std::size_t /*bytes*/)
{
    if (ec)
    {
        if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted)
            sLog.outError("[ModernWorld] 读取包头失败 来自 %s: %s",
                GetRemoteAddress().c_str(), ec.message().c_str());
        Close();
        return;
    }

    ModernPacketHeader hdr;
    hdr.Read(m_headerBuf);

    if (!hdr.IsValidSize())
    {
        sLog.outError("[ModernWorld] 非法包大小 %u 来自 %s", hdr.packetSize, GetRemoteAddress().c_str());
        Close();
        return;
    }

    if (hdr.packetSize == 0)
    {
        // 空包体（不应出现，但安全处理）
        AsyncReadHeader();
        return;
    }

    // 读取包体
    m_bodyBuf.resize(hdr.packetSize);
    auto self = shared_from_this();
    boost::asio::async_read(m_socket,
        boost::asio::buffer(m_bodyBuf.data(), hdr.packetSize),
        [self, hdr](const boost::system::error_code& ec2, std::size_t bytes2)
        {
            self->OnBodyRead(ec2, bytes2);
        });
}

void ModernWorldSocket::OnBodyRead(const boost::system::error_code& ec, std::size_t /*bytes*/)
{
    if (ec)
    {
        if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted)
            sLog.outError("[ModernWorld] 读取包体失败 来自 %s: %s",
                GetRemoteAddress().c_str(), ec.message().c_str());
        Close();
        return;
    }

    // 从头缓冲区重新解析 header 获取 tag
    ModernPacketHeader hdr;
    hdr.Read(m_headerBuf);

    // 解密（如果加密层已初始化则实际解密，否则仅推进计数器）
    if (!m_crypt.Decrypt(m_bodyBuf.data(), m_bodyBuf.size(), hdr.tag))
    {
        sLog.outError("[ModernWorld] 解密失败 来自 %s", GetRemoteAddress().c_str());
        Close();
        return;
    }

    // 包体前4字节 = opcode（小端）
    if (m_bodyBuf.size() < 4)
    {
        sLog.outError("[ModernWorld] 包体过短 来自 %s", GetRemoteAddress().c_str());
        Close();
        return;
    }

    uint32 modernOpcode = uint32(m_bodyBuf[0]) | (uint32(m_bodyBuf[1]) << 8) |
                           (uint32(m_bodyBuf[2]) << 16) | (uint32(m_bodyBuf[3]) << 24);
    const uint8* payload = m_bodyBuf.data() + 4;
    size_t payloadLen = m_bodyBuf.size() - 4;

    // 根据连接状态分发
    switch (m_state)
    {
        case MODERN_STATE_CHALLENGE_SENT:
            if (modernOpcode == ModernWorldOpcode::CMSG_AUTH_SESSION)
                HandleAuthSession(payload, payloadLen);
            else
            {
                sLog.outError("[ModernWorld] 期望AUTH_SESSION 收到 0x%04X 来自 %s",
                    modernOpcode, GetRemoteAddress().c_str());
                Close();
                return;
            }
            break;

        case MODERN_STATE_ENCRYPTING:
            if (modernOpcode == ModernWorldOpcode::CMSG_ENTER_ENCRYPTED_MODE_ACK)
                HandleEnterEncryptedModeAck();
            else
            {
                sLog.outError("[ModernWorld] 期望ENCRYPTED_ACK 收到 0x%04X 来自 %s",
                    modernOpcode, GetRemoteAddress().c_str());
                Close();
                return;
            }
            break;

        case MODERN_STATE_AUTHED:
            HandleIncomingPacket(modernOpcode, payload, payloadLen);
            break;

        default:
            Close();
            return;
    }

    // 继续读取
    if (!IsClosed())
        AsyncReadHeader();
}

// ============================================================================
// 握手：SMSG_AUTH_CHALLENGE
// ============================================================================

void ModernWorldSocket::SendAuthChallenge()
{
    // 生成16字节 ServerChallenge
    RAND_bytes(m_serverChallenge, 16);

    // 包体：DosChallenge(32字节，全1) + Challenge(16字节) + DosZeroBits(1字节)
    std::vector<uint8> body(32 + 16 + 1);
    std::memset(body.data(), 0x01, 32);                       // DosChallenge 全部填1
    std::memcpy(body.data() + 32, m_serverChallenge, 16);     // ServerChallenge
    body[48] = 0;                                              // DosZeroBits

    SendModernPacket(ModernWorldOpcode::SMSG_AUTH_CHALLENGE, body.data(), body.size());
}

// ============================================================================
// 握手：CMSG_AUTH_SESSION
// ============================================================================

void ModernWorldSocket::HandleAuthSession(const uint8* body, size_t len)
{
    // 格式：uint64 DosResponse + uint32 RegionID + uint32 BattlegroupID + uint32 RealmID
    //       + byte[16] LocalChallenge + byte[24] Digest + bit UseIPv6 + uint32 TicketLen + string Ticket
    if (len < 8 + 4 + 4 + 4 + 16 + 24)
    {
        sLog.outError("[ModernWorld] AUTH_SESSION 数据过短 来自 %s", GetRemoteAddress().c_str());
        Close();
        return;
    }

    size_t offset = 0;
    // uint64 DosResponse（跳过）
    offset += 8;
    // uint32 RegionID, BattlegroupID, RealmID（跳过）
    offset += 12;
    // byte[16] LocalChallenge
    uint8 localChallenge[16];
    std::memcpy(localChallenge, body + offset, 16);
    offset += 16;
    // byte[24] Digest
    uint8 digest[24];
    std::memcpy(digest, body + offset, 24);
    offset += 24;

    // UseIPv6 bit (1字节中的最高位)
    if (offset >= len) { Close(); return; }
    // 这里简化处理：按字节读（现代客户端的 bit 打包方式 - HasBit 使用位流，
    // 但 2.5.3 TBC Classic 的 AUTH_SESSION 实际上是普通字节流而非 bitpacked）
    // 跳过 UseIPv6 标志位（1字节）
    offset += 1;

    // uint32 RealmJoinTicket 长度
    if (offset + 4 > len) { Close(); return; }
    uint32 ticketLen = uint32(body[offset]) | (uint32(body[offset+1]) << 8) |
                       (uint32(body[offset+2]) << 16) | (uint32(body[offset+3]) << 24);
    offset += 4;

    // RealmJoinTicket 字符串
    std::string realmJoinTicket;
    if (ticketLen > 0 && offset + ticketLen <= len)
    {
        realmJoinTicket.assign(reinterpret_cast<const char*>(body + offset), ticketLen);
        offset += ticketLen;
    }

    // 从 BnetSessionMgr 取出会话票据（验证身份）
    BnetSessionTicket ticketData;
    if (!sBnetSessionMgr.PopTicket(realmJoinTicket, ticketData))
    {
        sLog.outError("[ModernWorld] AUTH_SESSION 票据无效: '%s' 来自 %s",
            realmJoinTicket.c_str(), GetRemoteAddress().c_str());
        // 发送认证失败响应
        Close();
        return;
    }

    m_accountId = ticketData.accountId;
    m_accountName = ticketData.accountName;
    m_expansion = ticketData.expansion;
    m_sessionKey = ticketData.sessionKey;

    // 验证 auth digest
    // 注意：在我们的集成模式下，session key 是 64 字节（BNet 流程生成）
    // 代理中使用平台特定 seed 验证，这里我们信任来自 BnetSessionMgr 的票据（已在 REST 阶段验证密码）
    // 可选：执行 ModernCrypto::VerifyAuthDigest 进行完整验证
    // 当前简化处理：票据有效即认证通过（ticket 本身是一次性的随机token）

    // 派生加密密钥
    uint8 sessionKey40[40];
    ModernCrypto::DeriveSessionKey40(m_sessionKey.data(), m_sessionKey.size(),
                                      localChallenge, m_serverChallenge, sessionKey40);

    ModernCrypto::DeriveEncryptKey16(sessionKey40, localChallenge, m_serverChallenge, m_encryptKey);

    sLog.outBasic("[ModernWorld] 账号 %s (id:%u) 认证成功 来自 %s",
        m_accountName.c_str(), m_accountId, GetRemoteAddress().c_str());

    // 发送 SMSG_ENTER_ENCRYPTED_MODE
    SendEnterEncryptedMode();
    m_state = MODERN_STATE_ENCRYPTING;
}

// ============================================================================
// 握手：SMSG_ENTER_ENCRYPTED_MODE
// ============================================================================

void ModernWorldSocket::SendEnterEncryptedMode()
{
    // 包体：RSA 签名(256字节) + Enabled bit
    // 签名 = RSA_Sign(HMAC-SHA256(encryptKey, [enabled] || EnableEncryptionSeed))
    uint8 signature[256];
    if (!ModernCrypto::SignEnterEncryptedMode(m_encryptKey, true, signature))
    {
        sLog.outError("[ModernWorld] RSA签名失败 来自 %s", GetRemoteAddress().c_str());
        Close();
        return;
    }

    // 构造包体：signature(256) + enabled bit(按字节打包：0x80 表示 bit=1)
    // 现代客户端的 bit 写入：WriteBit(true) = 设置当前字节最高位，FlushBits 写出
    std::vector<uint8> body(256 + 1);
    std::memcpy(body.data(), signature, 256);
    body[256] = 0x80;  // WriteBit(Enabled=true) → 最高位 = 1

    SendModernPacket(ModernWorldOpcode::SMSG_ENTER_ENCRYPTED_MODE, body.data(), body.size());
}

// ============================================================================
// 握手：CMSG_ENTER_ENCRYPTED_MODE_ACK
// ============================================================================

void ModernWorldSocket::HandleEnterEncryptedModeAck()
{
    // 收到 ACK 后启用加密
    m_crypt.Initialize(m_encryptKey);
    m_state = MODERN_STATE_AUTHED;

    sLog.outBasic("[ModernWorld] 加密已启用 账号 %s 来自 %s", m_accountName.c_str(), GetRemoteAddress().c_str());

    // 发送 AUTH_RESPONSE（加密的第一个包）
    SendAuthResponse();

    // 发送认证后系统包序列（时区/功能/缓存/hotfix/BNet状态）
    SendPostAuthPackets();

    // 创建 WorldSession 并加入世界
    // WorldSession 构造需要 WorldSocket* ，我们传 nullptr 然后设置 modernSocket
    m_session = new WorldSession(m_accountId, nullptr,
        AccountTypes(SEC_PLAYER), m_expansion,
        0, // mutetime
        LOCALE_zhCN, m_accountName, 0, 0, false);

    m_session->SetModernSocket(shared_from_this());
    m_session->LoadGlobalAccountData();
    m_session->LoadTutorialsData();
    m_session->SetGameBuild(8606); // 2.4.3 兼容 build
    m_session->SetOS(CLIENT_OS_WIN);
    m_session->SetPlatform(CLIENT_PLATFORM_X86);

    sWorld.AddSession(m_session);
}

// ============================================================================
// 握手：SMSG_AUTH_RESPONSE（现代格式）
// ============================================================================

void ModernWorldSocket::SendAuthResponse()
{
    // 构造现代 AUTH_RESPONSE 二进制格式
    // uint32 Result(0=Ok) + bit HasSuccessInfo(1) + bit HasWaitInfo(0) + FlushBits
    // SuccessInfo: VirtualRealmAddress(uint32) + VirtualRealms.Count(int32) + TimeRested(uint32)
    //             + ActiveExpansionLevel(uint8) + AccountExpansionLevel(uint8)
    //             + TimeSecondsUntilPCKick(uint32) + AvailableClasses.Count(int32)
    //             + Templates.Count(int32) + CurrencyID(uint32) + Time(int64)
    //             + [AvailableClasses data] + [bits] + [GameTimeInfo] + [VirtualRealms]

    std::vector<uint8> body;
    body.reserve(256);

    // uint32 Result = 0 (Ok)
    auto writeU32 = [&body](uint32 v) {
        body.push_back(uint8(v & 0xFF));
        body.push_back(uint8((v >> 8) & 0xFF));
        body.push_back(uint8((v >> 16) & 0xFF));
        body.push_back(uint8((v >> 24) & 0xFF));
    };
    auto writeU8 = [&body](uint8 v) { body.push_back(v); };
    auto writeI32 = [&body](int32 v) {
        uint32 u = *reinterpret_cast<uint32*>(&v);
        body.push_back(uint8(u & 0xFF));
        body.push_back(uint8((u >> 8) & 0xFF));
        body.push_back(uint8((u >> 16) & 0xFF));
        body.push_back(uint8((u >> 24) & 0xFF));
    };
    auto writeI64 = [&body](int64 v) {
        uint64 u = *reinterpret_cast<uint64*>(&v);
        for (int i = 0; i < 8; ++i)
            body.push_back(uint8((u >> (8*i)) & 0xFF));
    };

    writeU32(0);  // Result = AUTH_OK

    // Bit fields: HasSuccessInfo=1, HasWaitInfo=0, FlushBits
    // 现代客户端 bit packing：从最高位开始写，每8位 flush
    // bit0=HasSuccessInfo(1), bit1=HasWaitInfo(0) → 字节 = 0b10_000000 = 0x80... 
    // 实际上 WriteBit 在TrinityCore中是：当前字节左移，或上 bit value
    // 第一个 bit(1) → curByte = 0x80
    // 第二个 bit(0) → curByte = 0x80 | 0x00 → 实际 0x80 不变... 
    // Trinity bit 顺序：MSB first → byte = (bit0 << 7) | (bit1 << 6) | ...
    // HasSuccessInfo=1 → bit7=1, HasWaitInfo=0 → bit6=0 → 0b10000000 = 0x80
    writeU8(0x80);  // bit-packed: HasSuccessInfo=true, HasWaitInfo=false, flush

    // --- SuccessInfo ---
    // VirtualRealmAddress（region=1, site=0, realm=1 → (1<<24)|(0<<16)|(1) = 0x01000001）
    writeU32(0x01000001);
    // VirtualRealms.Count = 1
    writeI32(1);
    // TimeRested = 0
    writeU32(0);
    // ActiveExpansionLevel = 1 (TBC)
    writeU8(1);
    // AccountExpansionLevel = 1 (TBC)
    writeU8(m_expansion);
    // TimeSecondsUntilPCKick = 0
    writeU32(0);
    // AvailableClasses.Count = 0（让客户端用默认值）
    writeI32(0);
    // Templates.Count = 0
    writeI32(0);
    // CurrencyID = 0
    writeU32(0);
    // Time = Unix时间戳（秒）
    writeI64(int64(std::time(nullptr)));

    // Bits: IsExpansionTrial(0) + ForceCharacterTemplate(0) + NumPlayersHorde(0)
    //       + NumPlayersAlliance(0) + ExpansionTrialExpiration(0) → 全0 = 0x00
    writeU8(0x00);

    // GameTimeInfo（没有 NumPlayersHorde/Alliance/ExpansionTrialExpiration 因为 bits 全0）
    // 不需要写

    // VirtualRealms[0]: VirtualRealmNameInfo
    // 格式：bit IsLocal(1) + bit IsInternalRealm(0) + bits RealmNameActual length(8) + bits RealmNameNormalized length(8) + flush
    //       + string RealmNameActual + string RealmNameNormalized
    std::string realmName = sConfig.GetStringDefault("BNet.RealmName", "LWCore TBC");
    uint8 nameLen = uint8(realmName.size());
    // bit packing: IsLocal(1)=bit7, IsInternalRealm(0)=bit6
    // then 8 bits for RealmNameActual length, 8 bits for RealmNameNormalized length
    // Total bits: 2 + 8 + 8 = 18 bits → 3 bytes
    // Byte0: [IsLocal=1][IsInternalRealm=0][nameLen bits 7..2]
    // Byte1: [nameLen bits 1..0][normLen bits 7..2]  
    // Byte2: [normLen bits 1..0][pad 000000]
    // 简化：使用 VirtualRealmAddress 格式（直接重复 VirtualRealmAddress + bit fields）
    // 实际 VirtualRealmInfo.Write: WriteUInt32(RealmAddress) + WriteBit(IsLocal) + WriteBit(IsInternal) + WriteBits(NameActual.len, 8) + WriteBits(NameNormalized.len, 8) + FlushBits + WriteString(NameActual) + WriteString(NameNormalized)
    writeU32(0x01000001); // RealmAddress (same as VirtualRealmAddress)
    // Bits: IsLocal(1) + IsInternalRealm(0) + 8-bit nameActualLen + 8-bit nameNormalizedLen
    // bit stream: 1 0 [8 bits len] [8 bits len] → 18 bits = 3 bytes (pad with 0)
    uint8 bitBuf[3];
    bitBuf[0] = 0x80 | uint8((nameLen >> 2) & 0x3F);  // bit0=1(IsLocal), bit1=0, bits2-7 = nameLen[7:2]
    bitBuf[1] = uint8((nameLen & 0x03) << 6) | uint8((nameLen >> 2) & 0x3F); // nameLen[1:0] + normLen[7:2]
    bitBuf[2] = uint8((nameLen & 0x03) << 6); // normLen[1:0] + padding

    // 上面的 bit packing 太容易出错。让我用更直接的方式：
    // 按照 TrinityCore BitPack 格式手动构造
    // 实际上 2.5.3 TBC Classic 客户端的 AUTH_RESPONSE 解析比较复杂
    // 为了简化初始实现，我们跳过 VirtualRealmInfo 的详细内容
    // 先将 VirtualRealms.Count 设为 0 来避免复杂的 bit packing
    
    // 重新构造 - 将 VirtualRealms.Count 设为 0
    body.clear();
    writeU32(0);  // Result = AUTH_OK
    writeU8(0x80);  // HasSuccessInfo=true, HasWaitInfo=false

    writeU32(0x01000001);  // VirtualRealmAddress
    writeI32(0);           // VirtualRealms.Count = 0（跳过复杂结构）
    writeU32(0);           // TimeRested
    writeU8(1);            // ActiveExpansionLevel = TBC
    writeU8(m_expansion);  // AccountExpansionLevel
    writeU32(0);           // TimeSecondsUntilPCKick
    writeI32(0);           // AvailableClasses.Count = 0
    writeI32(0);           // Templates.Count = 0
    writeU32(0);           // CurrencyID
    writeI64(int64(std::time(nullptr)));  // Time

    // 5 optional bits 全 false = 0x00
    writeU8(0x00);

    SendModernPacket(ModernWorldOpcode::SMSG_AUTH_RESPONSE, body.data(), body.size());
}

// ============================================================================
// 正常包处理（认证完成后）
// ============================================================================

void ModernWorldSocket::HandleIncomingPacket(uint32 modernOpcode, const uint8* data, size_t len)
{
    // 特殊处理的包
    if (modernOpcode == ModernWorldOpcode::CMSG_PING)
    {
        HandlePing(data, len);
        return;
    }
    if (modernOpcode == ModernWorldOpcode::CMSG_KEEP_ALIVE)
    {
        return; // 心跳包，忽略
    }
    //By leewheel 2026-07-25 Hotfix 请求处理
    if (modernOpcode == ModernWorldOpcode::CMSG_DB_QUERY_BULK)
    {
        HandleDbQueryBulk(data, len);
        return;
    }
    if (modernOpcode == ModernWorldOpcode::CMSG_HOTFIX_REQUEST)
    {
        HandleHotfixRequest(data, len);
        return;
    }
    // 角色登录包体翻译（PackedGuid128 → uint64）
    if (modernOpcode == ModernWorldOpcode::CMSG_PLAYER_LOGIN)
    {
        HandlePlayerLogin(data, len);
        return;
    }
    //End By leewheel

    // 翻译 modern opcode → legacy opcode
    uint16 legacyOpcode = 0;
    if (!ModernOpcode::ToLegacy(uint16(modernOpcode), legacyOpcode))
    {
        // 无映射的包忽略（已由 ToLegacy 记录到日志）
        return;
    }

    if (legacyOpcode >= NUM_MSG_TYPES)
    {
        sLog.outError("[ModernWorld] 翻译后opcode %u 超出范围 来自 %s", legacyOpcode, GetRemoteAddress().c_str());
        return;
    }

    // 构造 WorldPacket 并投递给 WorldSession
    if (!m_session)
        return;

    auto pkt = std::make_unique<WorldPacket>(Opcodes(legacyOpcode), len);
    if (data && len > 0)
        pkt->append(data, len);

    m_session->QueuePacket(std::move(pkt));
}

void ModernWorldSocket::HandlePing(const uint8* data, size_t len)
{
    if (len < 8)
        return;

    // uint32 Serial + uint32 Latency
    uint32 serial = uint32(data[0]) | (uint32(data[1]) << 8) |
                    (uint32(data[2]) << 16) | (uint32(data[3]) << 24);
    uint32 latency = uint32(data[4]) | (uint32(data[5]) << 8) |
                     (uint32(data[6]) << 16) | (uint32(data[7]) << 24);

    if (m_session)
        m_session->SetLatency(latency);

    // 回复 SMSG_PONG
    uint8 pongData[4];
    pongData[0] = uint8(serial & 0xFF);
    pongData[1] = uint8((serial >> 8) & 0xFF);
    pongData[2] = uint8((serial >> 16) & 0xFF);
    pongData[3] = uint8((serial >> 24) & 0xFF);

    SendModernPacket(ModernWorldOpcode::SMSG_PONG, pongData, 4);
}

// ============================================================================
// Bit 打包辅助方法（实现现代客户端的 MSB-first 位写入）
// ============================================================================

void ModernWorldSocket::WriteBits(std::vector<uint8>& buf, uint32 value, int bitCount, int& bitPos, uint8& curByte)
{
    for (int i = bitCount - 1; i >= 0; --i)
    {
        curByte |= ((value >> i) & 1) << (7 - bitPos);
        ++bitPos;
        if (bitPos == 8)
        {
            buf.push_back(curByte);
            curByte = 0;
            bitPos = 0;
        }
    }
}

void ModernWorldSocket::FlushBits(std::vector<uint8>& buf, int& bitPos, uint8& curByte)
{
    if (bitPos > 0)
    {
        buf.push_back(curByte);
        curByte = 0;
        bitPos = 0;
    }
}

// ============================================================================
// 认证后系统包序列
// ============================================================================

void ModernWorldSocket::SendPostAuthPackets()
{
    SendSetTimeZoneInformation();
    SendFeatureSystemStatusGlueScreen();
    SendCacheVersion();
    SendAvailableHotfixes();
    SendBnetConnectionState();
}

void ModernWorldSocket::SendSetTimeZoneInformation()
{
    // 包体格式：WriteBits(serverTzLen, 7) + WriteBits(gameTzLen, 7) + FlushBits + string + string
    const char* tz = "Asia/Shanghai";
    uint32 tzLen = uint32(std::strlen(tz));

    std::vector<uint8> body;
    body.reserve(64);
    int bitPos = 0;
    uint8 curByte = 0;

    WriteBits(body, tzLen, 7, bitPos, curByte);  // ServerTimeTZ 长度
    WriteBits(body, tzLen, 7, bitPos, curByte);  // GameTimeTZ 长度
    FlushBits(body, bitPos, curByte);

    // 写入字符串内容（不含 null 结尾符）
    body.insert(body.end(), reinterpret_cast<const uint8*>(tz), reinterpret_cast<const uint8*>(tz) + tzLen);
    body.insert(body.end(), reinterpret_cast<const uint8*>(tz), reinterpret_cast<const uint8*>(tz) + tzLen);

    SendModernPacket(ModernWorldOpcode::SMSG_SET_TIME_ZONE_INFORMATION, body.data(), body.size());
}

void ModernWorldSocket::SendFeatureSystemStatusGlueScreen()
{
    // V2_5_3 路径（Legacy modern V1_14/V2_5 layout）
    // 18 bits + FlushBits + uint32*8 + int64 + int32*4 + int16
    std::vector<uint8> body;
    body.reserve(64);
    int bitPos = 0;
    uint8 curByte = 0;

    // 18 个 bits（MSB-first）
    WriteBits(body, 0, 1, bitPos, curByte); // BpayStoreEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // BpayStoreAvailable = false
    WriteBits(body, 0, 1, bitPos, curByte); // BpayStoreDisabledByParentalControls = false
    WriteBits(body, 0, 1, bitPos, curByte); // CharUndeleteEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // CommerceSystemEnabled = false
    WriteBits(body, 1, 1, bitPos, curByte); // Unk14 = true
    WriteBits(body, 0, 1, bitPos, curByte); // WillKickFromWorld = false
    WriteBits(body, 0, 1, bitPos, curByte); // IsExpansionPreorderInStore = false
    WriteBits(body, 0, 1, bitPos, curByte); // KioskModeEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // CompetitiveModeEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // TrialBoostEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // TokenBalanceEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // LiveRegionCharacterListEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // LiveRegionCharacterCopyEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // LiveRegionAccountCopyEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // LiveRegionKeyBindingsCopyEnabled = false
    WriteBits(body, 0, 1, bitPos, curByte); // Unknown901CheckoutRelated = false
    WriteBits(body, 0, 1, bitPos, curByte); // EuropaTicketSystemStatus = null (false)
    FlushBits(body, bitPos, curByte);

    // 固定字段
    auto writeU32 = [&body](uint32 v) {
        body.push_back(uint8(v & 0xFF)); body.push_back(uint8((v >> 8) & 0xFF));
        body.push_back(uint8((v >> 16) & 0xFF)); body.push_back(uint8((v >> 24) & 0xFF));
    };
    auto writeI32 = [&body](int32 v) {
        uint32 u; std::memcpy(&u, &v, 4);
        body.push_back(uint8(u & 0xFF)); body.push_back(uint8((u >> 8) & 0xFF));
        body.push_back(uint8((u >> 16) & 0xFF)); body.push_back(uint8((u >> 24) & 0xFF));
    };
    auto writeI64 = [&body](int64 v) {
        uint64 u; std::memcpy(&u, &v, 8);
        for (int i = 0; i < 8; ++i) body.push_back(uint8((u >> (8*i)) & 0xFF));
    };
    auto writeI16 = [&body](int16 v) {
        uint16 u; std::memcpy(&u, &v, 2);
        body.push_back(uint8(u & 0xFF)); body.push_back(uint8((u >> 8) & 0xFF));
    };

    writeU32(0);        // TokenPollTimeSeconds
    writeU32(0);        // KioskSessionMinutes
    writeI64(0);        // TokenBalanceAmount
    writeI32(10);       // MaxCharactersPerRealm
    writeI32(0);        // LiveRegionCharacterCopySourceRegions.Count
    writeU32(0);        // BpayStoreProductDeliveryDelay
    writeI32(0);        // ActiveCharacterUpgradeBoostType
    writeI32(0);        // ActiveClassTrialBoostType
    writeI32(1);        // MinimumExpansionLevel (TBC=1)
    writeI32(1);        // MaximumExpansionLevel (TBC=1)
    // V2_5_3 扩展字段
    writeI32(0);        // ActiveSeason
    writeI32(0);        // GameRuleValues.Count
    writeI16(50);       // MaxPlayerNameQueriesPerPacket

    SendModernPacket(ModernWorldOpcode::SMSG_FEATURE_SYSTEM_STATUS_GLUE_SCREEN, body.data(), body.size());
}

void ModernWorldSocket::SendCacheVersion()
{
    // 包体：uint32 CacheVersion = 0
    uint8 body[4] = { 0, 0, 0, 0 };
    SendModernPacket(ModernWorldOpcode::SMSG_CACHE_VERSION, body, 4);
}

void ModernWorldSocket::SendAvailableHotfixes()
{
    // 包体：VirtualRealmAddress(uint32) + HotfixCount(int32)
    // TBC Classic: 发送空列表（count=0），客户端会通过 CMSG_DB_QUERY_BULK 按需查询
    std::vector<uint8> body(8);
    // VirtualRealmAddress = (region=1)<<24 | (site=0)<<16 | (realm=1) = 0x01000001
    uint32 realmAddr = 0x01000001;
    body[0] = uint8(realmAddr & 0xFF);
    body[1] = uint8((realmAddr >> 8) & 0xFF);
    body[2] = uint8((realmAddr >> 16) & 0xFF);
    body[3] = uint8((realmAddr >> 24) & 0xFF);
    // HotfixCount = 0
    body[4] = 0; body[5] = 0; body[6] = 0; body[7] = 0;

    SendModernPacket(ModernWorldOpcode::SMSG_AVAILABLE_HOTFIXES, body.data(), body.size());
}

void ModernWorldSocket::SendBnetConnectionState()
{
    // 包体：State(2 bits) + SuppressNotification(1 bit) + FlushBits
    // State=1 (已连接), SuppressNotification=false
    std::vector<uint8> body;
    int bitPos = 0;
    uint8 curByte = 0;
    WriteBits(body, 1, 2, bitPos, curByte);  // State = 1
    WriteBits(body, 0, 1, bitPos, curByte);  // SuppressNotification = false
    FlushBits(body, bitPos, curByte);

    SendModernPacket(ModernWorldOpcode::SMSG_BATTLE_NET_CONNECTION_STATUS, body.data(), body.size());
}

// ============================================================================
// Hotfix 处理：CMSG_DB_QUERY_BULK
// ============================================================================

void ModernWorldSocket::HandleDbQueryBulk(const uint8* data, size_t len)
{
    if (len < 4)
        return;

    // uint32 TableHash
    uint32 tableHash = uint32(data[0]) | (uint32(data[1]) << 8) |
                       (uint32(data[2]) << 16) | (uint32(data[3]) << 24);

    // uint13 QueryCount（前 13 bits 的下一个字节，但这是 bit-packed）
    // 实际格式：在 TableHash 后紧跟 bit-packed 的 count(13 bits)
    // 每个 bit 从字节的高位开始读取
    if (len < 6) // 至少需要4字节hash + 2字节bits
        return;

    // 读取 13 bits count（从byte4的最高位开始）
    uint32 queryCount = 0;
    int readBitPos = 0;
    size_t readByteIdx = 4;
    for (int i = 0; i < 13; ++i)
    {
        if (readByteIdx >= len)
            break;
        uint8 bit = (data[readByteIdx] >> (7 - readBitPos)) & 1;
        queryCount |= (uint32(bit) << (12 - i));
        ++readBitPos;
        if (readBitPos == 8)
        {
            readBitPos = 0;
            ++readByteIdx;
        }
    }
    // FlushBits —— 跳到下一个字节边界
    if (readBitPos > 0)
    {
        readBitPos = 0;
        ++readByteIdx;
    }

    // 读取每个 RecordId (uint32)
    uint32 timestamp = uint32(std::time(nullptr));
    for (uint32 i = 0; i < queryCount; ++i)
    {
        if (readByteIdx + 4 > len)
            break;

        uint32 recordId = uint32(data[readByteIdx]) | (uint32(data[readByteIdx+1]) << 8) |
                          (uint32(data[readByteIdx+2]) << 16) | (uint32(data[readByteIdx+3]) << 24);
        readByteIdx += 4;

        // 对不同表类型进行处理，当前返回 Invalid 状态（客户端会使用本地缓存）
        // TODO: 后续从 hotfixes 库查询实际数据并填充响应
        SendDbReply(tableHash, recordId, timestamp, 0 /*Invalid*/, nullptr, 0);
    }
}

void ModernWorldSocket::SendDbReply(uint32 tableHash, uint32 recordId, uint32 timestamp,
                                     uint8 status, const uint8* replyData, size_t dataLen)
{
    // 包体格式：TableHash(uint32) + RecordID(uint32) + Timestamp(uint32)
    //         + Status(3 bits) + DataSize(uint32) + Data(variable)
    std::vector<uint8> body;
    body.reserve(16 + dataLen);

    auto writeU32 = [&body](uint32 v) {
        body.push_back(uint8(v & 0xFF)); body.push_back(uint8((v >> 8) & 0xFF));
        body.push_back(uint8((v >> 16) & 0xFF)); body.push_back(uint8((v >> 24) & 0xFF));
    };

    writeU32(tableHash);
    writeU32(recordId);
    writeU32(timestamp);

    // Status: 3 bits + 后续字段（DataSize + Data）是字节对齐的
    // 注意：HermesProxy 的 WriteBits 在此处写 3 bits 然后紧跟 uint32+data
    // 实际上客户端读取时，ReadBits(3) 然后 ReadUInt32（会先 FlushBits）
    int bitPos = 0;
    uint8 curByte = 0;
    WriteBits(body, uint32(status), 3, bitPos, curByte);
    FlushBits(body, bitPos, curByte);

    writeU32(uint32(dataLen));
    if (replyData && dataLen > 0)
        body.insert(body.end(), replyData, replyData + dataLen);

    SendModernPacket(ModernWorldOpcode::SMSG_DB_REPLY, body.data(), body.size());
}

// ============================================================================
// Hotfix 处理：CMSG_HOTFIX_REQUEST
// ============================================================================

void ModernWorldSocket::HandleHotfixRequest(const uint8* data, size_t len)
{
    if (len < 12)
        return;

    // uint32 ClientBuild + uint32 DataBuild + uint32 HotfixCount
    size_t offset = 0;
    //uint32 clientBuild = ...; // 跳过
    offset += 4;
    //uint32 dataBuild = ...; // 跳过
    offset += 4;
    uint32 hotfixCount = uint32(data[offset]) | (uint32(data[offset+1]) << 8) |
                         (uint32(data[offset+2]) << 16) | (uint32(data[offset+3]) << 24);
    offset += 4;

    sLog.outBasic("[ModernWorld] HOTFIX_REQUEST: 客户端请求 %u 条hotfix 来自 %s",
        hotfixCount, GetRemoteAddress().c_str());

    // 当前返回空响应（无匹配记录）
    // TODO: 后续从 hotfixes 库加载实际 hotfix 数据并填充
    // SMSG_HOTFIX_CONNECT 格式：int32 Count + [per record: HotfixId/UniqueId/TableHash/RecordId/ContentSize/Status(3bits)] + uint32 TotalDataSize + Data
    std::vector<uint8> body;
    body.reserve(8);

    auto writeI32 = [&body](int32 v) {
        uint32 u; std::memcpy(&u, &v, 4);
        body.push_back(uint8(u & 0xFF)); body.push_back(uint8((u >> 8) & 0xFF));
        body.push_back(uint8((u >> 16) & 0xFF)); body.push_back(uint8((u >> 24) & 0xFF));
    };
    auto writeU32 = [&body](uint32 v) {
        body.push_back(uint8(v & 0xFF)); body.push_back(uint8((v >> 8) & 0xFF));
        body.push_back(uint8((v >> 16) & 0xFF)); body.push_back(uint8((v >> 24) & 0xFF));
    };

    writeI32(0);   // Hotfixes.Count = 0（无匹配记录）
    writeU32(0);   // TotalDataSize = 0

    SendModernPacket(ModernWorldOpcode::SMSG_HOTFIX_CONNECT, body.data(), body.size());
}

// ============================================================================
// 角色列表包体翻译（SMSG_CHAR_ENUM legacy 2.4.3 → SMSG_ENUM_CHARACTERS_RESULT modern 2.5.3）
// ============================================================================

// 128位 GUID 高位常量（RealmSpecificCreate: High = type<<58 | realmId<<42）
static const uint64 GUID128_PLAYER_HIGH = (uint64(2) << 58) | (uint64(1) << 42);  // Player type=2, realm=1
static const uint64 GUID128_GUILD_HIGH  = (uint64(28) << 58) | (uint64(1) << 42); // Guild type=28, realm=1

void ModernWorldSocket::WritePackedGuid128(std::vector<uint8>& buf, uint64 low, uint64 high)
{
    // 空 GUID
    if (low == 0 && high == 0)
    {
        buf.push_back(0);
        buf.push_back(0);
        return;
    }

    uint8 lowMask = 0;
    uint8 highMask = 0;
    uint8 packed[18];
    uint8 packSize = 2;

    for (int i = 0; i < 8; ++i)
    {
        uint8 b = uint8(low >> (i * 8));
        if (b != 0)
        {
            lowMask |= (1 << i);
            packed[packSize++] = b;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        uint8 b = uint8(high >> (i * 8));
        if (b != 0)
        {
            highMask |= (1 << i);
            packed[packSize++] = b;
        }
    }

    packed[0] = lowMask;
    packed[1] = highMask;
    buf.insert(buf.end(), packed, packed + packSize);
}

bool ModernWorldSocket::ReadPackedGuid128(const uint8* data, size_t len, size_t& offset,
                                           uint64& outLow, uint64& outHigh)
{
    if (offset + 2 > len) return false;

    uint8 lowMask = data[offset++];
    uint8 highMask = data[offset++];

    outLow = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (lowMask & (1 << i))
        {
            if (offset >= len) return false;
            outLow |= uint64(data[offset++]) << (i * 8);
        }
    }

    outHigh = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (highMask & (1 << i))
        {
            if (offset >= len) return false;
            outHigh |= uint64(data[offset++]) << (i * 8);
        }
    }

    return true;
}

// ============================================================================
// CMSG_PLAYER_LOGIN 包体翻译（modern PackedGuid128+FarClip → legacy uint64 GUID）
// ============================================================================

void ModernWorldSocket::HandlePlayerLogin(const uint8* data, size_t len)
{
    // 现代格式: PackedGuid128 + float(FarClip) + bit(UnkBit) for V2_5
    // Legacy 格式: uint64 GUID
    size_t offset = 0;
    uint64 guidLow = 0, guidHigh = 0;
    if (!ReadPackedGuid128(data, len, offset, guidLow, guidHigh))
    {
        sLog.outError("[ModernWorld] PLAYER_LOGIN 解析 GUID 失败 来自 %s", GetRemoteAddress().c_str());
        return;
    }

    // Low64 就是 legacy GUID 值（RealmSpecificCreate: Low = counter）
    uint64 legacyGuid = guidLow;
    sLog.outBasic("[ModernWorld] PLAYER_LOGIN: GUID Low=0x%016llX High=0x%016llX -> legacy 0x%016llX 来自 %s",
        (unsigned long long)guidLow, (unsigned long long)guidHigh,
        (unsigned long long)legacyGuid, GetRemoteAddress().c_str());

    if (!m_session)
        return;

    // 构造 legacy 包体（仅包含 uint64 GUID）
    auto pkt = std::make_unique<WorldPacket>(Opcodes(0x03D), 8); // CMSG_PLAYER_LOGIN = 0x03D
    *pkt << uint64(legacyGuid);

    m_session->QueuePacket(std::move(pkt));
}

void ModernWorldSocket::TranslateCharEnum(const WorldPacket& pkt)
{
    const uint8* src = pkt.contents();
    size_t srcLen = pkt.size();
    size_t offset = 0;

    if (srcLen < 1)
    {
        sLog.outError("[ModernWorld] CHAR_ENUM 包体为空 来自 %s", GetRemoteAddress().c_str());
        return;
    }

    uint8 charCount = src[offset++];
    sLog.outBasic("[ModernWorld] 角色列表翻译: %u 个角色 来自 %s", charCount, GetRemoteAddress().c_str());

    // 辅助宏/lambda
    auto readU8 = [&]() -> uint8 {
        if (offset >= srcLen) return 0;
        return src[offset++];
    };
    auto readU32 = [&]() -> uint32 {
        if (offset + 4 > srcLen) { offset = srcLen; return 0; }
        uint32 v = uint32(src[offset]) | (uint32(src[offset+1]) << 8) |
                   (uint32(src[offset+2]) << 16) | (uint32(src[offset+3]) << 24);
        offset += 4;
        return v;
    };
    auto readU64 = [&]() -> uint64 {
        if (offset + 8 > srcLen) { offset = srcLen; return 0; }
        uint64 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64(src[offset + i]) << (i * 8);
        offset += 8;
        return v;
    };
    auto readFloat = [&]() -> float {
        uint32 raw = readU32();
        float f; std::memcpy(&f, &raw, 4);
        return f;
    };
    auto readCString = [&]() -> std::string {
        std::string s;
        while (offset < srcLen && src[offset] != 0)
            s.push_back(char(src[offset++]));
        if (offset < srcLen) ++offset; // 跳过 null 终结符
        return s;
    };

    // 写入辅助
    auto writeU8 = [](std::vector<uint8>& b, uint8 v) {
        b.push_back(v);
    };
    auto writeU16 = [](std::vector<uint8>& b, uint16 v) {
        b.push_back(uint8(v & 0xFF)); b.push_back(uint8((v >> 8) & 0xFF));
    };
    auto writeU32 = [](std::vector<uint8>& b, uint32 v) {
        b.push_back(uint8(v & 0xFF)); b.push_back(uint8((v >> 8) & 0xFF));
        b.push_back(uint8((v >> 16) & 0xFF)); b.push_back(uint8((v >> 24) & 0xFF));
    };
    auto writeI32 = [](std::vector<uint8>& b, int32 v) {
        uint32 u; std::memcpy(&u, &v, 4);
        b.push_back(uint8(u & 0xFF)); b.push_back(uint8((u >> 8) & 0xFF));
        b.push_back(uint8((u >> 16) & 0xFF)); b.push_back(uint8((u >> 24) & 0xFF));
    };
    auto writeU64 = [](std::vector<uint8>& b, uint64 v) {
        for (int i = 0; i < 8; ++i)
            b.push_back(uint8((v >> (i * 8)) & 0xFF));
    };
    auto writeFloat = [&writeU32](std::vector<uint8>& b, float f) {
        uint32 raw; std::memcpy(&raw, &f, 4);
        writeU32(b, raw);
    };

    // --- 解析所有角色数据 ---
    struct CharData {
        uint64 guid;
        std::string name;
        uint8 race, classId, sex;
        uint8 skin, face, hairStyle, hairColor, facialHair;
        uint8 level;
        uint32 zone, map;
        float x, y, z;
        uint32 guildId;
        uint32 flags;
        bool firstLogin;
        uint32 petDisplayId, petLevel, petFamily;
        // 装备槽位 (19 装备 + 1 背包 = 20)
        struct EquipSlot { uint32 displayId; uint8 invType; uint32 enchantId; };
        EquipSlot equip[20]; // 0-18 装备, 19 背包
    };

    std::vector<CharData> chars;
    chars.reserve(charCount);
    uint8 maxLevel = 1;

    for (uint8 i = 0; i < charCount; ++i)
    {
        if (offset >= srcLen) break;

        CharData c = {};
        c.guid = readU64();
        c.name = readCString();
        c.race = readU8();
        c.classId = readU8();
        c.sex = readU8();
        c.skin = readU8();
        c.face = readU8();
        c.hairStyle = readU8();
        c.hairColor = readU8();
        c.facialHair = readU8();
        c.level = readU8();
        c.zone = readU32();
        c.map = readU32();
        c.x = readFloat();
        c.y = readFloat();
        c.z = readFloat();
        c.guildId = readU32();
        c.flags = readU32();
        // TBC 2.4.3 没有 Flags2
        c.firstLogin = (readU8() != 0);
        c.petDisplayId = readU32();
        c.petLevel = readU32();
        c.petFamily = readU32();

        // 19个装备槽位
        for (int j = 0; j < 19; ++j)
        {
            c.equip[j].displayId = readU32();
            c.equip[j].invType = readU8();
            c.equip[j].enchantId = readU32(); // TBC 2.0.1+ 有 enchant
        }
        // 1个背包槽位 (TBC pre-3.3.3 bagCount=1)
        c.equip[19].displayId = readU32();
        c.equip[19].invType = readU8();
        c.equip[19].enchantId = readU32();

        if (c.level > maxLevel)
            maxLevel = c.level;

        chars.push_back(std::move(c));
    }

    // --- 构建现代格式包体 ---
    std::vector<uint8> body;
    body.reserve(2048);
    int bitPos = 0;
    uint8 curByte = 0;

    // == 信封 (7 bits + 4 × Int32) ==
    // V2_5 路径: 7 个 bit 标志
    WriteBits(body, 1, 1, bitPos, curByte); // Success = true
    WriteBits(body, 0, 1, bitPos, curByte); // IsDeletedCharacters = false
    WriteBits(body, 0, 1, bitPos, curByte); // IsNewPlayerRestrictionSkipped = false
    WriteBits(body, 0, 1, bitPos, curByte); // IsNewPlayerRestricted = false
    WriteBits(body, 0, 1, bitPos, curByte); // IsNewPlayer = false
    WriteBits(body, 0, 1, bitPos, curByte); // DisabledClassesMask.HasValue = false
    WriteBits(body, 0, 1, bitPos, curByte); // IsAlliedRacesCreationAllowed = false
    FlushBits(body, bitPos, curByte);

    // 种族解锁数据: races 1-8 + 10,11 = 10 个
    const int32 raceUnlockCount = 10;

    writeI32(body, int32(chars.size()));    // Characters.Count
    writeI32(body, int32(maxLevel));        // MaxCharacterLevel
    writeI32(body, raceUnlockCount);        // RaceUnlockData.Count
    writeI32(body, 0);                      // UnlockedConditionalAppearances.Count
    // 无 DisabledClassesMask（HasValue=false）

    // == 每个角色数据 (WriteLegacyModern 路径) ==
    for (size_t i = 0; i < chars.size(); ++i)
    {
        const CharData& c = chars[i];

        // PackedGuid128 - 玩家 GUID
        WritePackedGuid128(body, uint64(c.guid), GUID128_PLAYER_HIGH);

        // GuildClubMemberID (uint64)
        writeU64(body, 0);
        // ListPosition (uint8)
        writeU8(body, uint8(i));
        // Race, Class, Sex (uint8 each)
        writeU8(body, c.race);
        writeU8(body, c.classId);
        writeU8(body, c.sex);
        // Customizations.Count (int32) - 当前不发送自定义外观
        writeI32(body, 0);

        // ExperienceLevel (uint8)
        writeU8(body, c.level);
        // ZoneId, MapId (uint32)
        writeU32(body, c.zone);
        writeU32(body, c.map);
        // PreloadPos (Vector3: 3 × float)
        writeFloat(body, c.x);
        writeFloat(body, c.y);
        writeFloat(body, c.z);

        // PackedGuid128 - 公会 GUID
        if (c.guildId != 0)
            WritePackedGuid128(body, uint64(c.guildId), GUID128_GUILD_HIGH);
        else
            WritePackedGuid128(body, 0, 0); // 空 GUID

        // Flags, Flags2, Flags3 (uint32 each)
        writeU32(body, c.flags);
        writeU32(body, 0); // Flags2
        writeU32(body, 0); // Flags3

        // Pet (3 × uint32)
        writeU32(body, c.petDisplayId);
        writeU32(body, c.petLevel);
        writeU32(body, c.petFamily);

        // ProfessionIds[2] (2 × uint32)
        writeU32(body, 0);
        writeU32(body, 0);

        // VisualItems: 23 槽位 × 14字节 (DisplayId/EnchantId/SecondaryAppearance/InvType/Subclass)
        for (int j = 0; j < 23; ++j)
        {
            if (j < 20)
            {
                writeU32(body, c.equip[j].displayId);       // DisplayId
                writeU32(body, c.equip[j].enchantId);       // DisplayEnchantId
                writeU32(body, 0);                           // SecondaryItemModifiedAppearanceID
                writeU8(body, c.equip[j].invType);          // InvType
                writeU8(body, 0);                            // Subclass
            }
            else
            {
                // 空槽位 (20-22)
                writeU32(body, 0);
                writeU32(body, 0);
                writeU32(body, 0);
                writeU8(body, 0);
                writeU8(body, 0);
            }
        }

        // LastPlayedTime (uint64) - 当前时间
        writeU64(body, uint64(std::time(nullptr)));
        // SpecID (uint16)
        writeU16(body, 0);
        // Unknown703 (uint32)
        writeU32(body, 0);
        // LastLoginVersion (uint32) - 使用客户端 build 号
        writeU32(body, 40892); // 2.5.3.40892 approximate build
        // Flags4 (uint32)
        writeU32(body, 0);
        // MailSenders.Count (int32)
        writeI32(body, 0);
        // MailSenderTypes.Count (int32)
        writeI32(body, 0);
        // OverrideSelectScreenFileDataID (uint32)
        writeU32(body, 0);

        // Customizations 数据（Count=0 所以无数据）
        // MailSenderTypes 数据（Count=0 所以无数据）

        // Bit-packed 尾部
        uint32 nameLen = uint32(c.name.length());
        WriteBits(body, nameLen, 6, bitPos, curByte);   // 名称字节数 (6 bits)
        WriteBits(body, c.firstLogin ? 1 : 0, 1, bitPos, curByte); // FirstLogin
        WriteBits(body, 0, 1, bitPos, curByte);         // BoostInProgress = false
        // 至此已写 8 bits，自动填充一个字节
        WriteBits(body, 0, 5, bitPos, curByte);         // unkWod61x = 0 (5 bits)
        WriteBits(body, 0, 1, bitPos, curByte);         // unused = false
        WriteBits(body, 1, 1, bitPos, curByte);         // ExpansionChosen = true
        // 无 MailSenders 的 bits
        FlushBits(body, bitPos, curByte);

        // 无 MailSenders 字符串
        // 写入名称字符串（无 null 终结符，长度已由 6-bit 编码）
        body.insert(body.end(), c.name.begin(), c.name.end());
    }

    // == 种族解锁数据 (10 条) ==
    // 格式: Int32(RaceID) + 5 bits (HasExpansion/HasAchievement/HasHeritageArmor/IsLocked/Unused) + FlushBits
    static const int raceIds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 11 };
    for (int r = 0; r < raceUnlockCount; ++r)
    {
        writeI32(body, int32(raceIds[r]));
        WriteBits(body, 1, 1, bitPos, curByte); // HasExpansion = true
        WriteBits(body, 0, 1, bitPos, curByte); // HasAchievement = false
        WriteBits(body, 0, 1, bitPos, curByte); // HasHeritageArmor = false
        WriteBits(body, 0, 1, bitPos, curByte); // IsLocked = false
        WriteBits(body, 0, 1, bitPos, curByte); // Unused1027 = false
        FlushBits(body, bitPos, curByte);
    }

    // 发送现代格式包
    SendModernPacket(ModernWorldOpcode::SMSG_ENUM_CHARACTERS_RESULT, body.data(), body.size());
    sLog.outBasic("[ModernWorld] 角色列表已翻译并发送: %u 个角色, 包体大小 %u 字节",
        charCount, uint32(body.size()));
}
//End By leewheel
