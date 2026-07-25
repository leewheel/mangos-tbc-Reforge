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
#include <cstring>

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

// ============================================================================
// By leewheel 2026-07-25 DB2 记录查询与序列化
// 用途：CMSG_DB_QUERY_BULK 响应时，从 hotfixes 库查询记录并序列化为
//       2.5.3 客户端期望的 DB2 二进制格式。
// 格式参考：HermesProxy GameData.cs WriteItemHotfix / WriteItemSparseHotfix /
//           HotfixHandler.cs HandleDbQueryBulk (BroadcastText)
// ============================================================================

// ---- 辅助写入函数 ----
namespace {
    inline void WriteU8(std::vector<uint8>& buf, uint8 v)  { buf.push_back(v); }
    inline void WriteI8(std::vector<uint8>& buf, int8 v)   { buf.push_back(uint8(v)); }
    inline void WriteU16(std::vector<uint8>& buf, uint16 v) {
        buf.push_back(uint8(v & 0xFF)); buf.push_back(uint8((v >> 8) & 0xFF));
    }
    inline void WriteI16(std::vector<uint8>& buf, int16 v) {
        uint16 u; std::memcpy(&u, &v, 2);
        buf.push_back(uint8(u & 0xFF)); buf.push_back(uint8((u >> 8) & 0xFF));
    }
    inline void WriteU32(std::vector<uint8>& buf, uint32 v) {
        buf.push_back(uint8(v & 0xFF)); buf.push_back(uint8((v >> 8) & 0xFF));
        buf.push_back(uint8((v >> 16) & 0xFF)); buf.push_back(uint8((v >> 24) & 0xFF));
    }
    inline void WriteI32(std::vector<uint8>& buf, int32 v) {
        uint32 u; std::memcpy(&u, &v, 4);
        buf.push_back(uint8(u & 0xFF)); buf.push_back(uint8((u >> 8) & 0xFF));
        buf.push_back(uint8((u >> 16) & 0xFF)); buf.push_back(uint8((u >> 24) & 0xFF));
    }
    inline void WriteU64(std::vector<uint8>& buf, uint64 v) {
        for (int i = 0; i < 8; ++i) buf.push_back(uint8((v >> (i * 8)) & 0xFF));
    }
    inline void WriteI64(std::vector<uint8>& buf, int64 v) {
        uint64 u; std::memcpy(&u, &v, 8);
        WriteU64(buf, u);
    }
    inline void WriteFloat(std::vector<uint8>& buf, float v) {
        uint32 u; std::memcpy(&u, &v, 4);
        WriteU32(buf, u);
    }
    inline void WriteCString(std::vector<uint8>& buf, const std::string& s) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0); // null 终结符
    }
}

// ---- 按表哈希分发 ----
bool HotfixDataMgr::GetDb2RecordData(uint32 tableHash, uint32 recordId, std::vector<uint8>& outData)
{
    if (!HotfixDatabase)
        return false;

    switch (tableHash)
    {
        case DB2HASH_Item:
            return SerializeItemRecord(recordId, outData);
        case DB2HASH_ItemSparse:
            return SerializeItemSparseRecord(recordId, outData);
        case DB2HASH_BroadcastText:
            return SerializeBroadcastTextRecord(recordId, outData);
        case DB2HASH_ItemEffect:
            return SerializeItemEffectRecord(recordId, outData);
        case DB2HASH_ItemAppearance:
            return SerializeItemAppearanceRecord(recordId, outData);
        case DB2HASH_ItemModifiedAppearance:
            return SerializeItemModifiedAppearanceRecord(recordId, outData);
        default:
            return false;
    }
}

