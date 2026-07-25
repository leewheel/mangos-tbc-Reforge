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

//By leewheel 2026-07-25 BNet 协议常量：服务原始哈希、RPC 错误码、各服务方法ID
//数值逐字取自代理 Framework/Constants/ServiceHash.cs 与 BattlenetConst.cs
#ifndef MANGOS_HFX_BNETCONSTANTS_H
#define MANGOS_HFX_BNETCONSTANTS_H

#include "Common.h"

// 服务原始哈希（OriginalHash）：BNet 帧 Header.service_hash 字段，用于 RPC 路由
namespace BnetServiceHash
{
    constexpr uint32 AccountService         = 0x62DA0891;  // 账号服务（GetAccountState/GetGameAccountState）
    constexpr uint32 AuthenticationListener = 0x71240E35;  // 认证监听（服务器下发 LogonResult）
    constexpr uint32 AuthenticationService  = 0x0DECFC01;  // 认证服务（Logon/VerifyWebCredentials）
    constexpr uint32 ChallengeListener      = 0xBBDA171F;  // 挑战监听（服务器下发 web_auth_url）
    constexpr uint32 ConnectionService      = 0x65446991;  // 连接服务（Connect/KeepAlive/Disconnect）
    constexpr uint32 GameUtilitiesService   = 0x3FC1274D;  // 游戏工具服务（realm 列表/加入）
    constexpr uint32 ResourcesService       = 0xECBE75BA;  // 资源服务（未实现方法回 Ok 空包即可）
}

// BattlenetRpcErrorCode：Header.status 字段取值（仅列登录链路用到的）
enum BattlenetRpcErrorCode : uint32
{
    BNET_RPC_OK                    = 0x00000000,
    BNET_RPC_TIMED_OUT             = 0x00000002,
    BNET_RPC_DENIED                = 0x00000003,
    BNET_RPC_BAD_VERSION           = 0x0000001C,
    BNET_RPC_GAME_ACCOUNT_BANNED   = 0x00000034,
    BNET_RPC_GAME_ACCOUNT_SUSPENDED= 0x00000035,
    BNET_RPC_BAD_PROGRAM           = 0x0000004D,
    BNET_RPC_BAD_LOCALE            = 0x0000004E,
    BNET_RPC_BAD_PLATFORM          = 0x0000004F,
    BNET_RPC_NOT_IMPLEMENTED       = 0x00000BC7,
    // 游戏工具服务专用错误（高位 0x80000000）
    BNET_UTIL_INVALID_IDENTITY_ARGS= 0x8000006E,  // Param_Identity 无效
    BNET_USER_BAD_WOW_ACCOUNT      = 0x800000D3,  // 缺少游戏账号
    BNET_WOW_INVALID_JOIN_TICKET   = 0x8000012E,  // realm 加入参数缺失
    BNET_WOW_DENIED_REALM_LIST     = 0x80000132,  // Param_ClientInfo 缺失
    BNET_WOW_MISSING_GAME_ACCOUNT  = 0x80000133,
};

// 各服务方法ID（Header.method_id）
namespace BnetMethod
{
    // ConnectionService
    constexpr uint32 Connect          = 1;   // ConnectRequest -> ConnectResponse
    constexpr uint32 KeepAlive        = 5;   // NoData -> 无
    constexpr uint32 RequestDisconnect= 7;   // DisconnectRequest -> 无（并反推 DisconnectNotification）
    constexpr uint32 DisconnectNotify = 4;   // 服务器推送 DisconnectNotification

    // AuthenticationService
    constexpr uint32 Logon              = 1; // LogonRequest -> NoData（并反推 ChallengeExternalRequest）
    constexpr uint32 VerifyWebCredentials = 7; // VerifyWebCredentialsRequest -> 无（并反推 LogonResult）

    // AuthenticationListener（服务器推送）
    constexpr uint32 LogonResult        = 5; // 推送 LogonResult

    // ChallengeListener（服务器推送）
    constexpr uint32 ChallengeExternal  = 3; // 推送 ChallengeExternalRequest

    // AccountService
    constexpr uint32 GetAccountState    = 30; // GetAccountStateRequest -> GetAccountStateResponse
    constexpr uint32 GetGameAccountState= 31; // GetGameAccountStateRequest -> GetGameAccountStateResponse

    // GameUtilitiesService
    constexpr uint32 GenericClientRequest    = 1;  // ClientRequest -> ClientResponse
    constexpr uint32 GetAllValuesForAttribute= 10; // GetAllValuesForAttributeRequest -> Response
}

// 账号/游戏账号 EntityId 的 High 魔法值（照抄代理，用 703 HighGuid 会导致客户端断开）
namespace BnetEntityHigh
{
    constexpr uint64 Account     = 0x0100000000000000ULL;
    constexpr uint64 GameAccount = 0x020000200576F51ULL;
}

// "WoW" 的 ASCII fourcc（'W'=0x57 'o'=0x6F 'W'=0x57），用于 GameLevelInfo.program / GameStatus.program
constexpr uint32 BNET_PROGRAM_WOW = 0x00576F57;

// AccountFieldTags / GameAccountFieldTags 固定标签值
namespace BnetFieldTag
{
    constexpr uint32 PrivacyInfo    = 0xD7CA834D;
    constexpr uint32 GameLevelInfo  = 0x5C46D483;
    constexpr uint32 GameStatus     = 0x98B75F99;
}

#endif
//End By leewheel
