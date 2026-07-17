/***
 *
 *	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
 *
 *	This product contains software technology licensed from Id
 *	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
 *	All Rights Reserved.
 *
 *   Use, distribution, and modification of this source code and/or resulting
 *   object code is restricted to non-commercial enhancements to products from
 *   Valve LLC.  All other use, distribution, or modification is prohibited
 *   without written permission from Valve LLC.
 *
 ****/
/*

===== tf_shareddefs.cpp ========================================================

  functions dealing with conditions and other player stuff that should be synced on the server

*/

#include <limits>
#include <algorithm>

#include "extdll.h"
#include "util.h"

#include "tf_shareddefs.h"


bool ConditionExpiresFast(ETFCond eCond)
{
	return eCond == TF_COND_BURNING || eCond == TF_COND_URINE || eCond == TF_COND_BLEEDING || eCond == TF_COND_MAD_MILK || eCond == TF_COND_GAS;
}

static const char* g_aConditionNames[] =
	{
		"TF_COND_AIMING",							// = 0 - Sniper aiming, Heavy minigun.
		"TF_COND_ZOOMED",							// = 1
		"TF_COND_DISGUISING",						// = 2
		"TF_COND_DISGUISED",						// = 3
		"TF_COND_STEALTHED",						// = 4 - Spy specific
		"TF_COND_INVULNERABLE",						// = 5
		"TF_COND_TELEPORTED",						// = 6
		"TF_COND_TAUNTING",							// = 7
		"TF_COND_INVULNERABLE_WEARINGOFF",			// = 8
		"TF_COND_STEALTHED_BLINK",					// = 9
		"TF_COND_SELECTED_TO_TELEPORT",				// = 10
		"TF_COND_CRITBOOSTED",						// = 11 - DO NOT RE-USE THIS -- THIS IS FOR KRITZKRIEG AND REVENGE CRITS ONLY
		"TF_COND_TMPDAMAGEBONUS",					// = 12
		"TF_COND_FEIGN_DEATH",						// = 13
		"TF_COND_PHASE",							// = 14
		"TF_COND_STUNNED",							// = 15 - Any type of stun. Check iStunFlags for more info.
		"TF_COND_OFFENSEBUFF",						// = 16
		"TF_COND_SHIELD_CHARGE",					// = 17
		"TF_COND_DEMO_BUFF",						// = 18
		"TF_COND_ENERGY_BUFF",						// = 19
		"TF_COND_RADIUSHEAL",						// = 20
		"TF_COND_HEALTH_BUFF",						// = 21
		"TF_COND_BURNING",							// = 22
		"TF_COND_HEALTH_OVERHEALED",				// = 23
		"TF_COND_URINE",							// = 24
		"TF_COND_BLEEDING",							// = 25
		"TF_COND_DEFENSEBUFF",						// = 26 - 35% defense! No crit damage.
		"TF_COND_MAD_MILK",							// = 27
		"TF_COND_MEGAHEAL",							// = 28
		"TF_COND_REGENONDAMAGEBUFF",				// = 29
		"TF_COND_MARKEDFORDEATH",					// = 30
		"TF_COND_NOHEALINGDAMAGEBUFF",				// = 31
		"TF_COND_SPEED_BOOST",						// = 32
		"TF_COND_CRITBOOSTED_PUMPKIN",				// = 33 - Brandon hates bits
		"TF_COND_CRITBOOSTED_USER_BUFF",			// = 34
		"TF_COND_CRITBOOSTED_DEMO_CHARGE",			// = 35
		"TF_COND_SODAPOPPER_HYPE",					// = 36
		"TF_COND_CRITBOOSTED_FIRST_BLOOD",			// = 37 - arena mode first blood
		"TF_COND_CRITBOOSTED_BONUS_TIME",			// = 38
		"TF_COND_CRITBOOSTED_CTF_CAPTURE",			// = 39
		"TF_COND_CRITBOOSTED_ON_KILL",				// = 40 - KGB, etc.
		"TF_COND_CANNOT_SWITCH_FROM_MELEE",			// = 41
		"TF_COND_DEFENSEBUFF_NO_CRIT_BLOCK",		// = 42 - 35% defense! Still damaged by crits.
		"TF_COND_REPROGRAMMED",						// = 43 - Bots only
		"TF_COND_CRITBOOSTED_RAGE_BUFF",			// = 44
		"TF_COND_DEFENSEBUFF_HIGH",					// = 45 - 75% defense! Still damaged by crits.
		"TF_COND_SNIPERCHARGE_RAGE_BUFF",			// = 46 - Sniper Rage - Charge time speed up
		"TF_COND_DISGUISE_WEARINGOFF",				// = 47 - Applied for half-second post-disguise
		"TF_COND_MARKEDFORDEATH_SILENT",			// = 48 - Sans sound
		"TF_COND_DISGUISED_AS_DISPENSER",			// = 49
		"TF_COND_SAPPED",							// = 50 - Bots only
		"TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED", // = 51
		"TF_COND_INVULNERABLE_USER_BUFF",			// = 52
		"TF_COND_HALLOWEEN_BOMB_HEAD",				// = 53
		"TF_COND_HALLOWEEN_THRILLER",				// = 54
		"TF_COND_RADIUSHEAL_ON_DAMAGE",				// = 55
		"TF_COND_CRITBOOSTED_CARD_EFFECT",			// = 56
		"TF_COND_INVULNERABLE_CARD_EFFECT",			// = 57
		"TF_COND_MEDIGUN_UBER_BULLET_RESIST",		// = 58
		"TF_COND_MEDIGUN_UBER_BLAST_RESIST",		// = 59
		"TF_COND_MEDIGUN_UBER_FIRE_RESIST",			// = 60
		"TF_COND_MEDIGUN_SMALL_BULLET_RESIST",		// = 61
		"TF_COND_MEDIGUN_SMALL_BLAST_RESIST",		// = 62
		"TF_COND_MEDIGUN_SMALL_FIRE_RESIST",		// = 63
		"TF_COND_STEALTHED_USER_BUFF",				// = 64 - Any class can have this
		"TF_COND_MEDIGUN_DEBUFF",					// = 65
		"TF_COND_STEALTHED_USER_BUFF_FADING",		// = 66
		"TF_COND_BULLET_IMMUNE",					// = 67
		"TF_COND_BLAST_IMMUNE",						// = 68
		"TF_COND_FIRE_IMMUNE",						// = 69
		"TF_COND_PREVENT_DEATH",					// = 70
		"TF_COND_MVM_BOT_STUN_RADIOWAVE",			// = 71 - Bots only
		"TF_COND_HALLOWEEN_SPEED_BOOST",			// = 72
		"TF_COND_HALLOWEEN_QUICK_HEAL",				// = 73
		"TF_COND_HALLOWEEN_GIANT",					// = 74
		"TF_COND_HALLOWEEN_TINY",					// = 75
		"TF_COND_HALLOWEEN_IN_HELL",				// = 76
		"TF_COND_HALLOWEEN_GHOST_MODE",				// = 77
		"TF_COND_MINICRITBOOSTED_ON_KILL",			// = 78
		"TF_COND_OBSCURED_SMOKE",					// = 79
		"TF_COND_PARACHUTE_ACTIVE",					// = 80
		"TF_COND_BLASTJUMPING",						// = 81
		"TF_COND_HALLOWEEN_KART",					// = 82
		"TF_COND_HALLOWEEN_KART_DASH",				// = 83
		"TF_COND_BALLOON_HEAD",						// = 84 - larger head, lower-gravity-feeling jumps
		"TF_COND_MELEE_ONLY",						// = 85 - melee only
		"TF_COND_SWIMMING_CURSE",					// = 86 - player movement become swimming movement
		"TF_COND_FREEZE_INPUT",						// = 87 - freezes player input
		"TF_COND_HALLOWEEN_KART_CAGE",				// = 88 - attach cage model to player while in kart
		"TF_COND_DONOTUSE_0",						// = 89
		"TF_COND_RUNE_STRENGTH",					// = 90
		"TF_COND_RUNE_HASTE",						// = 91
		"TF_COND_RUNE_REGEN",						// = 92
		"TF_COND_RUNE_RESIST",						// = 93
		"TF_COND_RUNE_VAMPIRE",						// = 94
		"TF_COND_RUNE_REFLECT",						// = 95
		"TF_COND_RUNE_PRECISION",					// = 96
		"TF_COND_RUNE_AGILITY",						// = 97
		"TF_COND_GRAPPLINGHOOK",					// = 98
		"TF_COND_GRAPPLINGHOOK_SAFEFALL",			// = 99
		"TF_COND_GRAPPLINGHOOK_LATCHED",			// = 100
		"TF_COND_GRAPPLINGHOOK_BLEEDING",			// = 101
		"TF_COND_AFTERBURN_IMMUNE",					// = 102
		"TF_COND_RUNE_KNOCKOUT",					// = 103
		"TF_COND_RUNE_IMBALANCE",					// = 104
		"TF_COND_CRITBOOSTED_RUNE_TEMP",			// = 105
		"TF_COND_PASSTIME_INTERCEPTION",			// = 106
		"TF_COND_SWIMMING_NO_EFFECTS",				// = 107 - =107_DNOC_FT
		"TF_COND_PURGATORY",						// = 108
		"TF_COND_RUNE_KING",						// = 109
		"TF_COND_RUNE_PLAGUE",						// = 110
		"TF_COND_RUNE_SUPERNOVA",					// = 111
		"TF_COND_PLAGUE",							// = 112
		"TF_COND_KING_BUFFED",						// = 113
		"TF_COND_TEAM_GLOWS",						// = 114 - used to show team glows to living players
		"TF_COND_KNOCKED_INTO_AIR",					// = 115
		"TF_COND_COMPETITIVE_WINNER",				// = 116
		"TF_COND_COMPETITIVE_LOSER",				// = 117
		"TF_COND_HEALING_DEBUFF",					// = 118
		"TF_COND_PASSTIME_PENALTY_DEBUFF",			// = 119
		"TF_COND_GRAPPLED_TO_PLAYER",				// = 120
		"TF_COND_GRAPPLED_BY_PLAYER",				// = 121
		"TF_COND_PARACHUTE_DEPLOYED",				// = 122
		"TF_COND_GAS",								// = 123
		"TF_COND_BURNING_PYRO",						// = 124
		"TF_COND_ROCKETPACK",						// = 125
		"TF_COND_LOST_FOOTING",						// = 126
		"TF_COND_AIR_CURRENT",						// = 127
		"TF_COND_HALLOWEEN_HELL_HEAL",				// = 128
		"TF_COND_POWERUPMODE_DOMINANT",				// = 129
		"TF_COND_IMMUNE_TO_PUSHBACK",				// = 130

		//
		// ADD NEW ITEMS HERE TO AVOID BREAKING DEMOS
		//

		// ******** Keep this block last! ********
		// Keep experimental conditions below and graduate out of it before shipping
};