// ---- Item 表序列化（格式参考 HermesProxy WriteItemHotfix） ----
bool HotfixDataMgr::SerializeItemRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT ClassID, SubclassID, Material, InventoryType, RequiredLevel, "
        "SheatheType, RandomSelect, ItemRandomSuffixGroupID, SoundOverrideSubclassID, "
        "ScalingStatDistributionID, IconFileDataID, ItemGroupSoundsID, ContentTuningID, "
        "MaxDurability, AmmunitionType, "
        "DamageType1, DamageType2, DamageType3, DamageType4, DamageType5, "
        "Resistances1, Resistances2, Resistances3, Resistances4, Resistances5, Resistances6, Resistances7, "
        "MinDamage1, MinDamage2, MinDamage3, MinDamage4, MinDamage5, "
        "MaxDamage1, MaxDamage2, MaxDamage3, MaxDamage4, MaxDamage5 "
        "FROM item WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();

    outData.clear();
    outData.reserve(128);

    WriteU8(outData,  f[0].GetUInt8());   // ClassID
    WriteU8(outData,  f[1].GetUInt8());   // SubclassID
    WriteU8(outData,  f[2].GetUInt8());   // Material
    WriteI8(outData,  int8(f[3].GetInt32()));    // InventoryType
    WriteI32(outData, f[4].GetInt32());   // RequiredLevel
    WriteU8(outData,  f[5].GetUInt8());   // SheatheType
    WriteU16(outData, f[6].GetUInt16());  // RandomSelect → RandomProperty
    WriteU16(outData, f[7].GetUInt16());  // ItemRandomSuffixGroupID
    WriteI8(outData,  int8(f[8].GetInt32()));    // SoundOverrideSubclassID
    WriteU16(outData, f[9].GetUInt16());  // ScalingStatDistributionID
    WriteI32(outData, f[10].GetInt32());  // IconFileDataID
    WriteU8(outData,  f[11].GetUInt8());  // ItemGroupSoundsID
    WriteI32(outData, f[12].GetInt32());  // ContentTuningID
    WriteU32(outData, f[13].GetUInt32()); // MaxDurability
    WriteU8(outData,  f[14].GetUInt8());  // AmmunitionType → AmmoType
    // DamageType 1-5
    for (int i = 0; i < 5; ++i)
        WriteU8(outData, f[15 + i].GetUInt8());
    // Resistances 1-7
    for (int i = 0; i < 7; ++i)
        WriteI16(outData, f[20 + i].GetInt16());
    // MinDamage 1-5
    for (int i = 0; i < 5; ++i)
        WriteU16(outData, f[27 + i].GetUInt16());
    // MaxDamage 1-5
    for (int i = 0; i < 5; ++i)
        WriteU16(outData, f[32 + i].GetUInt16());

    return true;
}

