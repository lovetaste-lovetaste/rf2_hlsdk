// for the tf2 stuff
// this is seperate from the normal CBasePlayer so that old shit won't break as much

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "rf_player.h"
#include "tf_shareddefs.h"
#include "gamerules.h"
#include "weapons.h"
#include "pm_shared.h"

LINK_ENTITY_TO_CLASS(player, CRFPlayer)   // moved out of player.cpp

TYPEDESCRIPTION	CRFPlayer::m_rfSaveData[] =
{
	// Only the NEW CRFPlayer members belong here. Every base-class field is already persisted by CBasePlayer::Save (m_playerSaveData); listing them again here would double-save and re-restore the CLASSPTR/EHANDLE fields twice.
	DEFINE_FIELD(CRFPlayer, m_iClass, FIELD_INTEGER),
	DEFINE_FIELD(CRFPlayer, m_iTeam, FIELD_INTEGER),
	DEFINE_FIELD(CRFPlayer, m_nCond, FIELD_INTEGER), // todo: use the bitfield brah!!! the arrays mignt not be needed + iirc this is how tf2 does it
	DEFINE_FIELD(CRFPlayer, m_iAirJump, FIELD_INTEGER),
	// m_iTFConds / m_iTFConds_Timer are transient combat state -- intentionally not saved.
};

CRFPlayer* ToRFPlayer(CBaseEntity* pEntity)
{
	if (!pEntity || !pEntity->IsPlayer()) return NULL;
	return static_cast<CRFPlayer*>(pEntity);   // static_cast, no dynamic_cast
}

void CRFPlayer::Spawn(void)
{
    CBasePlayer::Spawn();     // always chain to base first
    // RF spawn logic

	RemoveAllConditions();

//	SET_MODEL(ENT(pev), "models/player.mdl"); // todo: use class models
}

// These override CBasePlayer virtuals. They must be defined (even as pass-throughs) or the vtable produces unresolved-external link errors. Fill in RF logic as needed.
void CRFPlayer::Precache(void)
{
	CBasePlayer::Precache();
	// all the playermodels are precached in ClientPrecache() in client.cpp, so just leave this empty for now
}

void CRFPlayer::PreThink(void)
{
	CBasePlayer::PreThink();
	CheckTeamFortressConditions();

	// TODO: run CheckTeamFortressConditions() here once conditions are wired up
}

void CRFPlayer::PostThink(void)
{
	CBasePlayer::PostThink();
}

void CRFPlayer::Killed(entvars_t* pevAttacker, int iGib)
{
	// TODO: RF death handling (clear conditions, RF death sound) before/after base
	CBasePlayer::Killed(pevAttacker, iGib);
}

int CRFPlayer::TakeHealth(float flHealth, int bitsDamageType) // healing
{
	return CBasePlayer::TakeHealth(flHealth, bitsDamageType);
}

int CRFPlayer::TakeDamage(entvars_t* pevInflictor, entvars_t* pevAttacker, float flDamage, int bitsDamageType)
{
	return CBasePlayer::TakeDamage(pevInflictor, pevAttacker, flDamage, bitsDamageType);
}

int CRFPlayer::Save(CSave & save)
{
    if (!CBasePlayer::Save(save)) return 0;
    return save.WriteFields("RFPLAYER", this, m_rfSaveData, ARRAYSIZE(m_rfSaveData));
}
int CRFPlayer::Restore(CRestore& restore)
{
    if (!CBasePlayer::Restore(restore)) return 0;
    return restore.ReadFields("RFPLAYER", this, m_rfSaveData, ARRAYSIZE(m_rfSaveData));
}

int CRFPlayer::GetTeamNumber()
{
	return m_iTeam;
}

// Sets the team number
void CRFPlayer::SetTeamNumber(int newTeam)
{
	m_iTeam = newTeam;
}

