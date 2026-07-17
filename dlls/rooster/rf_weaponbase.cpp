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
***/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "rf_viewmodel.h"
#include "rf_player.h"

// this file hosts all the weapon base classes
// the overall tf2 weapon, the guns, and the melee classes
// this file does NOT host the actual weapons themselves. those go in their respective files

// ------------------------------------------------ SHARED BASE ----------------------------------------------------- //

enum viewmodel_e
{
	PRIMARY_DRAW,
	PRIMARY_IDLE,
	PRIMARY_FIRE,
	PRIMARY_RELOAD,
	PRIMARY_RELOAD_START,
	PRIMARY_RELOAD_END,

	SECONDARY_DRAW,
	SECONDARY_IDLE,
	SECONDARY_FIRE,
	SECONDARY_RELOAD,
	SECONDARY_RELOAD_START,
	SECONDARY_RELOAD_END,

	MELEE_DRAW,
	MELEE_IDLE,
	MELEE_SWING_A,
	MELEE_SWING_B,
	MELEE_SWING_CRIT,

	ITEM1_DRAW,
	ITEM1_IDLE,

	ITEM2_DRAW,
	ITEM2_IDLE,

	LAST_ENUM_ANIM
};
// todo: make a function that first looks for the sequences with a string before using the index numbers directly
// ex: look for "primary_shoot" or "primary_fire" first. if those sequences exist, use the index they are in. if they do not, then default to using the index enum number
// would allow flexibility with creating viewemodels

enum ViewAnimState
{
	ATTACK,
	ATTACK_ALT,	// example of why i make FindSequenceViewmodel use a case rather than just a hardcoded things, as some unlocks use seperate viewmodel anims
	FIRE,
	RELOAD,
	RELOAD_START,
	RELOAD_END,
	IDLE,
	DRAW
};

inline int FindSequenceViewmodel(int slot, int state)
{
	// for tf2 viewmodels
	// specifically for the c_arms system i will make in the future ( ported from rf2 og branch )
	// the states have their own cases due to future animations being required

	if (state < ATTACK || state > DRAW)
		return 0;

	switch (state)
	{
		case ATTACK:
		{
		//	int idx = LookupSequence( "primary_fire" );
		//	if( !(idx >= 0) ) idx = LookupSequence( "primary_shoot" );
		//	return ( idx >= 0 ? idx : PRIMARY_FIRE );		// fall back to enum
		}
	}

	return 0;
}

void CRFBasePlayerWeapon::Holster(int skiplocal)
{
	// keep this here
	float holster_time = 0.5 * m_flHolsterPercentage;
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + holster_time;
	m_flNextPrimaryAttack = GetNextAttackDelay(holster_time);
	CBasePlayerWeapon::Holster();
}

BOOL CRFBasePlayerWeapon::Deploy()
{
	return true;
	// this gets replaced when new weapons are made
	// fallback just in case
	
	// todo: add error print in console, this function should never be used and if it does then i should know abt it
}

BOOL CRFBasePlayerWeapon::DefaultDeploy(const char* szWeaponModel, int iAnim, const char* szAnimExt)
{
	if (!CanDeploy())
		return false;
	
	CRFPlayer *player = ToRFPlayer(m_pPlayer);

	if (!player)
		return false; // this should never happen

	const char* pszDeployViewModel = ViewModel_ArmsModelForClass(player->m_iClass);

	if (pszDeployViewModel == nullptr)
	{
		return false; // this should also never happen
		// we don't need to worry abt errors with the c_models themselves due to the game precache noticing
	}

	m_pPlayer->TabulateAmmo();
	m_pPlayer->pev->viewmodel = MAKE_STRING(pszDeployViewModel);
	m_pPlayer->pev->weaponmodel = MAKE_STRING(szWeaponModel);
	strcpy(m_pPlayer->m_szAnimExtention, szAnimExt);

	SendWeaponAnim(iAnim, UseDecrement() != false);

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5;
	m_flLastFireTime = 0.0;

	return true;
}
/*

DefaultDeploy(const char* szViewModel, const char* szWeaponModel, int iAnim, const char* szAnimExt, int skiplocal, int body)
{
	if (!CanDeploy())
		return FALSE;


	m_pPlayer->TabulateAmmo();
	m_pPlayer->pev->viewmodel = MAKE_STRING(szViewModel);
	m_pPlayer->pev->weaponmodel = MAKE_STRING(szWeaponModel);
	strcpy(m_pPlayer->m_szAnimExtention, szAnimExt);
	SendWeaponAnim(iAnim, skiplocal, body);

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.0f;
	m_flLastFireTime = 0.0f;

	return TRUE;
}

*/



