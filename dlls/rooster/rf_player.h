#pragma once
#include "player.h"        // CBasePlayer base class (needs extdll/util/cbase included first)
#include "tf_shareddefs.h" // ETFCond, TF_COND_*, CLASS_*, TEAM_*, COLOR_*, PERMANENT_CONDITION

enum RandomCritType
{
	ALL_CRITS,			// status based crits + random crits
	NO_RANDOM_CRITS,	// status based crits only
	NO_CRITS			// no crits allowed. usually these weapons have their own way of scoring crits ( sniper rifles, knives )
};

enum CritStatusResult
{
	FULL_CRITS,
	MINI_CRITS,
	NONE
};

class CRFPlayer : public CBasePlayer
{
public:
	void Spawn(void);
    void Precache(void);
    void PreThink(void);
    void PostThink(void);
    void Killed(entvars_t* pevAttacker, int iGib);
    int  TakeDamage(entvars_t* pevInflictor, entvars_t* pevAttacker, float flDamage, int bitsDamageType);
    int  TakeHealth(float flHealth, int bitsDamageType);

    int  Save(CSave& save);
    int  Restore(CRestore& restore);
	int GetTeamNumber();
	void SetTeamNumber(int newTeam);
	int GetClassMaxHealth(int iClass);
	void ResetMaxSpeed();
	void SetResetMaxSpeed(float percent);
	void SetPlayerModel();
	void DeathSound();
    static TYPEDESCRIPTION m_rfSaveData[];

    int m_iClass;   // scout/soldier/etc
	int m_iTeam;	// team red/blue
    int m_nCond;    // TF_COND_* bitfield
    int m_iAirJump; // called m_iAirDash in tf2

	int m_iTFConds[TF_COND_LAST][3];
	float m_iTFConds_Timer[TF_COND_LAST];
	// table of tf2 conditions stuff to keep track of
	// [x][0] = m_flExpireTime -- the time the condition runs out ( set this to -1 so it lasts forever! )
	// [x][1] = m_pProvider -- who gave you the condition
	// [x][2] = how often the condition gets triggered, if it is a non-constant condition
	// this prevents these types of conditions from constantly triggering and causing unwanted shit
	// SOME OF THESE ARE HARDCODED!!! ( afterburn, bleeding )
	// [x][3] = the last time the non-constant condition triggered
	// [x] = the condition itself
	void AddCondition(ETFCond eCond, float flDuration /* = PERMANENT_CONDITION */, CBaseEntity* pProvider /*= NULL */, float timeBetweenTrigger = -1.0);
	void RemoveCondition(ETFCond eCond);
	void RemoveAllConditions();
	void CheckTeamFortressConditions();
	bool IsInCond(ETFCond eCond);

	// flamethrower bookkeeping (see wpn_flamethrower.cpp) -- live TF2 gates
	// flame damage per target and ramps it up with sustained contact
//	float m_flLastFlameDamageTime; // last time a flame damaged this player (0.075s gate)
//	int m_iLastFlameAttacker;	   // entindex of that flame's owner -- the gate is per
	// attacker in TF2, so two pyros both deal full DPS
//	float m_flFlameHeat;		   // heat index: 0..60, higher = taking full flame damage
//	float m_flFlameHeatTime;	   // when the heat was last updated (decays lazily)

	int GetCritStatus(int type);
	// handles everything crit related minus special things like headshots n backstabs

	bool GetUberStatus();
	// ubercharge status
	// includes wearing off

	// void SendConditionStatusUpdates();
	// sends gmsgCritStatus / gmsgUberStatus to this client whenever the crit or uber
	// state changes; called both while alive and on death so the effects clear properly
	// not ported yet

	int m_bWasCritBoosted;
	int m_bWasUbered;
	// for gmsgCritStatus and gmsgUberStatus
};

// Free helper (not a member): turn any entity into a CRFPlayer*, or NULL if it sn't a player.
// Every player, human or CHLBot, is a CRFPlayer so the cast is safe.
CRFPlayer* ToRFPlayer(CBaseEntity* pEntity);