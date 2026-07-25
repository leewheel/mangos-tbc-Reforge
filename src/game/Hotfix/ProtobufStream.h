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

//By leewheel 2026-07-25 protobuf wire 格式读写工具（BNet 协议基础）
//用途：不依赖 protoc，直接按 protobuf 线格式解析/构造 BNet 的 Header 等消息。
//参考：https://protobuf.dev/programming-guides/encoding/
#ifndef MANGOS_HFX_PROTOBUFSTREAM_H
#define MANGOS_HFX_PROTOBUFSTREAM_H

#include "Common.h"

#include <string>
#include <vector>

// protobuf wire 类型
enum ProtobufWireType
{
    PB_WIRE_VARINT           = 0,   // int32/int64/uint32/uint64/bool/enum
    PB_WIRE_FIXED64          = 1,   // fixed64/sfixed64/double
    PB_WIRE_LENGTH_DELIMITED = 2,   // string/bytes/嵌套消息/packed
    PB_WIRE_FIXED32          = 5,   // fixed32/sfixed32/float
};

// protobuf 读取器：按字段顺序解析线格式
class ProtobufReader
{
    public:
        ProtobufReader(const uint8* data, size_t len) : m_data(data), m_len(len), m_pos(0) {}

        bool Eof() const { return m_pos >= m_len; }
        size_t Remaining() const { return m_len - m_pos; }

        // 读取 tag，返回字段号；wireType 输出 wire 类型；失败返回 false
        bool ReadTag(uint32& fieldNumber, uint32& wireType)
        {
            uint64 key = 0;
            if (!ReadVarint(key))
                return false;
            wireType = uint32(key & 0x7);
            fieldNumber = uint32(key >> 3);
            return true;
        }

        // 读取变长整数
        bool ReadVarint(uint64& value)
        {
            value = 0;
            int shift = 0;
            while (m_pos < m_len)
            {
                uint8 b = m_data[m_pos++];
                value |= uint64(b & 0x7F) << shift;
                if ((b & 0x80) == 0)
                    return true;
                shift += 7;
                if (shift >= 64)
                    return false;
            }
            return false;
        }

        // 读取 32/64 位定长（小端）
        bool ReadFixed32(uint32& value)
        {
            if (Remaining() < 4) return false;
            value = uint32(m_data[m_pos]) | (uint32(m_data[m_pos + 1]) << 8) |
                    (uint32(m_data[m_pos + 2]) << 16) | (uint32(m_data[m_pos + 3]) << 24);
            m_pos += 4;
            return true;
        }

        bool ReadFixed64(uint64& value)
        {
            if (Remaining() < 8) return false;
            value = 0;
            for (int i = 0; i < 8; ++i)
                value |= uint64(m_data[m_pos + i]) << (8 * i);
            m_pos += 8;
            return true;
        }

        // 读取长度分隔数据（string/bytes/嵌套消息），返回指向内部缓冲的指针与长度
        bool ReadLengthDelimited(const uint8*& out, size_t& outLen)
        {
            uint64 len = 0;
            if (!ReadVarint(len))
                return false;
            if (Remaining() < len)
                return false;
            out = m_data + m_pos;
            outLen = size_t(len);
            m_pos += size_t(len);
            return true;
        }

        bool ReadString(std::string& value)
        {
            const uint8* p = nullptr;
            size_t n = 0;
            if (!ReadLengthDelimited(p, n))
                return false;
            value.assign(reinterpret_cast<const char*>(p), n);
            return true;
        }

        // 跳过当前字段（已知 wire 类型）
        bool SkipField(uint32 wireType)
        {
            switch (wireType)
            {
                case PB_WIRE_VARINT:
                {
                    uint64 v;
                    return ReadVarint(v);
                }
                case PB_WIRE_FIXED64:
                    m_pos += 8;
                    return m_pos <= m_len;
                case PB_WIRE_LENGTH_DELIMITED:
                {
                    const uint8* p;
                    size_t n;
                    return ReadLengthDelimited(p, n);
                }
                case PB_WIRE_FIXED32:
                    m_pos += 4;
                    return m_pos <= m_len;
                default:
                    return false;
            }
        }

    private:
        const uint8* m_data;
        size_t m_len;
        size_t m_pos;
};

// protobuf 写入器：构造线格式消息
class ProtobufWriter
{
    public:
        // 写入 tag（字段号 + wire 类型）
        void WriteTag(uint32 fieldNumber, uint32 wireType)
        {
            WriteVarint((uint64(fieldNumber) << 3) | wireType);
        }

        void WriteVarint(uint64 value)
        {
            while (value >= 0x80)
            {
                m_buf.push_back(uint8(value) | 0x80);
                value >>= 7;
            }
            m_buf.push_back(uint8(value));
        }

        void WriteUInt32(uint32 fieldNumber, uint32 value)
        {
            WriteTag(fieldNumber, PB_WIRE_VARINT);
            WriteVarint(value);
        }

        void WriteUInt64(uint32 fieldNumber, uint64 value)
        {
            WriteTag(fieldNumber, PB_WIRE_VARINT);
            WriteVarint(value);
        }

        void WriteFixed32(uint32 fieldNumber, uint32 value)
        {
            WriteTag(fieldNumber, PB_WIRE_FIXED32);
            m_buf.push_back(uint8(value & 0xFF));
            m_buf.push_back(uint8((value >> 8) & 0xFF));
            m_buf.push_back(uint8((value >> 16) & 0xFF));
            m_buf.push_back(uint8((value >> 24) & 0xFF));
        }

        void WriteFixed64(uint32 fieldNumber, uint64 value)
        {
            WriteTag(fieldNumber, PB_WIRE_FIXED64);
            for (int i = 0; i < 8; ++i)
                m_buf.push_back(uint8((value >> (8 * i)) & 0xFF));
        }

        void WriteString(uint32 fieldNumber, const std::string& value)
        {
            WriteTag(fieldNumber, PB_WIRE_LENGTH_DELIMITED);
            WriteVarint(value.size());
            m_buf.insert(m_buf.end(), value.begin(), value.end());
        }

        void WriteBytes(uint32 fieldNumber, const uint8* data, size_t len)
        {
            WriteTag(fieldNumber, PB_WIRE_LENGTH_DELIMITED);
            WriteVarint(len);
            m_buf.insert(m_buf.end(), data, data + len);
        }

        const std::vector<uint8>& Data() const { return m_buf; }
        size_t Size() const { return m_buf.size(); }

    private:
        std::vector<uint8> m_buf;
};

#endif
//End By leewheel