// ------------------------------------------------ RANGED WEAPONS -------------------------------------------------- //

void CRFBasePlayerGun::Precache(void)
{
	m_usRF2Ranged = PRECACHE_EVENT(1, "events/roosterfortress2.sc");
	// this whole fucking system is stupid and shitty
	// whichever subhuman at valve thought it was a good idea to tie this shit to files and do this shit man
	// ""scripting system"" if you didnt add it then FUCKING REMOVE IT VALVE. UGH
}

BOOL CRFBasePlayerGun::GetItemInfo(ItemInfo* p)
{
	// REPLACE THIS FULLY WHEN YOU MAKE A NEW WEAPON.
	// THIS IS A PLACEHOLDER

	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;
	p->iPosition = 0;
	p->iWeight = 10;
	p->iFlags = ITEM_FLAG_SELECTONEMPTY;
	return true;
}

void CRFBasePlayerGun::ShootBullets(float spread, int multishot = 1)
{
	// stolen from the glock #cagefuel

	if (m_iClip <= 0)
	{
		if (m_fFireOnEmpty)
		{
			PlayEmptySound();
			m_flNextPrimaryAttack = GetNextAttackDelay(0.25);
		}

		return;
	}
	
	m_iClip--;

	m_pPlayer->pev->effects = (int)(m_pPlayer->pev->effects) | EF_MUZZLEFLASH;

	int flags;
#if CLIENT_WEAPONS
	flags = FEV_NOTHOST;
#else
	flags = 0;
#endif
	// player "shoot" animation
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);

	m_pPlayer->m_iWeaponVolume = NORMAL_GUN_VOLUME;
	m_pPlayer->m_iWeaponFlash = NORMAL_GUN_FLASH;

	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAiming;
	vecAiming = gpGlobals->v_forward;

	Vector vecDir;
	vecDir = m_pPlayer->FireBulletsPlayer(multishot, vecSrc, vecAiming, Vector(spread, spread, spread), 8192, BULLET_PLAYER_TF2, 0, m_flDamage, m_pPlayer->pev, m_pPlayer->random_seed);

	PLAYBACK_EVENT_FULL(flags, m_pPlayer->edict(), m_usRF2Ranged, 0.0, g_vecZero, g_vecZero, vecDir.x, vecDir.y, 0, 0, (m_iClip == 0) ? 1 : 0, 0);
	// PLAYBACK_EVENT_FULL ( int flags, const edict_t *pInvoker, unsigned short eventindex, float delay, const float *origin, const float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1, int bparam2 );

	m_flNextPrimaryAttack = m_flNextSecondaryAttack = GetNextAttackDelay(m_flAttackDelay);

	if (!m_iClip && m_pPlayer->m_rgAmmo[m_iPrimaryAmmoType] <= 0)
		// HEV suit - indicate out of ammo condition
		m_pPlayer->SetSuitUpdate("!HEV_AMO0", FALSE, 0);

	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + UTIL_SharedRandomFloat(m_pPlayer->random_seed, 10, 15);
}
 
// ------------------------------------------------ MELEE WEAPONS --------------------------------------------------- //

inline void FindHullIntersection(const Vector& vecSrc, TraceResult& tr, const Vector& mins, const Vector& maxs, edict_t* pEntity)
{
	int i, j, k;
	float distance;
	const Vector* minmaxs[2] = { &mins, &maxs };
	TraceResult tmpTrace;
	Vector vecHullEnd = tr.vecEndPos;
	Vector vecEnd;

	distance = 1e6f;

	vecHullEnd = vecSrc + ((vecHullEnd - vecSrc) * 2);
	UTIL_TraceLine(vecSrc, vecHullEnd, dont_ignore_monsters, pEntity, &tmpTrace);
	if (tmpTrace.flFraction < 1.0)
	{
		tr = tmpTrace;
		return;
	}

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < 2; j++)
		{
			for (k = 0; k < 2; k++)
			{
				vecEnd.x = vecHullEnd.x + minmaxs[i]->x;
				vecEnd.y = vecHullEnd.y + minmaxs[j]->y;
				vecEnd.z = vecHullEnd.z + minmaxs[k]->z;

				UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pEntity, &tmpTrace);
				if (tmpTrace.flFraction < 1.0)
				{
					float thisDistance = (tmpTrace.vecEndPos - vecSrc).Length();
					if (thisDistance < distance)
					{
						tr = tmpTrace;
						distance = thisDistance;
					}
				}
			}
		}
	}
}