const char* GetTFConditionName(ETFCond eCond)
{
	if ((eCond >= ARRAYSIZE(g_aConditionNames)) || (eCond < 0))
		return NULL;

	return g_aConditionNames[eCond];
}


ETFCond GetTFConditionFromName(const char* pszCondName)
{
	for (int i = 0; i < TF_COND_LAST; i++)
	{
		ETFCond eCond = (ETFCond)i;
		if (!strcmp(GetTFConditionName(eCond), pszCondName))
			return eCond;
	}

	ALERT(at_console, "Invalid Condition Name!\n");
	return TF_COND_INVALID;
}

ETFCond condition_to_attribute_translation[] =
{
	TF_COND_BURNING,				 // 1 (1<<0)
	TF_COND_AIMING,					 // 2 (1<<1)
	TF_COND_ZOOMED,					 // 4 (1<<2)
	TF_COND_DISGUISING,				 // 8 (...)
	TF_COND_DISGUISED,				 // 16
	TF_COND_STEALTHED,				 // 32
	TF_COND_INVULNERABLE,			 // 64
	TF_COND_TELEPORTED,				 // 128
	TF_COND_TAUNTING,				 // 256
	TF_COND_INVULNERABLE_WEARINGOFF, // 512
	TF_COND_STEALTHED_BLINK,		 // 1024
	TF_COND_SELECTED_TO_TELEPORT,	 // 2048
	TF_COND_CRITBOOSTED,			 // 4096
	TF_COND_TMPDAMAGEBONUS,			 // 8192
	TF_COND_FEIGN_DEATH,			 // 16384
	TF_COND_PHASE,					 // 32768
	TF_COND_STUNNED,				 // 65536
	TF_COND_HEALTH_BUFF,			 // 131072
	TF_COND_HEALTH_OVERHEALED,		 // 262144
	TF_COND_URINE,					 // 524288
	TF_COND_ENERGY_BUFF,			 // 1048576
	TF_COND_LAST // sentinel value checked against when iterating
};

ETFCond g_aDebuffConditions[] =
{
	TF_COND_BURNING,
	TF_COND_URINE,
	TF_COND_BLEEDING,
	TF_COND_MAD_MILK,
	TF_COND_GAS,
	TF_COND_LAST
};
