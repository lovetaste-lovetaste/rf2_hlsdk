// hl_bot.h
// A simple navigation-mesh-driven bot for Half-Life deathmatch.
//
// This is NOT a port of the CS/CZ bot AI. It is a small, readable bot that
// exercises the ported nav mesh:
//
//   * If it can see an enemy, it attacks:
//       - with a usable ranged weapon it fires while strafing randomly
//       - with only the crowbar it runs straight at the enemy and swings
//   * If it cannot see an enemy and is armed (any non-crowbar weapon with
//     ammo), it paths to the nearest enemy.
//   * Otherwise it paths to the nearest weapon it doesn't have (weapons the
//     bot owns but has no ammo for count as "not had").
//
// Spawn one with the "bot_add [name]" server command (see hl_bot_manager.cpp).
//
// The fake client plumbing (RunPlayerMove command interface, think
// throttling) follows the proven CZ CBot pattern.

#ifndef HL_BOT_H
#define HL_BOT_H

#include "nav_path.h"
#include "rooster/rf_player.h"	// CHLBot derives from CRFPlayer so bots carry TF2 state (class, conditions)

class CBasePlayerItem;
class CBasePlayerWeapon;

// NOTE: base is CRFPlayer, NOT CBasePlayer. A bot must BE a CRFPlayer so that
// ToRFPlayer()/condition/class code sees the RF members at the right offsets.
// If this were CBasePlayer, those accesses would run past the allocation.
class CHLBot : public CRFPlayer
{
public:
	/// create a fake client, allocate it as a CHLBot, and put it in the server
	static CHLBot *CreateBot( const char *name );

	virtual void Spawn( void );

	// prevent bots from being blinded by their own (or others') flashbang-like effects
	virtual int ObjectCaps( void ) { return CBasePlayer::ObjectCaps() & ~FCAP_ACROSS_TRANSITION; }

	Vector m_viewTarget;
	bool m_lookatViewTarget;
	// where the bot looks at in Upkeep()
	// m_lookatViewTarget ensures the bot actually WANTS to look at that position. Set at Update(), used in Upkeep()

	void BotThink( void );			///< invoked once per server frame by CHLBotManager

private:
	//------------------------------------------------------------------------------------------------------
	// Thinking
	//------------------------------------------------------------------------------------------------------
	void Update( void );						// decision logic
	void UpdateAttack( CBasePlayer *enemy, bool movement = true );	// combat behavior while an enemy is visible
	void UpdateNavigation( void );				// pick a travel goal and follow the nav mesh to it
	void Upkeep(void);							// maintains the bot's aim and movement between updates, ensures smoothness

	//------------------------------------------------------------------------------------------------------
	// Perception and inventory
	//------------------------------------------------------------------------------------------------------
	CBasePlayer *SelectVisibleEnemy( void );	///< return the visible enemy with the biggest TargetCost(), or NULL
	float TargetCost( CBasePlayer *player, CBasePlayer *currentEnemy ) const;	///< target desirability - bigger = better
	bool IsEnemy( CBaseEntity *ent ) const;		///< return true if the given entity is an enemy player

	bool IsWeaponUsable( CBasePlayerWeapon *gun ) const;	///< true if the weapon has any ammo to fire
	CBasePlayerWeapon *GetBestRangedWeapon( bool ignoreGlock = false ) const;	///< best owned non-crowbar weapon with ammo, or NULL
	bool IsArmed( void ) const					{ return GetBestRangedWeapon() != NULL; }
	bool HasUsableWeapon( CBaseEntity *item ) const;		///< true if we own this map pickup AND have ammo for it