int CRFPlayer::GetClassMaxHealth(int iClass)
{
	switch (iClass)
	{
	/*case CLASS_SCOUT:
	case CLASS_SNIPER:
	case CLASS_SPY:
	case CLASS_ENGINEER: return 125;*/ // not needed as the return already does this correctly
	case CLASS_HEAVY: return 300;
	case CLASS_SOLDIER: return 200;
	case CLASS_PYRO:
	case CLASS_DEMOMAN: return 175;
	case CLASS_MEDIC: return 150;
	}
	return 125;
}

float GetClassMaxSpeed(int iClass)
{
	float speed = 300.0; // base speed
	// for some reason the values were all fucked up in the original chicken fortress 3
	// not too different though, so i just added base tf2 values
	// i also left the variable above so unlocks will be easier
	switch (iClass)
	{
	case CLASS_SCOUT:    speed *= 1.33; break;
	case CLASS_HEAVY:    speed *= 0.77; break;
	case CLASS_SOLDIER:  speed *= 0.8;  break;
	case CLASS_PYRO:     speed *= 1.0;  break;
	case CLASS_SNIPER:   speed *= 1.0;  break;
	case CLASS_MEDIC:    speed *= 1.07; break;
	case CLASS_ENGINEER: speed *= 1.0;  break;
	case CLASS_DEMOMAN:  speed *= 0.93; break;
	case CLASS_SPY:      speed *= 1.07; break;
	}
	return speed;
}

void CRFPlayer::ResetMaxSpeed()
{
	float speed = GetClassMaxSpeed(m_iClass);

	if (IsObserver() && !IsAlive())
		speed = 1200.0;

	g_engfuncs.pfnSetClientMaxspeed(ENT(pev), speed);
	pev->maxspeed = speed; // just in case
}

void CRFPlayer::SetResetMaxSpeed(float percent)
{
	float speed = GetClassMaxSpeed(m_iClass);

	if (IsObserver() && !IsAlive())
		speed = 1200.0;

	speed *= percent;

	g_engfuncs.pfnSetClientMaxspeed(ENT(pev), speed);
	pev->maxspeed = speed; // just in case
}

void CRFPlayer::SetPlayerModel()
{
	//if (this->IsFakeClient())
	//{
		//ALERT(at_console, "bot spawned");
		//SET_MODEL(ENT(pev), "models/player.mdl");
		//return;
	//}

	char* model;

	switch (m_iClass)
	{
	default: return;
	case CLASS_SCOUT: model = "scout"; break;
	case CLASS_HEAVY: model = "hvyweapon"; break;
	case CLASS_SOLDIER: model = "soldier"; break;
	case CLASS_PYRO: model = "pyro"; break;
	case CLASS_SNIPER: model = "sniper"; break;
	case CLASS_MEDIC: model = "medic"; break;
	case CLASS_ENGINEER: model = "engineer"; break;
	case CLASS_DEMOMAN: model = "demo"; break;
	case CLASS_SPY: model = "spy"; break;
	}

	char* color;

	if (m_iTeam == TEAM_BLUE)
	{
		color = COLOR_BLU;
	}
	else if (m_iTeam == TEAM_RED)
	{
		color = COLOR_RED;
	}
	else
		color = "1";

	char* infobuffer = g_engfuncs.pfnGetInfoKeyBuffer(edict());
	char* curmodel = g_engfuncs.pfnInfoKeyValue(infobuffer, "model");
	char* curcolor = g_engfuncs.pfnInfoKeyValue(infobuffer, "topcolor");
	if (strcmp(curmodel, model))
		g_engfuncs.pfnSetClientKeyValue(entindex(), infobuffer, "model", model);
	if (strcmp(curcolor, color))
	{
		g_engfuncs.pfnSetClientKeyValue(entindex(), infobuffer, "topcolor", color);
		g_engfuncs.pfnSetClientKeyValue(entindex(), infobuffer, "bottomcolor", color);
	}

	static char szModelPath[64];
	sprintf(szModelPath, "models/rooster_fortress/player/%s/%s.mdl", model, model);
	SET_MODEL(ENT(pev), szModelPath);
}

