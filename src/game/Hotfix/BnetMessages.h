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

//By leewheel 2026-07-25 BNet 登录链路 protobuf 消息手写编解码
//不依赖 protoc，按 wire 格式直接解析/构造。字段号逐字取自代理 Framework/Proto/*.cs。
//线型：V=varint F32=fixed32 F64=fixed64 LD=长度前缀(string/bytes/嵌套消息)
#ifndef MANGOS_HFX_BNETMESSAGES_H
#define MANGOS_HFX_BNETMESSAGES_H

#include "Common.h"
#include "Hotfix/ProtobufStream.h"

#include <string>
#include <vector>

namespace BnetMsg
{
    // 将子消息写入父 writer 的指定字段（长度前缀嵌套消息）
    inline void WriteSubMessage(ProtobufWriter& parent, uint32 field, const ProtobufWriter& child)
    {
        const std::vector<uint8>& d = child.Data();
        parent.WriteBytes(field, d.data(), d.size());
    }

    //=====================================================================
    // 通用小消息
    //=====================================================================

    // NoData：空消息
    struct NoData
    {
        void Write(ProtobufWriter&) const {}
    };

    // ProcessId（RpcTypes.cs）：label=1 V，epoch=2 V
    struct ProcessId
    {
        uint32 label = 0;   // 进程PID
        uint32 epoch = 0;   // UnixTime 秒

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 1: if (!r.ReadVarint(v)) return false; label = uint32(v); break;
                    case 2: if (!r.ReadVarint(v)) return false; epoch = uint32(v); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
        void Write(ProtobufWriter& w) const
        {
            if (label) w.WriteUInt32(1, label);
            if (epoch) w.WriteUInt32(2, epoch);
        }
    };

    // EntityId（EntityTypes.cs）：high=1 F64，low=2 F64（注意是 fixed64！）
    struct EntityId
    {
        uint64 high = 0;
        uint64 low = 0;

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 1: if (!r.ReadFixed64(v)) return false; high = v; break;
                    case 2: if (!r.ReadFixed64(v)) return false; low = v; break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
        void Write(ProtobufWriter& w) const
        {
            if (high) w.WriteFixed64(1, high);
            if (low)  w.WriteFixed64(2, low);
        }
    };

    // Variant（AttributeTypes.cs）：登录只用 string/blob/uint/int
    // bool=2 V，int=3 V(int64)，float=4 F64，string=5 LD，blob=6 LD，
    // message=7 LD，fourcc=8 LD，uint=9 V(uint64)，entity_id=10 LD
    struct Variant
    {
        enum class Type { None, Bool, Int, Float, String, Blob, Message, Fourcc, UInt, EntityId };
        Type type = Type::None;
        bool boolValue = false;
        int64 intValue = 0;
        double floatValue = 0.0;
        std::string stringValue;
        std::vector<uint8> blobValue;
        uint64 uintValue = 0;

        // 便捷构造
        static Variant MakeString(const std::string& s) { Variant v; v.type = Type::String; v.stringValue = s; return v; }
        static Variant MakeBlob(const uint8* d, size_t n) { Variant v; v.type = Type::Blob; v.blobValue.assign(d, d + n); return v; }
        static Variant MakeUInt(uint64 u) { Variant v; v.type = Type::UInt; v.uintValue = u; return v; }

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 2: if (!r.ReadVarint(v)) return false; type = Type::Bool; boolValue = (v != 0); break;
                    case 3: if (!r.ReadVarint(v)) return false; type = Type::Int; intValue = int64(v); break;
                    case 4: if (!r.ReadFixed64(v)) return false; type = Type::Float; memcpy(&floatValue, &v, 8); break;
                    case 5: if (!r.ReadString(stringValue)) return false; type = Type::String; break;
                    case 6: if (!r.ReadLengthDelimited(p, n)) return false; type = Type::Blob; blobValue.assign(p, p + n); break;
                    case 7: if (!r.ReadLengthDelimited(p, n)) return false; type = Type::Message; blobValue.assign(p, p + n); break;
                    case 8: if (!r.ReadString(stringValue)) return false; type = Type::Fourcc; break;
                    case 9: if (!r.ReadVarint(v)) return false; type = Type::UInt; uintValue = v; break;
                    case 10: if (!r.ReadLengthDelimited(p, n)) return false; type = Type::EntityId; blobValue.assign(p, p + n); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
        void Write(ProtobufWriter& w) const
        {
            switch (type)
            {
                case Type::Bool:   w.WriteUInt32(2, boolValue ? 1 : 0); break;
                case Type::Int:    w.WriteUInt64(3, uint64(intValue)); break;
                case Type::Float:  { uint64 bits; memcpy(&bits, &floatValue, 8); w.WriteFixed64(4, bits); } break;
                case Type::String: w.WriteString(5, stringValue); break;
                case Type::Blob:   w.WriteBytes(6, blobValue.data(), blobValue.size()); break;
                case Type::Message:w.WriteBytes(7, blobValue.data(), blobValue.size()); break;
                case Type::Fourcc: w.WriteString(8, stringValue); break;
                case Type::UInt:   w.WriteUInt64(9, uintValue); break;
                case Type::EntityId:w.WriteBytes(10, blobValue.data(), blobValue.size()); break;
                default: break;
            }
        }
    };