// ---- ItemSparse 表序列化（格式参考 HermesProxy WriteItemSparseHotfix） ----
bool HotfixDataMgr::SerializeItemSparseRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT AllowableRace, Description, Display3, Display2, Display1, Display, "
        "DmgVariance, DurationInInventory, QualityModifier, BagFamily, ItemRange, "
        "StatPercentageOfSocket1, StatPercentageOfSocket2, StatPercentageOfSocket3, StatPercentageOfSocket4, StatPercentageOfSocket5, "
        "StatPercentageOfSocket6, StatPercentageOfSocket7, StatPercentageOfSocket8, StatPercentageOfSocket9, StatPercentageOfSocket10, "
        "StatPercentEditor1, StatPercentEditor2, StatPercentEditor3, StatPercentEditor4, StatPercentEditor5, "
        "StatPercentEditor6, StatPercentEditor7, StatPercentEditor8, StatPercentEditor9, StatPercentEditor10, "
        "Stackable, MaxCount, RequiredAbility, SellPrice, BuyPrice, VendorStackCount, "
        "PriceVariance, PriceRandomValue, Flags1, Flags2, Flags3, Flags4, "
        "FactionRelated, MaxDurability, "
        "ItemNameDescriptionID, RequiredTransmogHoliday, RequiredHoliday, LimitCategory, "
        "GemProperties, SocketMatchEnchantmentId, TotemCategoryID, InstanceBound, "
        "ZoneBound1, ZoneBound2, ItemSet, LockID, StartQuestID, PageID, ItemDelay, "
        "MinFactionID, RequiredSkillRank, RequiredSkill, ItemLevel, AllowableClass, "
        "ItemRandomSuffixGroupID, RandomSelect, "
        "MinDamage1, MinDamage2, MinDamage3, MinDamage4, MinDamage5, "
        "MaxDamage1, MaxDamage2, MaxDamage3, MaxDamage4, MaxDamage5, "
        "Resistances1, Resistances2, Resistances3, Resistances4, Resistances5, Resistances6, Resistances7, "
        "ScalingStatDistributionID, "
        "ExpansionID, ArtifactID, SpellWeight, SpellWeightCategory, "
        "SocketType1, SocketType2, SocketType3, "
        "SheatheType, Material, PageMaterialID, LanguageID, Bonding, DamageDamageType, "
        "StatModifierBonusStat1, StatModifierBonusStat2, StatModifierBonusStat3, StatModifierBonusStat4, StatModifierBonusStat5, "
        "StatModifierBonusStat6, StatModifierBonusStat7, StatModifierBonusStat8, StatModifierBonusStat9, StatModifierBonusStat10, "
        "ContainerSlots, RequiredPVPMedal, RequiredPVPRank, InventoryType, OverallQualityID, AmmunitionType, "
        "StatModifierBonusAmount1, StatModifierBonusAmount2, StatModifierBonusAmount3, StatModifierBonusAmount4, StatModifierBonusAmount5, "
        "StatModifierBonusAmount6, StatModifierBonusAmount7, StatModifierBonusAmount8, StatModifierBonusAmount9, StatModifierBonusAmount10, "
        "RequiredLevel "
        "FROM item_sparse WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();
    outData.clear();
    outData.reserve(512);

    int col = 0;

    // int64 AllowableRace（Field 无 GetInt64，用 GetUInt64 转换）
    WriteI64(outData, int64_t(f[col++].GetUInt64()));
    // CString Description（可能为 NULL）
    WriteCString(outData, f[col].GetCppString());  ++col;
    // CString Display3 (name4)
    WriteCString(outData, f[col].GetCppString());  ++col;
    // CString Display2 (name3)
    WriteCString(outData, f[col].GetCppString());  ++col;
    // CString Display1 (name2)
    WriteCString(outData, f[col].GetCppString());  ++col;
    // CString Display (name1)
    WriteCString(outData, f[col].GetCppString());  ++col;
    // float DmgVariance
    WriteFloat(outData, f[col++].GetFloat());
    // uint32 DurationInInventory
    WriteU32(outData, f[col++].GetUInt32());
    // float QualityModifier
    WriteFloat(outData, f[col++].GetFloat());
    // uint32 BagFamily
    WriteU32(outData, f[col++].GetUInt32());
    // float ItemRange (RangeMod)
    WriteFloat(outData, f[col++].GetFloat());
    // 10 × float StatPercentageOfSocket
    for (int i = 0; i < 10; ++i)
        WriteFloat(outData, f[col++].GetFloat());
    // 10 × int32 StatPercentEditor
    for (int i = 0; i < 10; ++i)
        WriteI32(outData, f[col++].GetInt32());
    // int32 Stackable
    WriteI32(outData, f[col++].GetInt32());
    // int32 MaxCount
    WriteI32(outData, f[col++].GetInt32());
    // uint32 RequiredAbility
    WriteU32(outData, f[col++].GetUInt32());
    // uint32 SellPrice
    WriteU32(outData, f[col++].GetUInt32());
    // uint32 BuyPrice
    WriteU32(outData, f[col++].GetUInt32());
    // uint32 VendorStackCount
    WriteU32(outData, f[col++].GetUInt32());
    // float PriceVariance
    WriteFloat(outData, f[col++].GetFloat());
    // float PriceRandomValue
    WriteFloat(outData, f[col++].GetFloat());
    // 4 × uint32 Flags (写出为 int32)
    for (int i = 0; i < 4; ++i)
        WriteI32(outData, f[col++].GetInt32());
    // int32 FactionRelated (OppositeFactionItemId)
    WriteI32(outData, f[col++].GetInt32());
    // uint32 MaxDurability
    WriteU32(outData, f[col++].GetUInt32());
    // 14 × uint16 字段
    WriteU16(outData, f[col++].GetUInt16()); // ItemNameDescriptionID
    WriteU16(outData, f[col++].GetUInt16()); // RequiredTransmogHoliday
    WriteU16(outData, f[col++].GetUInt16()); // RequiredHoliday
    WriteU16(outData, f[col++].GetUInt16()); // LimitCategory
    WriteU16(outData, f[col++].GetUInt16()); // GemProperties
    WriteU16(outData, f[col++].GetUInt16()); // SocketMatchEnchantmentId
    WriteU16(outData, f[col++].GetUInt16()); // TotemCategoryID
    WriteU16(outData, f[col++].GetUInt16()); // InstanceBound
    WriteU16(outData, f[col++].GetUInt16()); // ZoneBound1
    WriteU16(outData, f[col++].GetUInt16()); // ZoneBound2
    WriteU16(outData, f[col++].GetUInt16()); // ItemSet
    WriteU16(outData, f[col++].GetUInt16()); // LockID
    WriteU16(outData, f[col++].GetUInt16()); // StartQuestID
    WriteU16(outData, f[col++].GetUInt16()); // PageID (PageText)
    WriteU16(outData, f[col++].GetUInt16()); // ItemDelay (Delay)
    WriteU16(outData, f[col++].GetUInt16()); // MinFactionID (RequiredReputationId)
    WriteU16(outData, f[col++].GetUInt16()); // RequiredSkillRank
    WriteU16(outData, f[col++].GetUInt16()); // RequiredSkill
    WriteU16(outData, f[col++].GetUInt16()); // ItemLevel
    // int16 AllowableClass
    WriteI16(outData, f[col++].GetInt16());
    // uint16 ItemRandomSuffixGroupID
    WriteU16(outData, f[col++].GetUInt16());
    // uint16 RandomSelect (RandomProperty)
    WriteU16(outData, f[col++].GetUInt16());
    // 5 × uint16 MinDamage
    for (int i = 0; i < 5; ++i)
        WriteU16(outData, f[col++].GetUInt16());
    // 5 × uint16 MaxDamage
    for (int i = 0; i < 5; ++i)
        WriteU16(outData, f[col++].GetUInt16());
    // 7 × int16 Resistances
    for (int i = 0; i < 7; ++i)
        WriteI16(outData, f[col++].GetInt16());
    // uint16 ScalingStatDistributionID
    WriteU16(outData, f[col++].GetUInt16());
    // uint8 字段组
    WriteU8(outData, f[col++].GetUInt8());  // ExpansionID
    WriteU8(outData, f[col++].GetUInt8());  // ArtifactID
    WriteU8(outData, f[col++].GetUInt8());  // SpellWeight
    WriteU8(outData, f[col++].GetUInt8());  // SpellWeightCategory
    WriteU8(outData, f[col++].GetUInt8());  // SocketType1
    WriteU8(outData, f[col++].GetUInt8());  // SocketType2
    WriteU8(outData, f[col++].GetUInt8());  // SocketType3
    WriteU8(outData, f[col++].GetUInt8());  // SheatheType
    WriteU8(outData, f[col++].GetUInt8());  // Material
    WriteU8(outData, f[col++].GetUInt8());  // PageMaterialID (PageMaterial)
    WriteU8(outData, f[col++].GetUInt8());  // LanguageID (PageLanguage)
    WriteU8(outData, f[col++].GetUInt8());  // Bonding
    WriteU8(outData, f[col++].GetUInt8());  // DamageDamageType (DamageType)
    // 10 × int8 StatType (StatModifierBonusStat)
    for (int i = 0; i < 10; ++i)
        WriteI8(outData, int8(f[col++].GetInt32()));
    WriteU8(outData, f[col++].GetUInt8());  // ContainerSlots
    WriteU8(outData, f[col++].GetUInt8());  // RequiredPVPMedal (RequiredReputationRank)
    WriteU8(outData, f[col++].GetUInt8());  // RequiredPVPRank (RequiredCityRank)
    // 注：HermesProxy 序列化中有 RequiredHonorRank 字段，但 hotfixes 库 item_sparse 表无此列，
    // 该字段在 TBC 中不使用，写 0 占位
    WriteU8(outData, 0);                    // RequiredHonorRank（DB 无此列，填0）
    WriteU8(outData, f[col++].GetUInt8());  // InventoryType
    WriteU8(outData, f[col++].GetUInt8());  // OverallQualityID
    WriteU8(outData, f[col++].GetUInt8());  // AmmunitionType (AmmoType)
    // 10 × int8 StatValues（StatModifierBonusAmount，需钳制到 ±127）
    for (int i = 0; i < 10; ++i)
    {
        int32 sv = f[col++].GetInt32();
        if (sv > 127)  sv = 127;
        if (sv < -127) sv = -127;
        WriteI8(outData, int8(sv));
    }
    // int8 RequiredLevel
    WriteI8(outData, int8(f[col++].GetInt32()));

    return true;
}

