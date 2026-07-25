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

//By leewheel 2026-07-25 BNet 协议基础编译单元
//用途：确保 ProtobufStream.h / BnetHeader.h / BnetConstants.h / BnetMessages.h 参与编译校验。
//服务原始哈希、RPC 错误码、方法ID 已集中在 BnetConstants.h；
//登录链路 protobuf 消息编解码集中在 BnetMessages.h。
#include "Hotfix/ProtobufStream.h"
#include "Hotfix/BnetHeader.h"
#include "Hotfix/BnetConstants.h"
#include "Hotfix/BnetMessages.h"
//End By leewheel