	CBaseEntity *FindNearestWantedWeapon( void );	///< nearest weapon pickup we don't have (or have no ammo for)
	CBaseEntity *FindNearestWeaponUpgrade( float maxRange );	///< nearest pickup whose weight beats our best owned gun
	CBaseEntity *FindNearestRestock( void );	///< nearest health/armor/ammo top-off we could use, or NULL when stocked
	void UpdateRoam( void );					///< wander the map when there is nothing else to do
	CBasePlayer *FindNearestEnemy( void );			///< nearest living enemy, visible or not
	CBaseEntity* FindNearestWantedHealth(bool visible = FALSE);
	//------------------------------------------------------------------------------------------------------
	// Path following
	//------------------------------------------------------------------------------------------------------
	void MoveTowardGoal( const Vector &goal );	///< recompute the path if needed and follow it
	void FollowPath( void );					///< steer along m_path
	void MoveTowardPos( const Vector &pos );	///< walk toward a position without touching the view (Bot_MoveToPos style)
	bool IsOnLadder( void ) const;				///< true while PM_LadderMove owns us (movetype FLY)
	bool GetLookaheadPos( float flLookAheadRange, Vector *pvBuffer );	///< a natural look-at point N units ahead along the path
	void LadderClimb( const Vector &goal );		///< climb toward the goal - solved buttons + pitch, yaw untouched
	void UpdateStuckMonitor( void );			///< detect being stuck; jump or repath to recover

	//------------------------------------------------------------------------------------------------------
	// Command interface (CZ CBot pattern): accumulate intent, then send one
	// movement command to the engine via pfnRunPlayerMove
	//------------------------------------------------------------------------------------------------------
	void ResetCommand( void );
	void ExecuteCommand( void );
	byte ThrottledMsec( void ) const;

	float GetMoveSpeed( void ) const;
	void MoveForward( void );
	void StrafeLeft( void );
	void StrafeRight( void );
	bool PressJump( void );		///< (CBasePlayer already has a virtual Jump(), hence the name)
	void PressPrimaryAttack( void );
	void PressSecondaryAttack( void );
	void GaussAttack( CBasePlayer *enemy, CBasePlayerWeapon *gun, float flWallDamage = 0.0f );	///< charge the gauss just enough to kill (+wall), then release
	float DamageToKill( CBasePlayer *player ) const;				///< one-hit kill damage through HL's armor rule
	bool CanGaussWallbang( CBasePlayer *enemy, float *pflWallDamage );	///< can a charged beam punch through to them? outputs the damage lost
	bool UpdateGaussWallbang( bool action );								///< charge a shot through a wall at the nearest enemy; true while engaged
	bool IsGaussCharging( void ) const;								///< true while our gauss holds a secondary charge (don't disturb the view!)
	CBasePlayerWeapon *GetOwnedWeapon( int iId ) const;				///< our copy of the given WEAPON_* id, or NULL
	void AimAt( const Vector &spot );			///< point v_angle at the given world position
	void SetAimAt(const Vector& spot);

	float m_forwardSpeed;
	float m_strafeSpeed;
	float m_verticalSpeed;
	unsigned short m_buttonFlags;

	float m_flNextBotThink;
	float m_flNextFullBotThink;
	float m_flPreviousCommandTime;
	float m_jumpTimestamp;
	float m_flLastAimTime;		///< when AimAt() last ran - its turn steps scale by elapsed time

	//------------------------------------------------------------------------------------------------------
	// Navigation state
	//------------------------------------------------------------------------------------------------------
	CNavPath m_path;
	int m_pathIndex;					///< the path segment we are moving toward
	Vector m_pathGoal;					///< where the current path leads
	CountdownTimer m_repathTimer;		///< limits how often we recompute the path

	Vector m_stuckSpot;					///< where we were when the stuck check last ran
	IntervalTimer m_stuckTimer;			///< how long we have been near m_stuckSpot

	// roaming (see UpdateRoam())
	enum { ROAM_MEMORY = 4 };			///< how many recent roam areas we avoid re-picking
	Vector m_roamGoal;					///< where we are currently wandering to
	CountdownTimer m_roamTimer;			///< gives up on an unreachable roam spot
	unsigned int m_recentRoamAreas[ ROAM_MEMORY ];	///< nav area IDs of recent roam goals
	int m_recentRoamHead;				///< ring buffer write position

	//------------------------------------------------------------------------------------------------------
	// Combat state
	//------------------------------------------------------------------------------------------------------
	CountdownTimer m_strafeTimer;		///< when elapsed, pick a new random strafe direction
	int m_strafeDir;					///< -1 = left, 0 = none, +1 = right

	EHANDLE m_hEnemy;					///< current target; gets a TargetCost() bonus so we don't rapidly
										///< switch targets. An EHANDLE, so disconnects resolve to NULL
										///< instead of a dangling pointer.
};

#endif // HL_BOT_H
