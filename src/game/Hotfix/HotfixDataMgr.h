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

//By leewheel 2026-07-25 hotfix数据管理器
//用途：启动时从 hotfixes 库加载数据到内存，
//      供后续 2.5.3 客户端直连（现代协议层）使用，杜绝读 CSV 或硬编码（开发目标3）。
#ifndef MANGOS_HFX_HOTFIXDATAMGR_H
#define MANGOS_HFX_HOTFIXDATAMGR_H

#include "Common.h"
#include "Policies/Singleton.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

// 生物模型碰撞高度（lw_creature_model_collision）
struct LwCreatureModelCollision
{
    float ModelScale;
    float CollisionHeight;
    float CollisionHeightMounted;
};

// 物品法术分类/冷却（lw_item_spells_data）
struct LwItemSpellsData
{
    uint32 Category;
    uint32 RecoveryTime;
    uint32 CategoryRecoveryTime;
};

class HotfixDataMgr
{
    public:
        HotfixDataMgr();
        ~HotfixDataMgr();

        // 启动时从 hotfixes 库加载全部数据到内存
        void LoadAll();

        // ---- 法术集合查询 ----
        bool IsAuraSpell(uint32 spellId) const        { return m_auraSpells.count(spellId) != 0; }
        bool IsAutoRepeatSpell(uint32 spellId) const  { return m_autoRepeatSpells.count(spellId) != 0; }
        bool IsMeleeSpell(uint32 spellId) const       { return m_meleeSpells.count(spellId) != 0; }
        bool IsStackableAura(uint32 spellId) const    { return m_stackableAuras.count(spellId) != 0; }

        // ---- 映射查询 ----
        // 物品ID → 显示ID（lw_item_id_to_display）
        bool GetItemDisplayId(uint32 itemId, uint32& displayId) const;
        // 显示ID → 文件数据ID（lw_item_display_to_filedata）
        bool GetItemDisplayFileDataId(uint32 displayId, uint32& fileDataId) const;
        // 附魔ID → 物品视觉（lw_item_enchant_visuals）
        bool GetItemEnchantVisual(uint32 enchantId, uint32& itemVisual) const;
        // 附魔ID → 宝石物品ID（lw_gems）
        bool GetGemItemId(uint32 enchantId, uint32& itemId) const;
        // 模型ID → 碰撞高度（lw_creature_model_collision）
        bool GetCreatureModelCollision(uint32 modelId, LwCreatureModelCollision& out) const;
        // 物品法术数据（lw_item_spells_data）
        bool GetItemSpellsData(uint32 spellId, LwItemSpellsData& out) const;

    private:
        // 各表加载子函数
        void LoadSpellSet(const char* table, std::unordered_set<uint32>& out, uint32& count);
        void LoadItemIdToDisplay();
        void LoadItemDisplayToFileData();
        void LoadItemEnchantVisuals();
        void LoadGems();
        void LoadCreatureModelCollision();
        void LoadItemSpellsData();
        void LoadTransports();

        // 法术集合
        std::unordered_set<uint32> m_auraSpells;
        std::unordered_set<uint32> m_autoRepeatSpells;
        std::unordered_set<uint32> m_meleeSpells;
        std::unordered_set<uint32> m_stackableAuras;

        // 映射表
        std::unordered_map<uint32, uint32> m_itemIdToDisplay;        // 物品ID → 显示ID
        std::unordered_map<uint32, uint32> m_displayToFileData;      // 显示ID → 文件数据ID
        std::unordered_map<uint32, uint32> m_enchantToVisual;        // 附魔ID → 物品视觉
        std::unordered_map<uint32, uint32> m_gemEnchantToItem;       // 附魔ID → 宝石物品ID
        std::unordered_map<uint32, LwCreatureModelCollision> m_creatureModelCollision;
        std::unordered_map<uint32, LwItemSpellsData> m_itemSpellsData;
        std::unordered_map<uint32, uint32> m_transportPeriod;        // 传送Entry → Period
};

#define sHotfixDataMgr MaNGOS::Singleton<HotfixDataMgr>::Instance()

#endif
//End By leewheel
