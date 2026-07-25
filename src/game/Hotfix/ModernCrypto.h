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

//By leewheel 2026-07-25 现代 world 协议加密工具层
//内容：SHA256 / HMAC-SHA256 / SessionKeyGenerator / world 会话密钥派生 /
//      SMSG_ENTER_ENCRYPTED_MODE 的 RSA 签名。
//算法与种子逐字取自代理 WorldSocket.cs / SessionKeyGeneration.cs / AuthenticationPackets.cs / RsaStore。
#ifndef MANGOS_HFX_MODERNCRYPTO_H
#define MANGOS_HFX_MODERNCRYPTO_H

#include "Common.h"

#include <array>
#include <vector>

namespace ModernCrypto
{
    constexpr size_t SHA256_LEN = 32;

    // 各类固定种子（16字节），取自代理 WorldSocket.cs / AuthenticationPackets.cs
    extern const uint8 AuthCheckSeed[16];        // digest 校验
    extern const uint8 SessionKeySeed[16];       // 派生 40 字节 sessionKey
    extern const uint8 ContinuedSessionSeed[16]; // 断线重连 digest
    extern const uint8 EncryptionKeySeed[16];    // 派生 16 字节 AES-GCM 密钥
    extern const uint8 EnableEncryptionSeed[16]; // ENTER_ENCRYPTED_MODE 签名

    // 一次性 SHA256，输出 32 字节到 out（out 至少 32 字节）
    void Sha256(const uint8* data, size_t len, uint8 out[SHA256_LEN]);

    // 多段拼接后 SHA256：SHA256(a || b)
    void Sha256Two(const uint8* a, size_t alen, const uint8* b, size_t blen, uint8 out[SHA256_LEN]);

    // HMAC-SHA256，输出 32 字节到 out（key 任意长，data 为多段拼接）
    void HmacSha256(const uint8* key, size_t keyLen,
                    const uint8* d1, size_t l1,
                    const uint8* d2, size_t l2,
                    const uint8* d3, size_t l3,
                    uint8 out[SHA256_LEN]);

    // SessionKeyGenerator：TrinityCore 风格密钥流生成器（SHA256，32字节块）
    // 构造：o1=SHA256(前半)，o2=SHA256(后半)，o0=SHA256(o1||0||o2)
    class SessionKeyGenerator
    {
        public:
            SessionKeyGenerator(const uint8* buff, size_t len);
            // 生成 sz 字节密钥流到 buf
            void Generate(uint8* buf, size_t sz);

        private:
            void FillUp();
            std::array<uint8, SHA256_LEN> m_o0{};
            std::array<uint8, SHA256_LEN> m_o1{};
            std::array<uint8, SHA256_LEN> m_o2{};
            size_t m_taken = 0;
    };

    // 校验客户端 CMSG_AUTH_SESSION 的 digest（24字节）
    // digestKey = SHA256(sessionKey K || seed)
    // expected  = HMAC-SHA256(digestKey, localChallenge || serverChallenge || AuthCheckSeed) 取前24字节
    bool VerifyAuthDigest(const uint8* sessionKey, size_t sessionKeyLen,
                          const uint8* seed,
                          const uint8 localChallenge[16],
                          const uint8 serverChallenge[16],
                          const uint8 digest[24]);

    // 派生 40 字节 world sessionKey
    // keyData = SHA256(K)
    // hmac    = HMAC-SHA256(keyData, serverChallenge || localChallenge || SessionKeySeed)
    // out40   = SessionKeyGenerator(hmac).Generate(40)
    void DeriveSessionKey40(const uint8* sessionKey, size_t sessionKeyLen,
                            const uint8 localChallenge[16],
                            const uint8 serverChallenge[16],
                            uint8 out40[40]);

    // 派生 16 字节 AES-GCM encryptKey
    // hmac     = HMAC-SHA256(sessionKey40, localChallenge || serverChallenge || EncryptionKeySeed)
    // out16    = hmac[0..16)
    void DeriveEncryptKey16(const uint8 sessionKey40[40],
                            const uint8 localChallenge[16],
                            const uint8 serverChallenge[16],
                            uint8 out16[16]);

    // 构造 SMSG_ENTER_ENCRYPTED_MODE 的 RSA 签名（TBC 用 RSA，256字节，字节序反转）
    // toSign    = HMAC-SHA256(encryptKey16, [enabled] || EnableEncryptionSeed)
    // signature = RSA_PKCS1_SHA256_Sign(toSign) 反转 → 256 字节
    // 成功返回 true，signature 输出 256 字节
    bool SignEnterEncryptedMode(const uint8 encryptKey16[16], bool enabled, uint8 signatureOut[256]);
}

#endif
//End By leewheel