void CRFPlayer::CheckTeamFortressConditions()
{
	// constant checking for tfcond stuff

	if (m_iClass == CLASS_PYRO && !IsInCond(TF_COND_AFTERBURN_IMMUNE))
	{
		// pyros are constantly checked just in case
		AddCondition(TF_COND_AFTERBURN_IMMUNE, PERMANENT_CONDITION, this, -1.0);
	}

	if (IsInCond(TF_COND_AIMING) || IsInCond(TF_COND_STUNNED))
	{
		SetResetMaxSpeed(0.3);
		if (pev->bInDuck) // if in duck state, then just zero speed out
		{
			// ALERT(at_console, "zero'd aiming max speed");
			pev->velocity[0] = pev->velocity[1] = 0.0;
			SetResetMaxSpeed(0.0);
		}
	}
	if (IsInCond(TF_COND_SPEED_BOOST) || IsInCond(TF_COND_HALLOWEEN_SPEED_BOOST))
	{
		float speed = GetClassMaxSpeed(m_iClass);
		speed += V_min(speed * 0.4, 105.0);
		g_engfuncs.pfnSetClientMaxspeed(ENT(pev), speed);
		pev->maxspeed = speed;
	}
	if (IsInCond(TF_COND_BLASTJUMPING))
	{
		// if ur on the ground while rocket-jumping, then remove condition
		if ((pev->flags & FL_ONGROUND)) //&& pev->groundentity)
		{
			RemoveCondition(TF_COND_BLASTJUMPING);
		}
	}


	// TIME BASED TFCOND's
	// TFCONDS THAT ARE ON AN INTERVAL
	// kept seperate for organization reasons
	// this prevents these types of conditions from constantly triggering and being messy

	if (IsInCond(TF_COND_BURNING))
	{
		if (gpGlobals->time >= m_iTFConds_Timer[TF_COND_BURNING])
		{
			m_iTFConds_Timer[TF_COND_BURNING] = gpGlobals->time + 0.5;

			if (!IsInCond(TF_COND_AFTERBURN_IMMUNE)) // if immune then ignore
			{
				edict_t* index = INDEXENT(m_iTFConds[TF_COND_BURNING][1]);
				CBaseEntity* p = index ? Instance(index) : NULL;
				if (p)
				{
					entvars_t* pointer = p->pev;

					TakeDamage(pointer, pointer, 4, DMG_SLOWBURN);

					MESSAGE_BEGIN(MSG_BROADCAST, SVC_TEMPENTITY, this->pev->origin);
					WRITE_BYTE(TE_SMOKE);
					WRITE_COORD(this->pev->origin.x);
					WRITE_COORD(this->pev->origin.y);
					WRITE_COORD(this->pev->origin.z);
					WRITE_SHORT(g_sModelIndexSmoke); // TODO: swap for a precached custom on-fire sprite
					WRITE_BYTE(6);						   // size
					WRITE_BYTE(30);						   // fps
					MESSAGE_END();
				}
			}
		}
	}

	if (IsInCond(TF_COND_BLEEDING))
	{
		if (gpGlobals->time >= m_iTFConds_Timer[TF_COND_BLEEDING])
		{
			m_iTFConds_Timer[TF_COND_BLEEDING] = gpGlobals->time + 0.5;

			edict_t* index = INDEXENT(m_iTFConds[TF_COND_BLEEDING][1]);
			CBaseEntity* p = index ? Instance(index) : NULL;
			if (p)
			{
				entvars_t* pointer = p->pev;

				TakeDamage(pointer, pointer, 4, DMG_SLOWBURN);

				MESSAGE_BEGIN(MSG_BROADCAST, SVC_TEMPENTITY, this->pev->origin);
				WRITE_BYTE(TE_SMOKE);
				WRITE_COORD(this->pev->origin.x);
				WRITE_COORD(this->pev->origin.y);
				WRITE_COORD(this->pev->origin.z);
				WRITE_SHORT(g_sModelIndexSmoke); // TODO: swap for a precached custom bleed sprite
				WRITE_BYTE(6);						   // size
				WRITE_BYTE(30);						   // fps
				MESSAGE_END();
			}
		}
	}

}