    // Attribute（AttributeTypes.cs）：name=1 LD(string)，value=2 LD(Variant)
    struct Attribute
    {
        std::string name;
        Variant value;

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 1: if (!r.ReadString(name)) return false; break;
                    case 2: if (!r.ReadLengthDelimited(p, n)) return false; if (!value.Read(p, n)) return false; break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
        void Write(ProtobufWriter& w) const
        {
            if (!name.empty()) w.WriteString(1, name);
            ProtobufWriter vw; value.Write(vw); WriteSubMessage(w, 2, vw);
        }
    };

    //=====================================================================
    // ConnectionService
    //=====================================================================

    // ConnectRequest：client_id=1 LD(ProcessId)，bind_request=2 LD，use_bindless_rpc=3 V(bool)
    struct ConnectRequest
    {
        ProcessId clientId;
        bool useBindlessRpc = false;

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0; const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 1: if (!r.ReadLengthDelimited(p, n)) return false; clientId.Read(p, n); break;
                    case 3: if (!r.ReadVarint(v)) return false; useBindlessRpc = (v != 0); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // ConnectResponse：server_id=1，client_id=2，server_time=6 V(uint64)，use_bindless_rpc=7 V(bool)
    struct ConnectResponse
    {
        ProcessId serverId;
        ProcessId clientId;
        uint64 serverTime = 0;       // UnixTime 毫秒
        bool useBindlessRpc = false;

        void Write(ProtobufWriter& w) const
        {
            { ProtobufWriter s; serverId.Write(s); WriteSubMessage(w, 1, s); }
            { ProtobufWriter s; clientId.Write(s); WriteSubMessage(w, 2, s); }
            if (serverTime) w.WriteUInt64(6, serverTime);
            w.WriteUInt32(7, useBindlessRpc ? 1 : 0);
        }
    };

    // DisconnectRequest：error_code=1 V
    struct DisconnectRequest
    {
        uint32 errorCode = 0;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 1: if (!r.ReadVarint(v)) return false; errorCode = uint32(v); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // DisconnectNotification：error_code=1 V，reason=2 LD(string)
    struct DisconnectNotification
    {
        uint32 errorCode = 0;
        void Write(ProtobufWriter& w) const
        {
            if (errorCode) w.WriteUInt32(1, errorCode);
        }
    };

    //=====================================================================
    // AuthenticationService
    //=====================================================================

    // LogonRequest：program=1，platform=2，locale=3，email=4，version=5，application_version=6 V
    //（复刻只读 program/platform/locale/application_version）
    struct LogonRequest
    {
        std::string program;
        std::string platform;
        std::string locale;
        std::string email;
        uint32 applicationVersion = 0;

        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 1: if (!r.ReadString(program)) return false; break;
                    case 2: if (!r.ReadString(platform)) return false; break;
                    case 3: if (!r.ReadString(locale)) return false; break;
                    case 4: if (!r.ReadString(email)) return false; break;
                    case 6: if (!r.ReadVarint(v)) return false; applicationVersion = uint32(v); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // VerifyWebCredentialsRequest：web_credentials=1 LD(bytes)（内容=login ticket 字符串）
    struct VerifyWebCredentialsRequest
    {
        std::vector<uint8> webCredentials;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 1: if (!r.ReadLengthDelimited(p, n)) return false; webCredentials.assign(p, p + n); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // LogonResult（服务器推送）：error_code=1 V，account_id=2 LD(EntityId)，
    // game_account_id=3 repeated LD(EntityId)，session_key=9 LD(bytes)
    struct LogonResult
    {
        uint32 errorCode = 0;
        EntityId accountId;
        std::vector<EntityId> gameAccountId;
        std::vector<uint8> sessionKey;   // 64 字节随机

