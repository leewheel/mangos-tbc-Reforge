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

//By leewheel 2026-07-25 现代 world 协议加密工具层实现
#include "Hotfix/ModernCrypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

#include <cstring>

namespace ModernCrypto
{
    // 种子常量（逐字取自代理 WorldSocket.cs:61-64 与 AuthenticationPackets.cs:471）
    const uint8 AuthCheckSeed[16]       = { 0xC5,0xC6,0x98,0x95,0x76,0x3F,0x1D,0xCD,0xB6,0xA1,0x37,0x28,0xB3,0x12,0xFF,0x8A };
    const uint8 SessionKeySeed[16]      = { 0x58,0xCB,0xCF,0x40,0xFE,0x2E,0xCE,0xA6,0x5A,0x90,0xB8,0x01,0x68,0x6C,0x28,0x0B };
    const uint8 ContinuedSessionSeed[16]= { 0x16,0xAD,0x0C,0xD4,0x46,0xF9,0x4F,0xB2,0xEF,0x7D,0xEA,0x2A,0x17,0x66,0x4D,0x2F };
    const uint8 EncryptionKeySeed[16]   = { 0xE9,0x75,0x3C,0x50,0x90,0x93,0x61,0xDA,0x3B,0x07,0xEE,0xFA,0xFF,0x9D,0x41,0xB8 };
    const uint8 EnableEncryptionSeed[16]= { 0x90,0x9C,0xD0,0x50,0x5A,0x2C,0x14,0xDD,0x5C,0x2C,0xC0,0x64,0x14,0xF3,0xFE,0xC9 };

    void Sha256(const uint8* data, size_t len, uint8 out[SHA256_LEN])
    {
        unsigned int outLen = SHA256_LEN;
        EVP_Digest(data, len, out, &outLen, EVP_sha256(), nullptr);
    }

