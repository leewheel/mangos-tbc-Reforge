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

//By leewheel 2026-07-25 BNet 服务器（TLS监听 + REST登录端点 + 现代World监听）
//用途：管理 BNet TLS 监听器（端口1119）、REST HTTP(S) 登录端点、
//      以及现代 World 监听器（端口8086，接受 2.5.3 客户端世界连接），
//      集成在 mangosd.exe 内运行。
#ifndef MANGOS_HFX_BNETSERVER_H
#define MANGOS_HFX_BNETSERVER_H

#include "Common.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <thread>
#include <vector>
#include <atomic>

class BnetSocket;
class ModernWorldSocket;

// BNet 服务器：管理 TLS 监听和 REST 登录
class BnetServer
{
    public:
        static BnetServer& Instance();

        // 初始化 SSL 上下文并启动监听（在 Master::Run 中调用）
        bool Start();
        // 停止服务器
        void Stop();

        boost::asio::io_context& GetIoContext() { return m_ioContext; }

    private:
        BnetServer();
        ~BnetServer();

        // 初始化 SSL 上下文（自签证书）
        bool InitSslContext();
        // 开始接受 BNet TLS 连接
        void StartAccept();
        // 接受回调
        void OnAccept(std::shared_ptr<BnetSocket> session, const boost::system::error_code& ec);

        // REST HTTP(S) 监听（简易 HTTP 实现）
        void StartRestAccept();
        void OnRestAccept(std::shared_ptr<boost::asio::ip::tcp::socket> socket, const boost::system::error_code& ec);
        void HandleRestConnection(std::shared_ptr<boost::asio::ip::tcp::socket> socket);

        // REST 请求处理
        std::string HandleRestLogin(const std::string& method, const std::string& path,
                                    const std::string& body, const std::string& remoteAddr);

//By leewheel 2026-07-25 现代 World 监听（端口8086，接受 2.5.3 客户端 TCP 连接）
        void StartModernWorldAccept();
        void OnModernWorldAccept(std::shared_ptr<ModernWorldSocket> sock, const boost::system::error_code& ec);
//End By leewheel

        boost::asio::io_context m_ioContext;
        boost::asio::ssl::context m_sslContext;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> m_bnetAcceptor;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> m_restAcceptor;
//By leewheel 2026-07-25 现代 World 监听器
        std::unique_ptr<boost::asio::ip::tcp::acceptor> m_modernWorldAcceptor;
//End By leewheel

        std::vector<std::thread> m_threads;
        std::atomic<bool> m_running{false};
};

#define sBnetServer BnetServer::Instance()

#endif
//End By leewheel