void CRFBasePlayerMelee::Precache()
{
	// make sure to call this when making new melee!
	// like CRFBasePlayerMelee::Precache() after everything else is precached
	// this is important!!!

	PRECACHE_SOUND("weapons/cbar_hit1.wav");
	PRECACHE_SOUND("weapons/cbar_hit2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod1.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod3.wav");
	PRECACHE_SOUND("weapons/cbar_miss1.wav");

	m_usMeleeWeapon = PRECACHE_EVENT(1, "events/melee.sc");
}

BOOL CRFBasePlayerMelee::GetItemInfo(ItemInfo* p)
{
	// same as Precache()
	// make sure to change the weapon id on the new melee before calling this though
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 2;
	p->iPosition = 2;
	p->iWeight = 10;
	p->iFlags = ITEM_FLAG_SELECTONEMPTY;
	return true;
}

void CRFBasePlayerMelee::PrimaryAttack()
{
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usMeleeWeapon,
		0.0, g_vecZero, g_vecZero, 0, 0, 0, 0.0, 0, 0.0);

	// Third-person "shoot" pose so other players see the swing.
	m_pPlayer->SetAnimation(PLAYER_ATTACK1);

	SetThink(&CRFBasePlayerMelee::Swing);
	pev->nextthink = gpGlobals->time + 0.2;
	m_flNextPrimaryAttack = GetNextAttackDelay(m_flSwingDelay);
}

void CRFBasePlayerMelee::Swing()
{
	bool fDidHit = false;

	TraceResult tr;

	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecEnd = vecSrc + gpGlobals->v_forward * 32;

	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, ENT(m_pPlayer->pev), &tr);

#ifndef CLIENT_DLL
	if (tr.flFraction >= 1.0)
	{
		UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, head_hull, ENT(m_pPlayer->pev), &tr);
		if (tr.flFraction < 1.0)
		{
			// Calculate the point of intersection of the line (or hull) and the object we hit
			// This is and approximation of the "best" intersection
			CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);
			if (!pHit || pHit->IsBSPModel())
				FindHullIntersection(vecSrc, tr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX, m_pPlayer->edict());
			vecEnd = tr.vecEndPos; // This is the point on the actual surface (the hull could have hit space)
		}
	}

	if (tr.flFraction < 1.0)
	{
		// hit
		fDidHit = true;
		CBaseEntity* pEntity = CBaseEntity::Instance(tr.pHit);

		ClearMultiDamage();

		pEntity->TraceAttack(m_pPlayer->pev, m_flMeleeDamage, gpGlobals->v_forward, &tr, DMG_CLUB);
		ApplyMultiDamage(m_pPlayer->pev, m_pPlayer->pev);

		// play thwack, smack, or dong sound
		float flVol = 1.0;
		bool fHitWorld = true;

		if (pEntity)
		{
			if (pEntity->Classify() != CLASS_NONE && pEntity->Classify() != CLASS_MACHINE)
			{
				// play thwack or smack sound
				switch (RANDOM_LONG(0, 2))
				{
				case 0:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_BODY, "weapons/cbar_hitbod1.wav", 1, ATTN_NORM);
					break;
				case 1:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_BODY, "weapons/cbar_hitbod2.wav", 1, ATTN_NORM);
					break;
				case 2:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_BODY, "weapons/cbar_hitbod3.wav", 1, ATTN_NORM);
					break;
				}
				m_pPlayer->m_iWeaponVolume = MELEE_BODYHIT_VOLUME;
				if (!pEntity->IsAlive())
					return;
				else
					flVol = 0.1;

				fHitWorld = false;
			}
		}

		// play texture hit sound
		// UNDONE: Calculate the correct point of intersection when we hit with the hull instead of the line

		if (fHitWorld)
		{
			float fvolbar = TEXTURETYPE_PlaySound(&tr, vecSrc, vecSrc + (vecEnd - vecSrc) * 2, BULLET_PLAYER_CROWBAR);

			if (g_pGameRules->IsMultiplayer())
			{
				// override the volume here, cause we don't play texture sounds in multiplayer,
				// and fvolbar is going to be 0 from the above call.

				fvolbar = 1;
			}

			// also play crowbar strike
			switch (RANDOM_LONG(0, 1))
			{
			case 0:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit1.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			case 1:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit2.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			}

			// delay the decal a bit
			m_trHit = tr;
			DecalGunshot(&m_trHit, BULLET_PLAYER_CROWBAR);
		}

		m_pPlayer->m_iWeaponVolume = flVol * MELEE_WALLHIT_VOLUME;
	}
#endif
	return;
}
//CRFBasePlayerMelee::