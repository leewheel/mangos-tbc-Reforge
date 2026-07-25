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

//By leewheel 2026-07-25 hotfix数据管理器实现
#include "Hotfix/HotfixDataMgr.h"
#include "Database/DatabaseEnv.h"
#include "Database/QueryResult.h"
#include "Log/Log.h"

#include <memory>

HotfixDataMgr::HotfixDataMgr()
{
}

HotfixDataMgr::~HotfixDataMgr()
{
}

void HotfixDataMgr::LoadAll()
{
    // hotfixes 库为必需库，启动时 Master::_StartDB 已保证连接成功；此处仅做防御性检查
    if (!HotfixDatabase)
    {
        sLog.outError("Hotfix database not connected, cannot load hotfix data");
        return;
    }

    sLog.outString("Loading hotfix data from hotfixes database...");

    uint32 count = 0;

    // 法术集合类（单列表）
    LoadSpellSet("lw_aura_spells", m_auraSpells, count);
    sLog.outString(">> Loaded %u aura spells", count);

    LoadSpellSet("lw_auto_repeat_spells", m_autoRepeatSpells, count);
    sLog.outString(">> Loaded %u auto repeat spells", count);

    LoadSpellSet("lw_melee_spells", m_meleeSpells, count);
    sLog.outString(">> Loaded %u melee spells", count);

    LoadSpellSet("lw_stackable_auras", m_stackableAuras, count);
    sLog.outString(">> Loaded %u stackable auras", count);

    // 映射类
    LoadItemIdToDisplay();
    LoadItemDisplayToFileData();
    LoadItemEnchantVisuals();
    LoadGems();
    LoadCreatureModelCollision();
    LoadItemSpellsData();
    LoadTransports();

    sLog.outString(">> Hotfix data loaded.");
    sLog.outString();
}

// 加载单列法术集合表（列名 SpellId）
void HotfixDataMgr::LoadSpellSet(const char* table, std::unordered_set<uint32>& out, uint32& count)
{
    out.clear();
    count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.PQuery("SELECT SpellId FROM %s", table));
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        out.insert(fields[0].GetUInt32());
        ++count;
    }
    while (result->NextRow());
}

// 物品ID → 显示ID
void HotfixDataMgr::LoadItemIdToDisplay()
{
    m_itemIdToDisplay.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query("SELECT Entry, DisplayId FROM lw_item_id_to_display"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            m_itemIdToDisplay[fields[0].GetUInt32()] = fields[1].GetUInt32();
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u item id -> display id mappings", count);
}

// 显示ID → 文件数据ID
void HotfixDataMgr::LoadItemDisplayToFileData()
{
    m_displayToFileData.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query("SELECT DisplayID, FileDataID FROM lw_item_display_to_filedata"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            m_displayToFileData[fields[0].GetUInt32()] = fields[1].GetUInt32();
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u item display -> filedata mappings", count);
}

// 附魔ID → 物品视觉
void HotfixDataMgr::LoadItemEnchantVisuals()
{
    m_enchantToVisual.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query("SELECT EnchantId, ItemVisual FROM lw_item_enchant_visuals"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            m_enchantToVisual[fields[0].GetUInt32()] = fields[1].GetUInt32();
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u item enchant visuals", count);
}

// 附魔ID → 宝石物品ID
void HotfixDataMgr::LoadGems()
{
    m_gemEnchantToItem.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query("SELECT EnchantId, ItemId FROM lw_gems"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            m_gemEnchantToItem[fields[0].GetUInt32()] = fields[1].GetUInt32();
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u gem entries", count);
}

// 模型ID → 碰撞高度
void HotfixDataMgr::LoadCreatureModelCollision()
{
    m_creatureModelCollision.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query(
        "SELECT ModelId, ModelScale, CollisionHeight, CollisionHeightMounted FROM lw_creature_model_collision"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            LwCreatureModelCollision data;
            data.ModelScale           = fields[1].GetFloat();
            data.CollisionHeight      = fields[2].GetFloat();
            data.CollisionHeightMounted = fields[3].GetFloat();
            m_creatureModelCollision[fields[0].GetUInt32()] = data;
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u creature model collision entries", count);
}

// 物品法术分类/冷却
void HotfixDataMgr::LoadItemSpellsData()
{
    m_itemSpellsData.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query(
        "SELECT ID, Category, RecoveryTime, CategoryRecoveryTime FROM lw_item_spells_data"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            LwItemSpellsData data;
            data.Category            = fields[1].GetUInt32();
            data.RecoveryTime        = fields[2].GetUInt32();
            data.CategoryRecoveryTime = fields[3].GetUInt32();
            m_itemSpellsData[fields[0].GetUInt32()] = data;
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u item spells data entries", count);
}

// 传送 Entry → Period
void HotfixDataMgr::LoadTransports()
{
    m_transportPeriod.clear();
    uint32 count = 0;

    std::unique_ptr<QueryResult> result(HotfixDatabase.Query("SELECT Entry, Period FROM lw_transports"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            m_transportPeriod[fields[0].GetUInt32()] = fields[1].GetUInt32();
            ++count;
        }
        while (result->NextRow());
    }
    sLog.outString(">> Loaded %u transport entries", count);
}

// ---- 查询接口实现 ----
bool HotfixDataMgr::GetItemDisplayId(uint32 itemId, uint32& displayId) const
{
    auto it = m_itemIdToDisplay.find(itemId);
    if (it == m_itemIdToDisplay.end())
        return false;
    displayId = it->second;
    return true;
}

bool HotfixDataMgr::GetItemDisplayFileDataId(uint32 displayId, uint32& fileDataId) const
{
    auto it = m_displayToFileData.find(displayId);
    if (it == m_displayToFileData.end())
        return false;
    fileDataId = it->second;
    return true;
}

bool HotfixDataMgr::GetItemEnchantVisual(uint32 enchantId, uint32& itemVisual) const
{
    auto it = m_enchantToVisual.find(enchantId);
    if (it == m_enchantToVisual.end())
        return false;
    itemVisual = it->second;
    return true;
}

bool HotfixDataMgr::GetGemItemId(uint32 enchantId, uint32& itemId) const
{
    auto it = m_gemEnchantToItem.find(enchantId);
    if (it == m_gemEnchantToItem.end())
        return false;
    itemId = it->second;
    return true;
}

bool HotfixDataMgr::GetCreatureModelCollision(uint32 modelId, LwCreatureModelCollision& out) const
{
    auto it = m_creatureModelCollision.find(modelId);
    if (it == m_creatureModelCollision.end())
        return false;
    out = it->second;
    return true;
}

bool HotfixDataMgr::GetItemSpellsData(uint32 spellId, LwItemSpellsData& out) const
{
    auto it = m_itemSpellsData.find(spellId);
    if (it == m_itemSpellsData.end())
        return false;
    out = it->second;
    return true;
}
//End By leewheel
