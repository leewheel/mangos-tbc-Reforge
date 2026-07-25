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

//By leewheel 2026-07-25 现代 World 加密层实现（AES-128-GCM，OpenSSL EVP）
#include "Hotfix/ModernWorldCrypt.h"

#include <openssl/evp.h>

namespace
{
    const uint32 SERVER_NONCE_SUFFIX = 0x52565253;   // "SRVR"（小端存储即 'S''R''V''R'）
    const uint32 CLIENT_NONCE_SUFFIX = 0x544E4C43;   // "CLNT"

    // 构造12字节 nonce：计数器(8字节小端) + 方向后缀(4字节小端)
    void BuildNonce(uint8* nonce, uint64 counter, uint32 suffix)
    {
        for (int i = 0; i < 8; ++i)
            nonce[i] = uint8((counter >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; ++i)
            nonce[8 + i] = uint8((suffix >> (8 * i)) & 0xFF);
    }
}

ModernWorldCrypt::ModernWorldCrypt()
{
}

ModernWorldCrypt::~ModernWorldCrypt()
{
}

void ModernWorldCrypt::Initialize(const uint8* key)
{
    std::memcpy(m_key, key, 16);
    m_serverCounter = 0;
    m_clientCounter = 0;
    m_initialized = true;
}

bool ModernWorldCrypt::Encrypt(uint8* data, size_t len, uint8* tag)
{
    if (!m_initialized)
    {
        // 未初始化（握手阶段）不加密，仅推进计数器（与代理行为一致）
        ++m_serverCounter;
        return true;
    }

    uint8 nonce[12];
    BuildNonce(nonce, m_serverCounter, SERVER_NONCE_SUFFIX);

    bool ok = false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx)
    {
        int outLen = 0;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_key, nonce) == 1 &&
            EVP_EncryptUpdate(ctx, data, &outLen, data, int(len)) == 1 &&
            EVP_EncryptFinal_ex(ctx, data + outLen, &outLen) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 12, tag) == 1)
        {
            ok = true;
        }
        EVP_CIPHER_CTX_free(ctx);
    }

    ++m_serverCounter;
    return ok;
}

bool ModernWorldCrypt::Decrypt(uint8* data, size_t len, const uint8* tag)
{
    if (!m_initialized)
    {
        // 未初始化（握手阶段）不解密，仅推进计数器
        ++m_clientCounter;
        return true;
    }

    uint8 nonce[12];
    BuildNonce(nonce, m_clientCounter, CLIENT_NONCE_SUFFIX);

    bool ok = false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx)
    {
        int outLen = 0;
        // 设置期望的tag用于校验（GCM 解密前需通过 SET_TAG 传入）
        if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, m_key, nonce) == 1 &&
            EVP_DecryptUpdate(ctx, data, &outLen, data, int(len)) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 12, const_cast<uint8*>(tag)) == 1 &&
            EVP_DecryptFinal_ex(ctx, data + outLen, &outLen) == 1)
        {
            ok = true;   // 认证标签校验通过
        }
        EVP_CIPHER_CTX_free(ctx);
    }

    ++m_clientCounter;
    return ok;
}
//End By leewheel
