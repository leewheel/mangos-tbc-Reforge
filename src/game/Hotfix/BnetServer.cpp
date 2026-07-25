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

//By leewheel 2026-07-25 BNet 服务器实现（TLS监听 + REST登录端点 + 自签证书生成）
#include "Hotfix/BnetServer.h"
#include "Hotfix/BnetSocket.h"
//By leewheel 2026-07-25 现代 World 监听支持
#include "Hotfix/ModernWorldSocket.h"
//End By leewheel
#include "Database/DatabaseEnv.h"
#include "Database/QueryResult.h"
#include "Log/Log.h"
#include "Config/Config.h"
#include "Auth/CryptoHash.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ctime>

// ============================================================================
// BnetServer 单例
// ============================================================================

BnetServer& BnetServer::Instance()
{
    static BnetServer inst;
    return inst;
}

BnetServer::BnetServer()
    : m_sslContext(boost::asio::ssl::context::tlsv12_server)
{
}

BnetServer::~BnetServer()
{
    Stop();
}

// ============================================================================
// 初始化 SSL 上下文（自签证书 + 私钥，运行时生成）
// ============================================================================

bool BnetServer::InitSslContext()
{
    // 生成 RSA 2048 密钥对
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey)
    {
        sLog.outError("[BNet] EVP_PKEY_new 失败");
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0)
    {
        sLog.outError("[BNet] RSA密钥生成失败");
        if (ctx) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // 生成自签证书
    X509* x509 = X509_new();
    if (!x509)
    {
        sLog.outError("[BNet] X509_new 失败");
        EVP_PKEY_free(pkey);
        return false;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600); // 1年有效

    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("LWCore BNet Server"), -1, -1, 0);
    X509_set_issuer_name(x509, name);

    X509_sign(x509, pkey, EVP_sha256());

    // 将证书和私钥写入 SSL 上下文
    if (SSL_CTX_use_certificate(m_sslContext.native_handle(), x509) != 1)
    {
        sLog.outError("[BNet] SSL_CTX_use_certificate 失败");
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }
    if (SSL_CTX_use_PrivateKey(m_sslContext.native_handle(), pkey) != 1)
    {
        sLog.outError("[BNet] SSL_CTX_use_PrivateKey 失败");
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);

    sLog.outString("[BNet] 自签TLS证书已生成");
    return true;
}

// ============================================================================
// 启动
// ============================================================================

bool BnetServer::Start()
{
    if (m_running)
        return true;

    if (!InitSslContext())
    {
        sLog.outError("[BNet] SSL上下文初始化失败，BNet服务器未启动");
        return false;
    }

    std::string bindIp = sConfig.GetStringDefault("BNet.BindIP", "0.0.0.0");
    int bnetPort = sConfig.GetIntDefault("BNet.Port", 1119);
    int restPort = sConfig.GetIntDefault("BNet.RestPort", 8081);
//By leewheel 2026-07-25 现代 World 监听端口
    int modernWorldPort = sConfig.GetIntDefault("ModernWorldPort", 8086);
//End By leewheel
    int threadCount = sConfig.GetIntDefault("BNet.Threads", 2);

    try
    {
        // BNet TLS 监听器
        m_bnetAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(
            m_ioContext,
            boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(bindIp), bnetPort));
        sLog.outString("[BNet] TLS监听器已启动 %s:%d", bindIp.c_str(), bnetPort);

        // REST HTTP 监听器
        m_restAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(
            m_ioContext,
            boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(bindIp), restPort));
        sLog.outString("[BNet] REST登录端点已启动 %s:%d", bindIp.c_str(), restPort);

//By leewheel 2026-07-25 现代 World TCP 监听器
        m_modernWorldAcceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(
            m_ioContext,
            boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(bindIp), modernWorldPort));
        sLog.outString("[ModernWorld] TCP监听器已启动 %s:%d", bindIp.c_str(), modernWorldPort);
//End By leewheel
    }
    catch (const boost::system::system_error& e)
    {
        sLog.outError("[BNet] 监听器启动失败: %s", e.what());
        return false;
    }

    // 开始接受连接
    StartAccept();
    StartRestAccept();
//By leewheel 2026-07-25 启动现代 World 监听
    StartModernWorldAccept();
//End By leewheel

    // 启动工作线程
    m_running = true;
    for (int i = 0; i < threadCount; ++i)
    {
        m_threads.emplace_back([this]()
        {
            while (m_running)
            {
                try
                {
                    m_ioContext.run();
                    break; // run() 正常退出
                }
                catch (const std::exception& e)
                {
                    sLog.outError("[BNet] 工作线程异常: %s", e.what());
                }
            }
        });
    }

    sLog.outString("[BNet] 服务器已启动 (%d工作线程)", threadCount);
    return true;
}

void BnetServer::Stop()
{
    if (!m_running)
        return;

    m_running = false;
    m_ioContext.stop();

    for (auto& t : m_threads)
    {
        if (t.joinable())
            t.join();
    }
    m_threads.clear();
    sLog.outString("[BNet] 服务器已停止");
}