        void Write(ProtobufWriter& w) const
        {
            if (errorCode) w.WriteUInt32(1, errorCode);
            { ProtobufWriter s; accountId.Write(s); WriteSubMessage(w, 2, s); }
            for (const auto& ga : gameAccountId)
            {
                ProtobufWriter s; ga.Write(s); WriteSubMessage(w, 3, s);
            }
            if (!sessionKey.empty())
                w.WriteBytes(9, sessionKey.data(), sessionKey.size());
        }
    };

    //=====================================================================
    // ChallengeService
    //=====================================================================

    // ChallengeExternalRequest（服务器推送）：request_token=1 LD，payload_type=2 LD，payload=3 LD(bytes)
    struct ChallengeExternalRequest
    {
        std::string requestToken;
        std::string payloadType;      // "web_auth_url"
        std::vector<uint8> payload;   // REST 登录 URL 的 UTF-8

        void Write(ProtobufWriter& w) const
        {
            if (!requestToken.empty()) w.WriteString(1, requestToken);
            if (!payloadType.empty())  w.WriteString(2, payloadType);
            if (!payload.empty())      w.WriteBytes(3, payload.data(), payload.size());
        }
    };

    //=====================================================================
    // AccountService
    //=====================================================================

    // GetAccountStateOptions：field_privacy_info=3 V（其余略）
    struct GetAccountStateOptions
    {
        bool fieldPrivacyInfo = false;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 3: if (!r.ReadVarint(v)) return false; fieldPrivacyInfo = (v != 0); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // GetAccountStateRequest：options=10 LD
    struct GetAccountStateRequest
    {
        GetAccountStateOptions options;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 10: if (!r.ReadLengthDelimited(p, n)) return false; options.Read(p, n); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // PrivacyInfo：is_using_rid=3，is_visible_for_view_friends=4，is_hidden_from_friend_finder=5
    struct PrivacyInfo
    {
        bool isUsingRid = false;
        bool isVisibleForViewFriends = false;
        bool isHiddenFromFriendFinder = true;
        void Write(ProtobufWriter& w) const
        {
            if (isUsingRid) w.WriteUInt32(3, 1);
            if (isVisibleForViewFriends) w.WriteUInt32(4, 1);
            if (isHiddenFromFriendFinder) w.WriteUInt32(5, 1);
        }
    };

    // AccountState：privacy_info=2 LD
    struct AccountState
    {
        PrivacyInfo privacyInfo;
        void Write(ProtobufWriter& w) const
        {
            ProtobufWriter s; privacyInfo.Write(s); WriteSubMessage(w, 2, s);
        }
    };

    // AccountFieldTags：privacy_info_tag=3 F32
    struct AccountFieldTags
    {
        uint32 privacyInfoTag = 0;
        void Write(ProtobufWriter& w) const
        {
            if (privacyInfoTag) w.WriteFixed32(3, privacyInfoTag);
        }
    };

    // GetAccountStateResponse：state=1 LD，tags=2 LD
    struct GetAccountStateResponse
    {
        AccountState state;
        AccountFieldTags tags;
        void Write(ProtobufWriter& w) const
        {
            { ProtobufWriter s; state.Write(s); WriteSubMessage(w, 1, s); }
            { ProtobufWriter s; tags.Write(s); WriteSubMessage(w, 2, s); }
        }
    };