// ---- BroadcastText 表序列化 ----
bool HotfixDataMgr::SerializeBroadcastTextRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT Text, Text1, ID, LanguageID, ConditionID, EmotesID, Flags, "
        "ChatBubbleDurationMs, VoiceOverPriorityID, "
        "SoundKitID1, SoundKitID2, "
        "EmoteID1, EmoteID2, EmoteID3, "
        "EmoteDelay1, EmoteDelay2, EmoteDelay3 "
        "FROM broadcast_text WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();
    outData.clear();
    outData.reserve(256);

    // CString MaleText (Text)
    WriteCString(outData, f[0].GetCppString());
    // CString FemaleText (Text1)
    WriteCString(outData, f[1].GetCppString());
    // uint32 Entry (ID)
    WriteU32(outData, f[2].GetUInt32());
    // uint32 Language (LanguageID)
    WriteU32(outData, f[3].GetUInt32());
    // uint32 ConditionId (ConditionID)
    WriteU32(outData, f[4].GetUInt32());
    // uint16 EmotesId (EmotesID)
    WriteU16(outData, f[5].GetUInt16());
    // uint8 Flags
    WriteU8(outData, f[6].GetUInt8());
    // uint32 ChatBubbleDurationMs
    WriteU32(outData, f[7].GetUInt32());
    // uint32 VoiceOverPriorityID（2.5.3 版本包含此字段）
    WriteU32(outData, f[8].GetUInt32());
    // 2 × uint32 SoundEntriesID (SoundKitID1, SoundKitID2)
    WriteU32(outData, f[9].GetUInt32());
    WriteU32(outData, f[10].GetUInt32());
    // 3 × uint16 Emotes (EmoteID1-3)
    WriteU16(outData, f[11].GetUInt16());
    WriteU16(outData, f[12].GetUInt16());
    WriteU16(outData, f[13].GetUInt16());
    // 3 × uint16 EmoteDelays (EmoteDelay1-3)
    WriteU16(outData, f[14].GetUInt16());
    WriteU16(outData, f[15].GetUInt16());
    WriteU16(outData, f[16].GetUInt16());

    return true;
}