void CRFPlayer::AddCondition(ETFCond eCond, float flDuration /* = PERMANENT_CONDITION */, CBaseEntity* pProvider /*= NULL */, float timeBetweenTrigger)
{
	// most logic converted from tf2 sdk

	if (!(eCond > -1 && eCond < TF_COND_LAST))
		return; // invalid condition bro o_0

	if (!IsAlive())
		return;

	// sanity check to prevent servers from adding these conditions when they shouldn't
	if ((eCond == TF_COND_COMPETITIVE_WINNER) || (eCond == TF_COND_COMPETITIVE_LOSER))
	{
		// if (g_fGameOver) // look for this in gamerules.h or wherever else
			return; // GAME OVER BRO!
	}

	if (flDuration != PERMANENT_CONDITION)
		m_iTFConds[eCond][0] = gpGlobals->time + flDuration; // after the global time reaches this, then the condition is no longer active
	else
		m_iTFConds[eCond][0] = -1; // infinite

	if (!pProvider)
		pProvider = this;

	m_iTFConds[eCond][1] = pProvider->entindex();

	m_iTFConds[eCond][2] = timeBetweenTrigger;

	m_iTFConds_Timer[eCond] = gpGlobals->time + timeBetweenTrigger;

	// [x][0] = m_flExpireTime -- the time the condition runs out ( set this to -1 so it lasts forever! )
	// [x][1] = m_pProvider -- who gave you the condition
	// [x][2] = how often the condition gets triggered, if it is a non-constant condition
	// this prevents these types of conditions from constantly triggering and causing unwanted shit
	// SOME OF THESE ARE HARDCODED!!! ( afterburn, bleeding )
	// m_iTFConds_Timer[x] = the last time the non-constant condition triggered
	// [x] = the condition itself

}

void CRFPlayer::RemoveCondition(ETFCond eCond)
{
	// most logic from tf2 sdk

	if (!(eCond > -1 && eCond < TF_COND_LAST))
		return; // invalid condition bro o_0

	if (!IsInCond(eCond))
		return;

	m_iTFConds[eCond][0] = gpGlobals->time - 1.0;
	// forces the clear time to occur
	// ex: condition runs until time 6
	// current time is 3
	// this forces the condition to run until the current time minus one, so it gets set to time 2
	// since that time has passed, then IsInCond will return false
	// THATS WHY IT'S IMPORTANT TO USE IsInCond!!!!!!!!!!

	if (m_iTFConds[eCond][0] == -1)
		m_iTFConds[eCond][0] = 0;
	// special case for infinite conditions

	m_iTFConds[eCond][1] = NULL;
	m_iTFConds[eCond][2] = -1.0;
	m_iTFConds_Timer[eCond] = -1.0;
	// [x][0] = m_flExpireTime -- the time the condition runs out ( set this to -1 so it lasts forever! )
	// [x][1] = m_pProvider -- who gave you the condition
	// [x][2] = how often the condition gets triggered, if it is a non-constant condition
	// this prevents these types of conditions from constantly triggering and causing unwanted shit
	// SOME OF THESE ARE HARDCODED!!! ( afterburn, bleeding )
	// m_iTFConds_Timer[x] = the last time the non-constant condition triggered
	// [x] = the condition itself
}

void CRFPlayer::RemoveAllConditions()
{
	for (int eCond = 0; eCond < TF_COND_LAST; eCond++)
	{
		m_iTFConds[eCond][0] = 0;
		m_iTFConds[eCond][1] = NULL;
		m_iTFConds[eCond][2] = -1.0;
		m_iTFConds_Timer[eCond] = -1.0;

		// alternatively just use RemoveCondition and put eCond in it
	}
}