    void Sha256Two(const uint8* a, size_t alen, const uint8* b, size_t blen, uint8 out[SHA256_LEN])
    {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        unsigned int outLen = SHA256_LEN;
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, a, alen);
        EVP_DigestUpdate(ctx, b, blen);
        EVP_DigestFinal_ex(ctx, out, &outLen);
        EVP_MD_CTX_free(ctx);
    }

    void HmacSha256(const uint8* key, size_t keyLen,
                    const uint8* d1, size_t l1,
                    const uint8* d2, size_t l2,
                    const uint8* d3, size_t l3,
                    uint8 out[SHA256_LEN])
    {
        // 用 EVP_MAC 上下文逐段 Update，避免拼接临时缓冲
        EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, const_cast<char*>("SHA256"), 0);
        params[1] = OSSL_PARAM_construct_end();
        EVP_MAC_init(ctx, key, keyLen, params);
        if (d1 && l1) EVP_MAC_update(ctx, d1, l1);
        if (d2 && l2) EVP_MAC_update(ctx, d2, l2);
        if (d3 && l3) EVP_MAC_update(ctx, d3, l3);
        size_t outLen = SHA256_LEN;
        EVP_MAC_final(ctx, out, &outLen, SHA256_LEN);
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
    }

    SessionKeyGenerator::SessionKeyGenerator(const uint8* buff, size_t len)
    {
        size_t half = len / 2;
        Sha256(buff, half, m_o1.data());
        Sha256(buff + half, len - half, m_o2.data());
        FillUp();
    }

    void SessionKeyGenerator::FillUp()
    {
        // o0 = SHA256(o1 || o0 || o2)，初始 o0 全 0
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        unsigned int outLen = SHA256_LEN;
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, m_o1.data(), SHA256_LEN);
        EVP_DigestUpdate(ctx, m_o0.data(), SHA256_LEN);
        EVP_DigestUpdate(ctx, m_o2.data(), SHA256_LEN);
        EVP_DigestFinal_ex(ctx, m_o0.data(), &outLen);
        EVP_MD_CTX_free(ctx);
        m_taken = 0;
    }

    void SessionKeyGenerator::Generate(uint8* buf, size_t sz)
    {
        for (size_t i = 0; i < sz; ++i)
        {
            if (m_taken == SHA256_LEN)
                FillUp();
            buf[i] = m_o0[m_taken];
            ++m_taken;
        }
    }

    bool VerifyAuthDigest(const uint8* sessionKey, size_t sessionKeyLen,
                          const uint8* seed,
                          const uint8 localChallenge[16],
                          const uint8 serverChallenge[16],
                          const uint8 digest[24])
    {
        // digestKey = SHA256(K || seed)
        std::vector<uint8> kseed(sessionKey, sessionKey + sessionKeyLen);
        kseed.insert(kseed.end(), seed, seed + 16);
        uint8 digestKey[SHA256_LEN];
        Sha256(kseed.data(), kseed.size(), digestKey);

        // expected = HMAC-SHA256(digestKey, localChallenge || serverChallenge || AuthCheckSeed)
        uint8 expected[SHA256_LEN];
        HmacSha256(digestKey, SHA256_LEN,
                   localChallenge, 16,
                   serverChallenge, 16,
                   AuthCheckSeed, 16,
                   expected);

        // 客户端 digest 为 24 字节，比较前 24 字节
        return memcmp(expected, digest, 24) == 0;
    }

    void DeriveSessionKey40(const uint8* sessionKey, size_t sessionKeyLen,
                            const uint8 localChallenge[16],
                            const uint8 serverChallenge[16],
                            uint8 out40[40])
    {
        // keyData = SHA256(K)
        uint8 keyData[SHA256_LEN];
        Sha256(sessionKey, sessionKeyLen, keyData);

        // hmac = HMAC-SHA256(keyData, serverChallenge || localChallenge || SessionKeySeed)
        uint8 hmacOut[SHA256_LEN];
        HmacSha256(keyData, SHA256_LEN,
                   serverChallenge, 16,
                   localChallenge, 16,
                   SessionKeySeed, 16,
                   hmacOut);

        // out40 = SessionKeyGenerator(hmac).Generate(40)
        SessionKeyGenerator gen(hmacOut, SHA256_LEN);
        gen.Generate(out40, 40);
    }

    void DeriveEncryptKey16(const uint8 sessionKey40[40],
                            const uint8 localChallenge[16],
                            const uint8 serverChallenge[16],
                            uint8 out16[16])
    {
        // hmac = HMAC-SHA256(sessionKey40, localChallenge || serverChallenge || EncryptionKeySeed)
        uint8 hmacOut[SHA256_LEN];
        HmacSha256(sessionKey40, 40,
                   localChallenge, 16,
                   serverChallenge, 16,
                   EncryptionKeySeed, 16,
                   hmacOut);
        // 只取前 16 字节
        memcpy(out16, hmacOut, 16);
    }

    //=====================================================================
    // Blizzard realm-list 签名 RSA 私钥（2048位，取自代理 RsaStore.RSAParameters，大端字节序）
    //=====================================================================
    namespace
    {
        const uint8 kRsaModulus[256] = {
            0xee,0xb3,0xdc,0xd4,0xd3,0xc3,0xb4,0x54,0x51,0xce,0x66,0x5b,0xcb,0x32,0xb8,0xf0,
            0xf7,0x92,0x53,0xc6,0x19,0xf2,0x0c,0x85,0x2f,0x8a,0x26,0xa9,0x7a,0x45,0x9f,0x60,
            0xc4,0xeb,0xcd,0xea,0x7f,0x8d,0x59,0xd8,0x57,0xb2,0x60,0x7b,0x09,0x4c,0x9b,0x68,
            0xb8,0xc7,0xed,0xef,0x1e,0x80,0x0d,0xe6,0x6b,0x37,0x5b,0x53,0x90,0xeb,0x18,0x13,
            0x0d,0x7f,0x43,0x64,0x83,0xda,0x98,0xe6,0xac,0xc2,0x30,0xa2,0x82,0xa5,0xc6,0xcb,
            0xc7,0xfb,0x86,0x9f,0x9f,0xa9,0x02,0x6a,0x03,0x49,0xc5,0x38,0xfb,0xc0,0xc8,0x55,
            0xcc,0xc0,0xce,0x25,0x91,0xbe,0x85,0xcf,0xd1,0xd1,0x37,0xce,0xcc,0x83,0xd2,0xea,
            0x30,0x80,0x07,0x7b,0x80,0x9f,0x9d,0x44,0x54,0x22,0x29,0xbe,0x86,0xda,0xdb,0x48,
            0xc5,0xa9,0xf9,0x13,0x36,0x95,0x23,0x76,0xf1,0x0e,0xdc,0x84,0x0d,0x94,0x02,0x12,
            0xa8,0x97,0xf3,0x3b,0x14,0xee,0xaa,0x6f,0x98,0x05,0x27,0x4e,0x1f,0xa3,0x60,0xa5,
            0xa9,0xda,0xd8,0x17,0xdf,0x33,0xcb,0xe2,0x13,0x54,0x8b,0x18,0xb0,0xca,0xb9,0xbb,
            0x88,0x64,0x06,0xdf,0x75,0xa6,0xd7,0x61,0x00,0xbb,0xb0,0x5a,0x0e,0x7a,0xd4,0x77,
            0x08,0x4d,0x15,0xe2,0x10,0x83,0xb0,0x04,0xaa,0x9e,0x8b,0x77,0xa9,0x06,0x89,0x5d,
            0x08,0x5d,0x0f,0xb8,0x2e,0x6b,0xc1,0xcb,0x64,0xcf,0x6e,0x5c,0xdb,0x4f,0x58,0x65,
            0x08,0x51,0xfb,0x0d,0x48,0x1a,0x6f,0xb6,0x3d,0x1f,0x0b,0xdd,0xfe,0x1b,0x1d,0xf0,
            0xbf,0xb0,0x27,0x6b,0xf5,0x8e,0xbc,0xc7,0x40,0x01,0xff,0xa7,0x0b,0x80,0xd6,0x5f
        };
        const uint8 kRsaExponent[3] = { 0x01,0x00,0x01 };
        const uint8 kRsaPrivateExp[256] = {
            0x22,0x9a,0xe6,0xaf,0xe0,0x07,0x66,0x34,0x37,0x2b,0xe2,0x00,0xfa,0xc3,0x5e,0xb6,
            0x68,0x5d,0xc9,0x51,0x55,0xdf,0x96,0x5b,0x14,0x9a,0x45,0xa2,0x9a,0x3c,0x4f,0xaf,
            0xba,0xbc,0xa8,0xbc,0x8f,0x43,0x51,0xbc,0x20,0x72,0x96,0xb4,0x1f,0x94,0x00,0x8f,
            0xbd,0x02,0x17,0x07,0x6c,0x77,0x8a,0x0c,0x56,0x8c,0xce,0xeb,0x9d,0x7d,0xc7,0x9e,
            0xb3,0x7d,0x38,0xaa,0xf0,0xc6,0x97,0x16,0x12,0x03,0x91,0x03,0x6e,0x47,0x54,0x3b,
            0xa4,0xc1,0x5d,0x31,0xf4,0xf6,0x8e,0x88,0x09,0xf3,0xfe,0xe8,0x94,0xee,0xcc,0xdc,
            0x4b,0x73,0xc4,0x2f,0x04,0x23,0x07,0xc9,0x2a,0x14,0xd7,0xaf,0x5e,0x4c,0xda,0x1d,
            0xe3,0x6c,0x1c,0x29,0x96,0x6b,0x0d,0x64,0xa3,0x81,0xd4,0x65,0x6f,0xad,0x78,0xce,
            0x9b,0x52,0xad,0x39,0x9e,0x02,0x4d,0x33,0x34,0x5a,0xb3,0xda,0x2d,0x50,0xd3,0xf5,
            0xac,0x7c,0xa7,0x29,0x23,0x98,0x5c,0x35,0xea,0xf1,0x8f,0x8f,0xf4,0x79,0x0e,0x4c,
            0xbd,0x56,0x96,0x9b,0xb5,0xf6,0x4e,0xbb,0xf0,0x04,0x5b,0x6e,0x7d,0x5c,0x31,0x22,
            0x42,0x04,0xeb,0x07,0x81,0x20,0xf9,0x2e,0x06,0x26,0x31,0xea,0x03,0x33,0xd9,0x06,
            0x63,0x32,0xff,0x18,0x65,0x0c,0xae,0x28,0x31,0x77,0x9f,0xa9,0x74,0x9c,0x7c,0x3e,
            0x30,0xd1,0x1c,0x6e,0xb8,0x21,0x6b,0xea,0x5c,0x4b,0x3d,0x9c,0xf4,0x4b,0x7e,0x41,
            0x2b,0x59,0x08,0x5a,0x62,0x24,0xba,0xff,0xbd,0x79,0x0b,0x88,0xe0,0x7a,0xf5,0x0b,
            0x25,0x70,0x72,0x1e,0x1f,0x91,0xfb,0xeb,0xa7,0xce,0x31,0xf2,0xdb,0xc0,0x16,0x79
        };

        // SHA256 的 DigestInfo ASN.1 前缀（19字节），用于把 32 字节摘要包成 PKCS1 v1.5 待签数据
        const uint8 kSha256DigestInfoPrefix[19] = {
            0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
        };

        // 用 n/e/d 构造 EVP_PKEY（RSA 私钥，OpenSSL 3.x 非弃用方式）
        EVP_PKEY* BuildRsaKey()
        {
            BIGNUM* n = BN_bin2bn(kRsaModulus, sizeof(kRsaModulus), nullptr);
            BIGNUM* e = BN_bin2bn(kRsaExponent, sizeof(kRsaExponent), nullptr);
            BIGNUM* d = BN_bin2bn(kRsaPrivateExp, sizeof(kRsaPrivateExp), nullptr);

            OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n);
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e);
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_D, d);
            OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);

            EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            EVP_PKEY* pkey = nullptr;
            EVP_PKEY_fromdata_init(kctx);
            EVP_PKEY_fromdata(kctx, &pkey, EVP_PKEY_KEYPAIR, params);

            OSSL_PARAM_free(params);
            OSSL_PARAM_BLD_free(bld);
            EVP_PKEY_CTX_free(kctx);
            BN_free(n);
            BN_free(e);
            BN_free(d);
            return pkey;
        }
    }

    bool SignEnterEncryptedMode(const uint8 encryptKey16[16], bool enabled, uint8 signatureOut[256])
    {
        // toSign = HMAC-SHA256(encryptKey16, [enabled] || EnableEncryptionSeed)
        uint8 enabledByte = enabled ? 1 : 0;
        uint8 toSign[SHA256_LEN];
        HmacSha256(encryptKey16, 16,
                   &enabledByte, 1,
                   EnableEncryptionSeed, 16,
                   nullptr, 0,
                   toSign);

        // 待签数据 = DigestInfo前缀 || toSign（共 51 字节）
        uint8 tbs[19 + SHA256_LEN];
        memcpy(tbs, kSha256DigestInfoPrefix, 19);
        memcpy(tbs + 19, toSign, SHA256_LEN);

        EVP_PKEY* pkey = BuildRsaKey();
        if (!pkey)
            return false;

        EVP_PKEY_CTX* sctx = EVP_PKEY_CTX_new(pkey, nullptr);
        bool ok = false;
        size_t sigLen = 256;
        if (EVP_PKEY_sign_init(sctx) > 0 &&
            EVP_PKEY_CTX_set_rsa_padding(sctx, RSA_PKCS1_PADDING) > 0 &&
            EVP_PKEY_sign(sctx, signatureOut, &sigLen, tbs, sizeof(tbs)) > 0 &&
            sigLen == 256)
        {
            // 客户端期望大端整数，.NET 的签名结果做了 Reverse()，这里同样反转字节序
            for (int i = 0; i < 128; ++i)
                std::swap(signatureOut[i], signatureOut[255 - i]);
            ok = true;
        }

        EVP_PKEY_CTX_free(sctx);
        EVP_PKEY_free(pkey);
        return ok;
    }
}
//End By leewheel
