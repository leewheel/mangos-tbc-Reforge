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

//By leewheel 2026-07-25 现代客户端(2.5.3) opcode 翻译层
//用途：在 2.5.3 客户端直连时，把现代 opcode 数值翻译为 mangos 内部(2.4.3)数值，
//      以及反向翻译。映射数据见 OpcodeMapping.h（由代理两版本枚举派生）。
#ifndef MANGOS_HFX_MODERNOPCODEHANDLER_H
#define MANGOS_HFX_MODERNOPCODEHANDLER_H

#include "Common.h"

namespace ModernOpcode
{
    // 现代(2.5.3) opcode → mangos 内部(2.4.3) opcode；无对应返回 false
    bool ToLegacy(uint16 modernOpcode, uint16& legacyOpcode);

    // mangos 内部(2.4.3) opcode → 现代(2.5.3) opcode；无对应返回 false
    bool ToModern(uint16 legacyOpcode, uint16& modernOpcode);

    // 该现代 opcode 是否有映射（用于快速判断）
    bool IsKnownModernOpcode(uint16 modernOpcode);
}

#endif
//End By leewheel