bool CRFPlayer::IsInCond(ETFCond eCond)
{
	if (!(eCond > -1 && eCond < TF_COND_LAST))
		return false; // invalid condition bro o_0

	if (m_iTFConds[eCond][0] == -1) // -1 is infinite
		return true;

	if (m_iTFConds[eCond][0] > gpGlobals->time) // if time hasnt ended for the condition, then ur in the condition silly!!!
		return true;

	return false;
}

bool CRFPlayer::GetUberStatus()
{
	if (IsInCond(TF_COND_INVULNERABLE)
		|| IsInCond(TF_COND_INVULNERABLE_HIDE_UNLESS_DAMAGED)
		|| IsInCond(TF_COND_INVULNERABLE_CARD_EFFECT)
		|| IsInCond(TF_COND_INVULNERABLE_USER_BUFF)
		|| IsInCond(TF_COND_INVULNERABLE_WEARINGOFF)
		)
	{
		return true;
	}

	return false;
}

inline bool GetRandomCrit()
{
	return RANDOM_FLOAT(0.0, 10.0) < 2.0;
}

inline bool GetCritBoost()
{
	return RANDOM_FLOAT(0.0, 10.0) < 2.0;
}

int CRFPlayer::GetCritStatus(int type)
{
	if (type == ALL_CRITS)
	{
		float random = RANDOM_LONG(0, 100);
		if (random > 98)
		{
			//	ALERT(at_console, "Random Crit Rolled as %f\n", random);
			return FULL_CRITS;
		}
		// random crit stuff
	}

	if (IsInCond(TF_COND_CRITBOOSTED)
		|| IsInCond(TF_COND_CRITBOOSTED_ON_KILL)
		|| IsInCond(TF_COND_CRITBOOSTED_PUMPKIN)
		|| IsInCond(TF_COND_CRITBOOSTED_RUNE_TEMP)
		|| IsInCond(TF_COND_CRITBOOSTED_CTF_CAPTURE)
		|| IsInCond(TF_COND_CRITBOOSTED_USER_BUFF)
		|| IsInCond(TF_COND_CRITBOOSTED_RAGE_BUFF)
		|| IsInCond(TF_COND_CRITBOOSTED_BONUS_TIME)
		|| IsInCond(TF_COND_CRITBOOSTED_DEMO_CHARGE)
		|| IsInCond(TF_COND_CRITBOOSTED_CARD_EFFECT)
		|| IsInCond(TF_COND_CRITBOOSTED_FIRST_BLOOD))
		return FULL_CRITS;
	// crit cond stuff

	if (IsInCond(TF_COND_MINICRITBOOSTED_ON_KILL) || IsInCond(TF_COND_SHIELD_CHARGE) || IsInCond(TF_COND_OFFENSEBUFF))
		return MINI_CRITS;
	// minicrit cond stuff

	return NONE;
}

void CRFPlayer::DeathSound()
{
	// water death sounds
	/*
	if (pev->waterlevel == 3)
	{
		EMIT_SOUND(ENT(pev), CHAN_VOICE, "player/h2odeath.wav", 1, ATTN_NONE);
		return;
	}
	*/

	// TODO: if player died via critical hits, then play the death sound

	// temporarily using pain sounds for death sounds
	switch (RANDOM_LONG(1, 5))
	{
	case 1:
		EMIT_SOUND(ENT(pev), CHAN_VOICE, "player/pl_pain5.wav", 1, ATTN_NORM);
		break;
	case 2:
		EMIT_SOUND(ENT(pev), CHAN_VOICE, "player/pl_pain6.wav", 1, ATTN_NORM);
		break;
	case 3:
		EMIT_SOUND(ENT(pev), CHAN_VOICE, "player/pl_pain7.wav", 1, ATTN_NORM);
		break;
	}
}