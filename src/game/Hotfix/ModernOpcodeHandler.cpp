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

//By leewheel 2026-07-25 现代客户端(2.5.3) opcode 翻译层实现
#include "Hotfix/ModernOpcodeHandler.h"
#include "Hotfix/OpcodeMapping.h"
#include "Log/Log.h"

namespace ModernOpcode
{
    bool ToLegacy(uint16 modernOpcode, uint16& legacyOpcode)
    {
        int legacy = ModernToLegacyOpcode(modernOpcode);
        if (legacy < 0)
        {
            // 无映射的现代 opcode：记录到缺失opcode日志，便于后续维护补全
            sLog.outMissingOpcode("MODERN: unmapped 2.5.3 opcode 0x%04X (no 2.4.3 equivalent)", modernOpcode);
            return false;
        }
        legacyOpcode = static_cast<uint16>(legacy);
        return true;
    }

    bool ToModern(uint16 legacyOpcode, uint16& modernOpcode)
    {
        int modern = LegacyToModernOpcode(legacyOpcode);
        if (modern < 0)
            return false;
        modernOpcode = static_cast<uint16>(modern);
        return true;
    }

    bool IsKnownModernOpcode(uint16 modernOpcode)
    {
        return ModernToLegacyOpcode(modernOpcode) >= 0;
    }
}
//End By leewheel