// ============================================================================
// BNet TLS 连接接受
// ============================================================================

void BnetServer::StartAccept()
{
    auto session = std::make_shared<BnetSocket>(m_ioContext, m_sslContext);
    m_bnetAcceptor->async_accept(session->m_sslStream.lowest_layer(),
        [this, session](const boost::system::error_code& ec)
        {
            OnAccept(session, ec);
        });
}

void BnetServer::OnAccept(std::shared_ptr<BnetSocket> session, const boost::system::error_code& ec)
{
    if (!ec)
    {
        session->Start();
    }
    else if (ec != boost::asio::error::operation_aborted)
    {
        sLog.outError("[BNet] Accept失败: %s", ec.message().c_str());
    }

    if (m_running)
        StartAccept();
}

// ============================================================================
// REST HTTP 连接接受（简易实现，不用TLS因为客户端内置浏览器会信任）
// ============================================================================

void BnetServer::StartRestAccept()
{
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(m_ioContext);
    m_restAcceptor->async_accept(*socket,
        [this, socket](const boost::system::error_code& ec)
        {
            OnRestAccept(socket, ec);
        });
}

void BnetServer::OnRestAccept(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code& ec)
{
    if (!ec)
    {
        HandleRestConnection(socket);
    }
    else if (ec != boost::asio::error::operation_aborted)
    {
        sLog.outError("[BNet REST] Accept失败: %s", ec.message().c_str());
    }

    if (m_running)
        StartRestAccept();
}

void BnetServer::HandleRestConnection(std::shared_ptr<boost::asio::ip::tcp::socket> socket)
{
    // 读取 HTTP 请求（简易：一次性读取所有已到达的数据）
    auto buf = std::make_shared<std::vector<char>>(4096, 0);
    auto self = socket;
    boost::asio::async_read(*socket,
        boost::asio::buffer(buf->data(), buf->size()),
        boost::asio::transfer_at_least(1),
        [this, socket, buf](const boost::system::error_code& ec, std::size_t bytesRead)
        {
            if (ec && ec != boost::asio::error::eof)
                return;

            std::string request(buf->data(), bytesRead);
            std::string remoteAddr;
            try { remoteAddr = socket->remote_endpoint().address().to_string(); }
            catch (...) { remoteAddr = "unknown"; }

            // 解析 HTTP 方法和路径
            std::string method, path, body;
            std::istringstream iss(request);
            iss >> method >> path;

            // 找 body（在空行之后）
            auto bodyPos = request.find("\r\n\r\n");
            if (bodyPos != std::string::npos)
                body = request.substr(bodyPos + 4);

            // 处理
            std::string response = HandleRestLogin(method, path, body, remoteAddr);

            // 发送 HTTP 响应
            auto respBuf = std::make_shared<std::string>(response);
            boost::asio::async_write(*socket,
                boost::asio::buffer(respBuf->data(), respBuf->size()),
                [socket, respBuf](const boost::system::error_code& /*ec*/, std::size_t /*written*/)
                {
                    boost::system::error_code shutEc;
                    socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, shutEc);
                });
        });
}

// ============================================================================
// REST 登录处理
// ============================================================================