    // GetGameAccountStateOptions：field_game_level_info=2，field_game_status=4
    struct GetGameAccountStateOptions
    {
        bool fieldGameLevelInfo = false;
        bool fieldGameStatus = false;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                uint64 v = 0;
                switch (f)
                {
                    case 2: if (!r.ReadVarint(v)) return false; fieldGameLevelInfo = (v != 0); break;
                    case 4: if (!r.ReadVarint(v)) return false; fieldGameStatus = (v != 0); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // GetGameAccountStateRequest：options=10 LD
    struct GetGameAccountStateRequest
    {
        GetGameAccountStateOptions options;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 10: if (!r.ReadLengthDelimited(p, n)) return false; options.Read(p, n); break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // GameLevelInfo：name=8 LD(string)，program=9 F32
    struct GameLevelInfo
    {
        std::string name;
        uint32 program = 0;   // "WoW" fourcc
        void Write(ProtobufWriter& w) const
        {
            if (!name.empty()) w.WriteString(8, name);
            if (program) w.WriteFixed32(9, program);
        }
    };

    // GameStatus：is_suspended=4，is_banned=5，suspension_expires=6 V(uint64)，program=7 F32
    struct GameStatus
    {
        bool isSuspended = false;
        bool isBanned = false;
        uint64 suspensionExpires = 0;
        uint32 program = 0;
        void Write(ProtobufWriter& w) const
        {
            if (isSuspended) w.WriteUInt32(4, 1);
            if (isBanned) w.WriteUInt32(5, 1);
            if (suspensionExpires) w.WriteUInt64(6, suspensionExpires);
            if (program) w.WriteFixed32(7, program);
        }
    };

    // GameAccountState：game_level_info=1 LD，game_status=3 LD
    struct GameAccountState
    {
        bool hasGameLevelInfo = false;
        GameLevelInfo gameLevelInfo;
        bool hasGameStatus = false;
        GameStatus gameStatus;
        void Write(ProtobufWriter& w) const
        {
            if (hasGameLevelInfo) { ProtobufWriter s; gameLevelInfo.Write(s); WriteSubMessage(w, 1, s); }
            if (hasGameStatus)    { ProtobufWriter s; gameStatus.Write(s); WriteSubMessage(w, 3, s); }
        }
    };

    // GameAccountFieldTags：game_level_info_tag=2 F32，game_status_tag=4 F32
    struct GameAccountFieldTags
    {
        uint32 gameLevelInfoTag = 0;
        uint32 gameStatusTag = 0;
        void Write(ProtobufWriter& w) const
        {
            if (gameLevelInfoTag) w.WriteFixed32(2, gameLevelInfoTag);
            if (gameStatusTag) w.WriteFixed32(4, gameStatusTag);
        }
    };

    // GetGameAccountStateResponse：state=1 LD，tags=2 LD
    struct GetGameAccountStateResponse
    {
        GameAccountState state;
        GameAccountFieldTags tags;
        void Write(ProtobufWriter& w) const
        {
            { ProtobufWriter s; state.Write(s); WriteSubMessage(w, 1, s); }
            { ProtobufWriter s; tags.Write(s); WriteSubMessage(w, 2, s); }
        }
    };

    //=====================================================================
    // GameUtilitiesService
    //=====================================================================

    // ClientRequest：attribute=1 repeated LD(Attribute)
    struct ClientRequest
    {
        std::vector<Attribute> attributes;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                const uint8* p = nullptr; size_t n = 0;
                switch (f)
                {
                    case 1:
                    {
                        if (!r.ReadLengthDelimited(p, n)) return false;
                        Attribute attr;
                        if (!attr.Read(p, n)) return false;
                        attributes.push_back(std::move(attr));
                        break;
                    }
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }

        // 查找指定名属性
        const Attribute* FindAttribute(const std::string& name) const
        {
            for (const auto& a : attributes)
                if (a.name == name)
                    return &a;
            return nullptr;
        }
    };

    // ClientResponse：attribute=1 repeated LD(Attribute)
    struct ClientResponse
    {
        std::vector<Attribute> attributes;
        void AddAttribute(const std::string& name, const Variant& value)
        {
            Attribute a; a.name = name; a.value = value;
            attributes.push_back(std::move(a));
        }
        void Write(ProtobufWriter& w) const
        {
            for (const auto& a : attributes)
            {
                ProtobufWriter s; a.Write(s); WriteSubMessage(w, 1, s);
            }
        }
    };

    // GetAllValuesForAttributeRequest：attribute_key=1 LD(string)
    struct GetAllValuesForAttributeRequest
    {
        std::string attributeKey;
        bool Read(const uint8* data, size_t len)
        {
            ProtobufReader r(data, len);
            while (!r.Eof())
            {
                uint32 f = 0, w = 0;
                if (!r.ReadTag(f, w)) return false;
                switch (f)
                {
                    case 1: if (!r.ReadString(attributeKey)) return false; break;
                    default: if (!r.SkipField(w)) return false; break;
                }
            }
            return true;
        }
    };

    // GetAllValuesForAttributeResponse：attribute_value=1 repeated LD(Variant)
    struct GetAllValuesForAttributeResponse
    {
        std::vector<Variant> values;
        void Write(ProtobufWriter& w) const
        {
            for (const auto& v : values)
            {
                ProtobufWriter s; v.Write(s); WriteSubMessage(w, 1, s);
            }
        }
    };
}

#endif
//End By leewheel