// ---- ItemEffect 表序列化 ----
bool HotfixDataMgr::SerializeItemEffectRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT LegacySlotIndex, TriggerType, Charges, CoolDownMSec, "
        "CategoryCoolDownMSec, SpellCategoryID, SpellID, ChrSpecializationID, ParentItemID "
        "FROM item_effect WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();
    outData.clear();
    outData.reserve(32);

    WriteU8(outData,  f[0].GetUInt8());   // LegacySlotIndex
    WriteI8(outData,  int8(f[1].GetInt32()));    // TriggerType
    WriteI16(outData, f[2].GetInt16());   // Charges
    WriteI32(outData, f[3].GetInt32());   // CoolDownMSec
    WriteI32(outData, f[4].GetInt32());   // CategoryCoolDownMSec
    WriteU16(outData, f[5].GetUInt16());  // SpellCategoryID
    WriteI32(outData, f[6].GetInt32());   // SpellID
    WriteU16(outData, f[7].GetUInt16());  // ChrSpecializationID
    WriteI32(outData, f[8].GetInt32());   // ParentItemID

    return true;
}

// ---- ItemAppearance 表序列化 ----
bool HotfixDataMgr::SerializeItemAppearanceRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT DisplayType, ItemDisplayInfoID, DefaultIconFileDataID, UiOrder "
        "FROM item_appearance WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();
    outData.clear();
    outData.reserve(16);

    WriteU8(outData,  f[0].GetUInt8());   // DisplayType
    WriteI32(outData, f[1].GetInt32());   // ItemDisplayInfoID
    WriteI32(outData, f[2].GetInt32());   // DefaultIconFileDataID
    WriteI32(outData, f[3].GetInt32());   // UiOrder

    return true;
}

// ---- ItemModifiedAppearance 表序列化 ----
bool HotfixDataMgr::SerializeItemModifiedAppearanceRecord(uint32 recordId, std::vector<uint8>& outData)
{
    auto result = std::unique_ptr<QueryResult>(HotfixDatabase.PQuery(
        "SELECT ID, ItemID, ItemAppearanceModifierID, ItemAppearanceID, OrderIndex, TransmogSourceTypeEnum "
        "FROM item_modified_appearance WHERE ID=%u", recordId));

    if (!result)
        return false;

    Field* f = result->Fetch();
    outData.clear();
    outData.reserve(24);

    WriteI32(outData, f[0].GetInt32());   // ID
    WriteI32(outData, f[1].GetInt32());   // ItemID
    WriteI32(outData, f[2].GetInt32());   // ItemAppearanceModifierID
    WriteI32(outData, f[3].GetInt32());   // ItemAppearanceID
    WriteI32(outData, f[4].GetInt32());   // OrderIndex
    WriteI32(outData, f[5].GetInt32());   // TransmogSourceTypeEnum

    return true;
}
//End By leewheel