std::string BnetServer::HandleRestLogin(const std::string& method, const std::string& path,
                                         const std::string& body, const std::string& remoteAddr)
{
    // 所有 REST 路径都应包含 /bnetserver/login
    // GET  → 返回登录表单（JSON格式：inputs数组）
    // POST → 验证账号密码，返回 ticket

    auto MakeHttpResponse = [](int code, const std::string& status, const std::string& contentType, const std::string& body) -> std::string
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << code << " " << status << "\r\n";
        oss << "Content-Type: " << contentType << "\r\n";
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    };

    if (method == "GET")
    {
        // 返回登录表单 JSON（模拟 BNet 登录表单）
        std::string json = R"({"type":"LOGIN_FORM","inputs":[)"
            R"({"input_id":"account_name","type":"text","label":"Account Name","max_length":128},)"
            R"({"input_id":"password","type":"password","label":"Password","max_length":16})"
            R"(]})";
        return MakeHttpResponse(200, "OK", "application/json;charset=utf-8", json);
    }
    else if (method == "POST")
    {
        // 从 body 中解析账号密码（JSON 格式：{"inputs":[{"input_id":"account_name","value":"xxx"},{"input_id":"password","value":"xxx"}]}）
        // 简易 JSON 解析
        std::string accountName, password;

        // 查找 account_name 的 value
        auto findValue = [&body](const std::string& key) -> std::string
        {
            std::string searchKey = "\"input_id\":\"" + key + "\"";
            auto pos = body.find(searchKey);
            if (pos == std::string::npos) return "";
            auto valuePos = body.find("\"value\":\"", pos);
            if (valuePos == std::string::npos) return "";
            valuePos += 9; // strlen("\"value\":\"")
            auto endPos = body.find('"', valuePos);
            if (endPos == std::string::npos) return "";
            return body.substr(valuePos, endPos - valuePos);
        };

        accountName = findValue("account_name");
        password = findValue("password");

        if (accountName.empty())
        {
            std::string json = R"({"authentication_state":"LOGIN","error_code":"UNABLE_TO_DECODE","error_message":"Invalid request"})";
            return MakeHttpResponse(400, "Bad Request", "application/json;charset=utf-8", json);
        }

        // 转大写（mangos 用大写存储）
        std::transform(accountName.begin(), accountName.end(), accountName.begin(), ::toupper);
        std::transform(password.begin(), password.end(), password.begin(), ::toupper);

        // 查询数据库验证账号
        std::string safeAccount = accountName;
        LoginDatabase.escape_string(safeAccount);

        auto result = LoginDatabase.PQuery(
            "SELECT id, sha_pass_hash, expansion, sessionkey FROM account WHERE username='%s'",
            safeAccount.c_str());

        if (!result)
        {
            sLog.outBasic("[BNet REST] 登录失败：账号不存在 %s 来自 %s", accountName.c_str(), remoteAddr.c_str());
            std::string json = R"({"authentication_state":"LOGIN","error_code":"UNABLE_TO_DECODE","error_message":"Account not found"})";
            return MakeHttpResponse(200, "OK", "application/json;charset=utf-8", json);
        }

        Field* fields = result->Fetch();
        uint32 accountId = fields[0].GetUInt32();
        std::string dbPassHash = fields[1].GetCppString();
        uint8 expansion = fields[2].GetUInt8();

        // 验证密码：SHA1(UPPER(username):UPPER(password))
        Sha1Hash sha;
        std::string token = accountName + ":" + password;
        sha.UpdateData(token);
        sha.Finalize();

        // 转为hex字符串
        std::ostringstream hashOss;
        for (int i = 0; i < 20; ++i)
            hashOss << std::hex << std::setfill('0') << std::setw(2) << uint32(sha.GetDigest()[i]);
        std::string computedHash = hashOss.str();

        if (computedHash != dbPassHash)
        {
            sLog.outBasic("[BNet REST] 登录失败：密码错误 %s 来自 %s", accountName.c_str(), remoteAddr.c_str());
            std::string json = R"({"authentication_state":"LOGIN","error_code":"UNABLE_TO_DECODE","error_message":"Invalid credentials"})";
            return MakeHttpResponse(200, "OK", "application/json;charset=utf-8", json);
        }

        // 生成 login ticket："TC-" + 20字节随机hex
        uint8 ticketRandom[20];
        RAND_bytes(ticketRandom, 20);
        std::ostringstream ticketOss;
        ticketOss << "TC-";
        for (int i = 0; i < 20; ++i)
            ticketOss << std::hex << std::setfill('0') << std::setw(2) << uint32(ticketRandom[i]);
        std::string loginTicket = ticketOss.str();

        // 生成 64 字节 SessionKey
        std::vector<uint8> sessionKey(64);
        RAND_bytes(sessionKey.data(), 64);

        // 更新数据库 sessionkey（hex格式，用于世界连接验证）
        std::ostringstream skHex;
        for (size_t i = 0; i < sessionKey.size(); ++i)
            skHex << std::hex << std::setfill('0') << std::setw(2) << uint32(sessionKey[i]);
        LoginDatabase.PExecute("UPDATE account SET sessionkey='%s' WHERE id=%u",
            skHex.str().c_str(), accountId);

        // 存入票据管理器
        BnetSessionTicket ticket;
        ticket.accountId = accountId;
        ticket.accountName = accountName;
        ticket.expansion = expansion;
        ticket.sessionKey = sessionKey;
        sBnetSessionMgr.AddTicket(loginTicket, ticket);

        sLog.outBasic("[BNet REST] 登录成功：%s (id:%u) 来自 %s", accountName.c_str(), accountId, remoteAddr.c_str());

        // 返回成功（authentication_state=DONE + login_ticket）
        std::string json = R"({"authentication_state":"DONE","login_ticket":")" + loginTicket + R"("})";
        return MakeHttpResponse(200, "OK", "application/json;charset=utf-8", json);
    }

    // 其他方法
    return MakeHttpResponse(404, "Not Found", "text/plain", "Not Found");
}

// ============================================================================
// 现代 World TCP 监听（端口8086）
// ============================================================================

void BnetServer::StartModernWorldAccept()
{
    if (!m_modernWorldAcceptor)
        return;

    auto session = std::make_shared<ModernWorldSocket>(m_ioContext);
    m_modernWorldAcceptor->async_accept(session->GetSocket(),
        [this, session](const boost::system::error_code& ec)
        {
            OnModernWorldAccept(session, ec);
        });
}

void BnetServer::OnModernWorldAccept(std::shared_ptr<ModernWorldSocket> sock, const boost::system::error_code& ec)
{
    if (!m_running)
        return;

    if (!ec)
    {
        sock->Start();
    }
    else if (ec != boost::asio::error::operation_aborted)
    {
        sLog.outError("[ModernWorld] Accept失败: %s", ec.message().c_str());
    }

    StartModernWorldAccept();
}
//End By leewheel
