// hl_bot.cpp
// A simple navigation-mesh-driven bot for Half-Life deathmatch.
// See hl_bot.h for an overview of its behavior.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "gamerules.h"
#include "client.h"		// ClientConnect()/ClientPutInServer()
#include "airstrafe.h"

#include "hl_bot.h"
#include "hl_bot_manager.h"
#include "bot_util.h"
#include "nav_area.h"

// command pacing, matching human client rates (CZ CBot values)
static const float BotCommandInterval = 1.0f / 30.0f;	// send movement commands 30 times per second
static const float BotFullThinkInterval = 1.0f / 30.0f;	// run decision logic 30 times per second

// Ladder climbing style: 0 = a single fore/back key (normal climb, 200 u/s),
// 1 = fore/back + strafe held together - the engine sums both key vectors
// without normalizing, so the climb runs ~1.4x faster (the classic fast-
// ladder trick). Works at any camera yaw either way; see LadderClimb().
// TODO: swap for a per-bot difficulty boolean once difficulty values exist.
#define BOT_LADDER_FAST_CLIMB 1

// movement tuning
static const float BotMoveSpeed = 270.0f;				///< HLDM run speed
static const float WaypointTouchRange = 36.0f;			///< how close (2D) we must get to a waypoint to advance past it
static const float MeleeAttackRange = 72.0f;			///< crowbar swing reach

//--------------------------------------------------------------------------------------------------------------
/**
 * Map weapon classnames the bot can look for on the ground.
 * Both the retail names and common aliases are listed; map entities link
 * to the same weapon classes either way.
 */
static const char *wantedWeaponClassnames[] =
{
	"weapon_9mmhandgun", "weapon_glock",
	"weapon_357", "weapon_python",
	"weapon_9mmAR", "weapon_mp5",
	"weapon_shotgun",
	"weapon_crossbow",
	"weapon_rpg",
	"weapon_gauss",
	"weapon_egon",
	"weapon_hornetgun",
	"weapon_handgrenade",
	"weapon_satchel",
	"weapon_tripmine",
	"weapon_snark",
	NULL
};

float AngleNormalize(float angle)
{
	angle = fmodf(angle, 360.0);
	if (angle > 180)
	{
		angle -= 360;
	}
	if (angle < -180)
	{
		angle += 360;
	}

	return angle;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Create a fake client, allocate its private data as a CHLBot (so
 * ClientPutInServer()'s GetClassPtr() reuses our instance), and connect it
 */
CHLBot *CHLBot::CreateBot( const char *name )
{
	if (UTIL_ClientsInGame() >= gpGlobals->maxClients)
	{
		CONSOLE_ECHO( "bot_add: Server is full.\n" );
		return NULL;
	}

	// pick a default name if none was given (or the given one is taken)
	char botName[64];
	if (name && *name && !UTIL_IsNameTaken( name ))
	{
		strncpy( botName, name, sizeof( botName ) - 1 );
		botName[ sizeof( botName ) - 1 ] = '\0';
	}
	else
	{
		int serial = 1;
		do
		{
			sprintf( botName, "Bot%02d", serial++ );
		}
		while( UTIL_IsNameTaken( botName ) && serial < 100 );
	}

	edict_t *pent = CREATE_FAKE_CLIENT( botName );
	if (FNullEnt( pent ))
	{
		CONSOLE_ECHO( "bot_add: pfnCreateFakeClient() failed - no free client slots?\n" );
		return NULL;
	}

	// run the normal connection path
	char reject[128];
	if (0 == ClientConnect( pent, STRING( pent->v.netname ), "127.0.0.1", reject ))
	{
		SERVER_COMMAND( UTIL_VarArgs( "kick \"%s\"\n", STRING( pent->v.netname ) ) );
		return NULL;
	}

	// give the bot a player model
	char *infobuffer = GET_USERINFO( pent );
	SET_CLIENT_KEY_VALUE( ENTINDEX( pent ), infobuffer, "model", "gordon" );

	// allocate our private data as CHLBot BEFORE ClientPutInServer -
	// its GetClassPtr() call then reuses this instance instead of
	// allocating a plain CBasePlayer
	FREE_PRIVATE( pent );
	CHLBot *bot = GetClassPtr( (CHLBot *)VARS( pent ) );

	ClientPutInServer( pent );

	return bot;
}

void CHLBot::Spawn( void )
{
	// let the game set everything up
	CRFPlayer::Spawn();

	// make sure everyone knows we are a bot
	pev->flags |= ( FL_CLIENT | FL_FAKECLIENT );

	// bots use their own thinking mechanism (BotThink() from the manager)
	SetThink( NULL );
	pev->nextthink = -1;

	m_flNextBotThink = gpGlobals->time + BotCommandInterval;
	m_flNextFullBotThink = gpGlobals->time + BotFullThinkInterval;
	m_flPreviousCommandTime = gpGlobals->time;
	m_jumpTimestamp = 0.0f;
	m_flLastAimTime = 0.0f;		// forces the first AimAt() to snap
	m_lookatViewTarget = false;

	m_path.Invalidate();
	m_pathIndex = 0;
	m_pathGoal = pev->origin;
	m_repathTimer.Invalidate();

	m_stuckSpot = pev->origin;
	m_stuckTimer.Start();

	m_roamGoal = pev->origin;
	m_roamTimer.Invalidate();
	for( int r = 0; r < ROAM_MEMORY; ++r )
		m_recentRoamAreas[r] = 0;
	m_recentRoamHead = 0;

	m_strafeTimer.Invalidate();
	m_strafeDir = 0;
	m_hEnemy = NULL;	// no target carries over into a fresh life

	ResetCommand();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Heartbeat, invoked once per server frame by CHLBotManager::StartFrame().
 */
void CHLBot::BotThink( void )
{
	if (gpGlobals->time < m_flNextBotThink)
		return;

	m_flNextBotThink = gpGlobals->time + BotCommandInterval;

	if (!IsAlive())
	{
		// press attack to respawn (PlayerDeathThink() waits for a button)
		ResetCommand();
		AirStrafe_Reset(this);
		if (pev->deadflag == DEAD_RESPAWNABLE)
		{
			m_buttonFlags = IN_ATTACK;
			m_buttonFlags = IN_JUMP;
		}
		
		ExecuteCommand();
		m_path.Invalidate();
		return;
	}

	if (cv_bot_stop.value != 0.0f)
		return;

	if (gpGlobals->time >= m_flNextFullBotThink)
	{
		m_flNextFullBotThink = gpGlobals->time + BotFullThinkInterval;
		m_lookatViewTarget = false;

		ResetCommand();
		Update();
	}

	// maintains inbetween stuff
	// stuff like aim looks weird when only being ran on specific ticks. it isnt smooth
	// this aims the stuff instead. since its being ran every think, keep this function cheap
	// (runs on Update() ticks too - Update() only picks the aim target, Upkeep() turns toward it)
	Upkeep();

	ExecuteCommand();
}

// decision logic
void CHLBot::Update( void )
{
	UpdateStuckMonitor();
	CBaseEntity* visibleHealthKit = FindNearestWantedHealth(true);
	CBasePlayer *visibleEnemy = SelectVisibleEnemy();
	CBasePlayerWeapon* gun = GetBestRangedWeapon();

	if (visibleEnemy)
	{
		if (gun && m_path.IsValid()) // has a gun and has a path, keep going on path
			UpdateAttack(visibleEnemy,false);
		else
		{
			m_path.Invalidate();	// fighting now - stale paths are useless
			UpdateAttack(visibleEnemy, true);
			return;
		}
	}
	else if (UpdateGaussWallbang(true))
	{
		// gauss shot
		return;
	}
	else if( visibleHealthKit )
	{
		if (pev->health <= 50.0)
		{
			SetAimAt(visibleHealthKit->pev->origin);
		//	Vector to = visibleHealthKit->pev->origin - pev->origin;
		//	if (to.Length() <= 100.0)
		//	{
		//		SetBits(m_buttonFlags, IN_USE);
		//	}
		// i thought item_healthkit was the hp kits on the wall. but they arent LOL so this isnt needed
		}
	}

	UpdateNavigation();
}

void CHLBot::Upkeep(void)
{
	if (m_lookatViewTarget)
	{
		CBasePlayer* currentEnemy = SelectVisibleEnemy();
		if (currentEnemy && (!currentEnemy->IsPlayer() || !currentEnemy->IsAlive()))
			currentEnemy = NULL;
		if (AirStrafe_IsAirborne(this))
		{
			// NEVER airstrafe while the gauss holds a charge: the strafe
			// owns the view yaw and drops ATTACK2, so the charge would be
			// wrenched around and released at nothing. This is a direct
			// weapon-state check - the wallbang check below only covers
			// charges whose wallbang is still viable
			bool canStrafe = !IsGaussCharging() && !UpdateGaussWallbang(false);

			if (canStrafe && currentEnemy)
			{
				// with an enemy up, only strafe when we couldn't meaningfully
				// shoot anyway: they're far off, or the gun is between shots
				const float strafeEnemyRange = 1024.0f;

				bool farAway = (currentEnemy->pev->origin - pev->origin).IsLengthGreaterThan( strafeEnemyRange );

				CBasePlayerWeapon *gun = m_pActiveItem ? (CBasePlayerWeapon *)m_pActiveItem->GetWeaponPtr() : NULL;
				bool onCooldown = (gun != NULL) && (gun->m_flNextPrimaryAttack > UTIL_WeaponTimeBase());

				if (!farAway && !onCooldown)
					canStrafe = false;
			}

			if (canStrafe)
			{
				float deltaT = gpGlobals->time - m_flPreviousCommandTime;
				int buttons = m_buttonFlags;
				float fwd, side;
				if (Bot_AirStrafeTo(this, m_viewTarget, deltaT, &buttons, &fwd, &side))
				{
					m_buttonFlags = buttons;
					m_forwardSpeed = fwd;
					m_strafeSpeed = side;

					// no firing while strafing past an enemy - the strafe
					// owns the view, so shots would spray wherever it
					// happens to be looking mid-carve
					if (currentEnemy)
						ClearBits( m_buttonFlags, IN_ATTACK | IN_ATTACK2 );

					return;   // airstrafe owns the view this tick
				}
			}
		}
		else
		{
			AimAt(m_viewTarget);
			/*
			Vector2D vVel(pev->velocity.x, pev->velocity.y);
			float flSpeed = vVel.Length();
			if (flSpeed >= BotMoveSpeed)
				PressJump();
			*/
		}
	}
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true while our gauss is holding a secondary charge. While this is
 * true nothing should disturb the view or the buttons - the charge fires
 * the moment ATTACK2 is released, wherever we happen to be looking.
 */
bool CHLBot::IsGaussCharging( void ) const
{
	if (m_pActiveItem == NULL)
		return false;

	if (m_pActiveItem->m_iId != WEAPON_GAUSS)
		return false;

	return m_pActiveItem->m_fInAttack != 0;
}

// Combat, while an enemy is visible
void CHLBot::UpdateAttack( CBasePlayer *enemy, bool movement )
{
	// aim at the enemy's center of mass
	SetAimAt( enemy->Center() );
	
	CBasePlayerWeapon *gun = GetBestRangedWeapon((enemy->pev->origin - pev->origin).IsLengthLessThan(MeleeAttackRange));
	if (gun)
	{
		// make sure our best gun is in hand
		if (m_pActiveItem != gun)
		{
			SwitchWeapon( gun );
			return;		// deploying takes a moment, so fire next think
		}


		if (gun->m_iId == WEAPON_GAUSS)
			GaussAttack( enemy, gun );
		else
			PressPrimaryAttack();

		if (movement)
		{
			if (m_strafeTimer.IsElapsed())
			{
				m_strafeDir = RANDOM_LONG(0, 2) - 1;		// -1, 0 or +1
				m_strafeTimer.Start(RANDOM_FLOAT(0.4f, 1.2f));
			}

			if (m_strafeDir < 0)
				StrafeLeft();
			else if (m_strafeDir > 0)
				StrafeRight();
		}
	}
	else
	{
		// melee combat: charge the enemy
		if (m_pActiveItem == NULL || !FClassnameIs( m_pActiveItem->pev, "weapon_crowbar" ))
			SelectItem( "weapon_crowbar" );

		if (movement)
		{
			MoveForward();
		}

		if ((enemy->pev->origin - pev->origin).IsLengthLessThan( MeleeAttackRange ))
			PressPrimaryAttack();

		// hop over small obstacles between us and the enemy
		if (pev->flags & FL_ONGROUND)
		{
			Vector velocity2D( pev->velocity.x, pev->velocity.y, 0 );
			if (velocity2D.IsLengthLessThan( 50.0f ) && m_stuckTimer.IsGreaterThen( 0.5f ))
				PressJump();
		}
	}
}

//--------------------------------------------------------------------------------------------------------------
/**
 * The raw damage a single hit must deal to kill the given player through
 * their armor, using HL's exact absorption rule (ratio 0.2, bonus 0.5):
 * armor eats 80% of incoming damage and each armor point blocks 2 damage,
 * so a kill needs health + 2*armor... UNLESS the armor pool is deep enough
 * to never deplete, where only the 20% leak damages health (5*health).
 */
float CHLBot::DamageToKill( CBasePlayer *player ) const
{
	float flHealth = player->pev->health;
	float flArmor = player->pev->armorvalue;

	if (flHealth >= 0.5f * flArmor)
		return flHealth + 2.0f * flArmor;	// armor depletes during the hit

	return 5.0f * flHealth;					// armor survives; only the leak kills
}

//--------------------------------------------------------------------------------------------------------------
//	Trigger discipline for the gauss cannon: charge the secondary just long enough to kill the enemy in one hit.
//	flWallDamage: extra damage the shot must carry to survive geometry in the way (wallbangs - see
//	UpdateGaussWallbang()). The charged beam loses damage equal to the wall thickness it punches, so the
//	charge is sized for kill + wall; if even a full charge can't cover both, it charges to max anyway and
//	takes the chip damage - the gauss can't go any higher.

void CHLBot::GaussAttack( CBasePlayer *enemy, CBasePlayerWeapon *gun, float flWallDamage )
{
	const float gaussMaxChargeDamage = 200.0f;	// MP secondary cap (gauss.cpp)
	const float gaussPrimaryDamage = 20.0f;		// flat primary orb (gauss.cpp)
	const float overkillMargin = 1.1f;			// charge 10% extra so a sliver of lag doesn't leave 2hp

	float flNeeded = DamageToKill( enemy ) * overkillMargin + flWallDamage;

	// a primary orb already kills them - no point standing around charging
	// (never for wallbangs: the primary orb can't punch through anything)
	if (flWallDamage <= 0.0f && flNeeded <= gaussPrimaryDamage && gun->m_fInAttack == 0)
	{
		PressPrimaryAttack();
		return;
	}

	// how long the charge must be held for that much damage (capped at full)
	CGauss *gauss = (CGauss *)gun;
	float flFraction = flNeeded / gaussMaxChargeDamage;
	if (flFraction > 1.0f)
		flFraction = 1.0f;		// can't go higher - full charge will have to do

	float flHoldTime = gauss->GetFullChargeTime() * flFraction;

	if (gun->m_fInAttack == 0)
	{
		// start charging (the weapon sets m_flStartCharge when it processes this)
		PressSecondaryAttack();
		return;
	}

	// charging - hold until the ramp reaches the needed damage, then let go
	if (gpGlobals->time - m_flStartCharge < flHoldTime)
		PressSecondaryAttack();
	// else: release - not pressing ATTACK2 this tick is the trigger pull
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Can a charged gauss beam from our eyes reach the given (non-visible)
 * enemy through the geometry in the way? If so, return true and set
 * *pflWallDamage to the damage the beam will lose punching through.
 *
 * Mirrors the rules in CGauss::Fire():
 *   - the beam only penetrates when it strikes within ~60 degrees of the
 *     surface normal (dot >= 0.5); flatter hits reflect instead
 *   - penetration costs damage equal to the wall's thickness in units
 *
 * Thickness is measured with the classic two-way trace: entry point from
 * our side, exit point from the enemy's side, distance between = occluded
 * span. With several walls stacked the span includes the air gaps too, so
 * the estimate only ever OVERCHARGES - the shot still kills. (The entry
 * angle is only checked on the first wall; a deeper glancing wall can
 * still deflect the beam. The bot lives with that, like a human would.)
 */
bool CHLBot::CanGaussWallbang( CBasePlayer *enemy, float *pflWallDamage )
{
	*pflWallDamage = 0.0f;

	Vector vecSrc = EyePosition();
	Vector vecTarget = enemy->Center();
	Vector vecDir = (vecTarget - vecSrc).Normalize();

	// entry point, from our side (world geometry only - the beam shrugs
	// off entities like players in between)
	TraceResult tr;
	UTIL_TraceLine( vecSrc, vecTarget, ignore_monsters, edict(), &tr );

	if (tr.flFraction >= 1.0f)
		return false;		// nothing in the way - that's a normal shot, not a wallbang

	if (tr.fStartSolid)
		return false;

	// too flat an angle - the beam would reflect off, not punch through
	if (-DotProduct( tr.vecPlaneNormal, vecDir ) < 0.5f)
		return false;

	// exit point, from the enemy's side
	TraceResult trBack;
	UTIL_TraceLine( vecTarget, vecSrc, ignore_monsters, enemy->edict(), &trBack );

	if (trBack.fStartSolid)
		return false;		// enemy is buried in geometry? nothing sane to compute

	// the exit must lie beyond the entry along the beam, or the traces
	// disagree about the world (thin brushes, weird geometry) - bail
	Vector span = trBack.vecEndPos - tr.vecEndPos;
	if (DotProduct( span, vecDir ) <= 0.0f)
		return false;

	float flThickness = span.Length();
	*pflWallDamage = (flThickness < 1.0f) ? 1.0f : flThickness;

	return true;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * No enemy in sight: if we carry a usable gauss and the nearest enemy is
 * close behind punchable geometry, stand our ground and charge a shot
 * through the wall at them. Returns true while engaged (the caller should
 * skip navigation - we're busy aiming through a wall).
 *
 * The attempt re-validates every think, so if the enemy walks somewhere
 * unpunchable mid-charge this returns false, Update() falls back to
 * navigation, the button drops, and the half-charge discharges - same
 * flinch a human wallbanger makes when the target moves.
 */
bool CHLBot::UpdateGaussWallbang( bool action = false )
{
	const float wallbangMaxRange = 1200.0f;		// don't snipe through walls across the map
	const float minDeliveredDamage = 30.0f;		// full charge must land at least this much to bother

	const float gaussMaxChargeDamage = 200.0f;

	// need a gauss with ammo
	CBasePlayerWeapon *gauss = GetOwnedWeapon( WEAPON_GAUSS );
	if (gauss == NULL || !IsWeaponUsable( gauss ))
		return false;

	CBasePlayer *enemy = FindNearestEnemy();
	if (enemy == NULL)
		return false;

	if ((enemy->pev->origin - pev->origin).IsLengthGreaterThan( wallbangMaxRange ))
		return false;

	float flWallDamage;
	if (!CanGaussWallbang( enemy, &flWallDamage ))
		return false;

	// is the wall thin enough that a full charge still hurts on the far side?
	if (gaussMaxChargeDamage - flWallDamage < minDeliveredDamage)
		return false;

	// engage: gauss in hand, aim through the wall at them, size the charge
	if (m_pActiveItem != gauss)
	{
		if (action)
		{
			SwitchWeapon(gauss);
		}
		return true;	// deploying - keep holding position
	}

	if (action)
	{
		SetAimAt(enemy->Center());
		GaussAttack(enemy, gauss, flWallDamage);
		// the action bool is here so that we can use this even if we just want to check
	}
	
	return true;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return our copy of the given weapon (by WEAPON_* id), or NULL.
 */
CBasePlayerWeapon *CHLBot::GetOwnedWeapon( int iId ) const
{
	for( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		for( CBasePlayerItem *item = m_rgpPlayerItems[i]; item; item = item->m_pNext )
		{
			if (item->m_iId != iId)
				continue;

			return (CBasePlayerWeapon *)item->GetWeaponPtr();
		}
	}

	return NULL;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * No enemy in sight - pick a travel goal and follow the nav mesh to it.
 */
void CHLBot::UpdateNavigation( void )
{
	CBaseEntity *goal = NULL;

	if (pev->health <= 50.0)
	{
		goal = FindNearestWantedHealth(false);
	}

	// (goal-NULL checks keep low health as the top priority - the armed
	// branch used to overwrite the health goal)
	if (goal == NULL && IsArmed())
	{
		// armed, but is there a STRONGER weapon worth a short detour?
		// (limited range so bots upgrade opportunistically instead of
		// abandoning the hunt to cross the map for an egon)
		const float upgradeSearchRange = 1500.0f;
		goal = FindNearestWeaponUpgrade( upgradeSearchRange );

		// nothing better nearby - hunt the nearest enemy
		if (goal == NULL)
			goal = FindNearestEnemy();
	}
	else if (goal == NULL)
	{
		// find something to fight with
		goal = FindNearestWantedWeapon();

		// no weapons anywhere - hunt with what we have rather than stand still
		if (goal == NULL)
			goal = FindNearestEnemy();
	}

	if (goal == NULL)
	{
		// alone on the server: top off health/armor/ammo while it's safe,
		// and roam the map once we're stocked - never just stand there
		goal = FindNearestRestock();

		if (goal == NULL)
		{
			UpdateRoam();
			return;
		}
	}

	MoveTowardGoal( goal->pev->origin );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the nearest pickup that tops off something we're missing - health
 * below full, armor below full, or reserve ammo below max for a weapon we
 * actually carry - or NULL when we're fully stocked. This is the "alone
 * time" errand list; urgent low-health seeking lives in UpdateNavigation.
 */
CBaseEntity *CHLBot::FindNearestRestock( void )
{
	// map ground ammo entities to the ammo pool they refill. The bot only
	// wants one if a weapon it CARRIES draws from that pool.
	static const struct
	{
		const char *classname;
		const char *ammoName;
	}
	ammoPickups[] =
	{
		{ "ammo_9mmclip", "9mm" },
		{ "ammo_9mmbox", "9mm" },
		{ "ammo_9mmAR", "9mm" },
		{ "ammo_357", "357" },
		{ "ammo_buckshot", "buckshot" },
		{ "ammo_crossbow", "bolts" },
		{ "ammo_rpgclip", "rockets" },
		{ "ammo_gaussclip", "uranium" },
		{ "ammo_egonclip", "uranium" },
		{ "ammo_ARgrenades", "ARgrenades" },
		{ NULL, NULL }
	};

	CBaseEntity *close = NULL;
	float closeRangeSq = 1.0e12f;

	// health below full (the urgent <=50 case is handled before we run)
	if (pev->health < pev->max_health)
	{
		CBaseEntity *item = NULL;
		while( (item = UTIL_FindEntityByClassname( item, "item_healthkit" )) != NULL )
		{
			if (item->pev->effects & EF_NODRAW)
				continue;

			Vector to = item->pev->origin - pev->origin;
			float rangeSq = DotProduct( to, to );
			if (rangeSq < closeRangeSq)
			{
				close = item;
				closeRangeSq = rangeSq;
			}
		}
	}

	// armor below full
	if (pev->armorvalue < MAX_NORMAL_BATTERY)
	{
		CBaseEntity *item = NULL;
		while( (item = UTIL_FindEntityByClassname( item, "item_battery" )) != NULL )
		{
			if (item->pev->effects & EF_NODRAW)
				continue;

			Vector to = item->pev->origin - pev->origin;
			float rangeSq = DotProduct( to, to );
			if (rangeSq < closeRangeSq)
			{
				close = item;
				closeRangeSq = rangeSq;
			}
		}
	}

	// reserve ammo below max, for pools a carried weapon draws from
	for( int i = 0; ammoPickups[i].classname; ++i )
	{
		int ammoIndex = GetAmmoIndex( ammoPickups[i].ammoName );
		if (ammoIndex < 0)
			continue;

		// find a carried weapon feeding from this pool; its ItemInfo
		// carries the pool's max
		int maxCarry = -1;
		for( int slot = 0; slot < MAX_ITEM_TYPES && maxCarry < 0; ++slot )
		{
			for( CBasePlayerItem *owned = m_rgpPlayerItems[slot]; owned; owned = owned->m_pNext )
			{
				CBasePlayerWeapon *gun = (CBasePlayerWeapon *)owned->GetWeaponPtr();
				if (gun && gun->m_iPrimaryAmmoType == ammoIndex)
				{
					maxCarry = owned->iMaxAmmo1();
					break;
				}
			}
		}

		if (maxCarry < 0 || m_rgAmmo[ammoIndex] >= maxCarry)
			continue;		// no carried gun uses it, or already full

		CBaseEntity *item = NULL;
		while( (item = UTIL_FindEntityByClassname( item, ammoPickups[i].classname )) != NULL )
		{
			if (item->pev->effects & EF_NODRAW)
				continue;

			Vector to = item->pev->origin - pev->origin;
			float rangeSq = DotProduct( to, to );
			if (rangeSq < closeRangeSq)
			{
				close = item;
				closeRangeSq = rangeSq;
			}
		}
	}

	return close;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Nothing to fight, grab, or top off: wander the map like a player looking
 * for action. Picks a random nav area (avoiding the last few we roamed to,
 * so we sweep the map instead of pacing one hallway), paths to it, and
 * picks a new one on arrival or after a timeout.
 */
void CHLBot::UpdateRoam( void )
{
	const float roamArriveRange = 64.0f;	///< close enough - pick somewhere new
	const float maxRoamTime = 20.0f;		///< give up on unreachable roam spots

	bool pickNew = !m_roamTimer.HasStarted() || m_roamTimer.IsElapsed();

	if (!pickNew)
	{
		Vector2D to( m_roamGoal.x - pev->origin.x, m_roamGoal.y - pev->origin.y );
		if (to.IsLengthLessThan( roamArriveRange ))
			pickNew = true;
	}

	if (pickNew)
	{
		if (TheNavAreaList.empty())
			return;		// no nav mesh - nowhere to roam

		// random area, re-rolled a few times if we've been there recently
		CNavArea *area = NULL;
		for( int attempt = 0; attempt < 5; ++attempt )
		{
			int skip = RANDOM_LONG( 0, (int)TheNavAreaList.size() - 1 );
			NavAreaList::iterator it = TheNavAreaList.begin();
			while( skip-- > 0 )
				++it;
			area = *it;

			bool recent = false;
			for( int r = 0; r < ROAM_MEMORY; ++r )
			{
				if (m_recentRoamAreas[r] == area->GetID())
				{
					recent = true;
					break;
				}
			}

			if (!recent)
				break;
		}

		if (area == NULL)
			return;

		// remember it so we spread out instead of pacing the same hallway
		m_recentRoamAreas[ m_recentRoamHead ] = area->GetID();
		m_recentRoamHead = (m_recentRoamHead + 1) % ROAM_MEMORY;

		m_roamGoal = *area->GetCenter();
		m_roamTimer.Start( maxRoamTime );
	}

	MoveTowardGoal( m_roamGoal );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if the given entity is an enemy player.
 * In teamplay, teammates are friends; in deathmatch everyone is an enemy.
 */
bool CHLBot::IsEnemy( CBaseEntity *ent ) const
{
	if (ent == NULL || !ent->IsPlayer())
		return false;

	if (ent == static_cast<const CBaseEntity *>( this ))
		return false;

	if (!ent->IsAlive())
		return false;

	if (g_pGameRules && g_pGameRules->PlayerRelationship( const_cast<CHLBot *>( this ), ent ) == GR_TEAMMATE)
		return false;

	return true;
}

float CHLBot::TargetCost( CBasePlayer *player, CBasePlayer *currentEnemy ) const
{
	float range = (player->pev->origin - pev->origin).Length();

	// invert distance so closer targets cost more; +1 avoids dividing by zero
	float cost = 1.0f / (range + 1.0f);

	if (player == currentEnemy)
		cost *= 2.0;

	return cost;
}

CBasePlayer *CHLBot::SelectVisibleEnemy( void )
{
	// resolve our remembered enemy safely: the EHANDLE returns NULL if they
	// disconnected, and we drop them here if they died - a dead "current
	// enemy" must not soak up the target-stickiness bonus
	CBasePlayer *currentEnemy = static_cast<CBasePlayer *>( static_cast<CBaseEntity *>( m_hEnemy ) );
	if (currentEnemy && (!currentEnemy->IsPlayer() || !currentEnemy->IsAlive()))
		currentEnemy = NULL;

	CBasePlayer *best = NULL;
	float bestCost = 0.0f;

	for( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBasePlayer *player = static_cast<CBasePlayer *>( UTIL_PlayerByIndex( i ) );

		if (!IsEntityValid( player ))
			continue;

		if (!IsEnemy( player ))
			continue;

		float cost = TargetCost( player, currentEnemy );
		if (cost <= bestCost)
			continue;

		// visibility check last - it traces, which is the expensive part
		if (!FVisible( player ))
			continue;

		best = player;
		bestCost = cost;
	}

	// remember the winner (or forget a target we can no longer see)
	m_hEnemy = best;

	return best;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the nearest living enemy, visible or not.
 */
CBasePlayer *CHLBot::FindNearestEnemy( void )
{
	CBasePlayer *close = NULL;
	float closeRangeSq = 1.0e12f;

	for( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBasePlayer *player = static_cast<CBasePlayer *>( UTIL_PlayerByIndex( i ) );

		if (!IsEntityValid( player ))
			continue;

		if (!IsEnemy( player ))
			continue;

		Vector to = player->pev->origin - pev->origin;
		float rangeSq = DotProduct( to, to );
		if (rangeSq < closeRangeSq)
		{
			close = player;
			closeRangeSq = rangeSq;
		}
	}

	return close;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if the weapon has any ammo left to fire.
 */
bool CHLBot::IsWeaponUsable( CBasePlayerWeapon *gun ) const
{
	if (gun == NULL)
		return false;

	if (gun->m_iClip > 0)
		return true;

	int ammoIndex = gun->PrimaryAmmoIndex();
	if (ammoIndex >= 0 && m_rgAmmo[ ammoIndex ] > 0)
		return true;

	return false;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return our best usable non-crowbar weapon, or NULL if we have none.
 * "Best" is the standard HL auto-select weight. Weapons with negative
 * weight (satchel, tripmine) are never auto-selected, matching HL itself.
 * Weapons without ammo are treated as if we didn't have them at all.
 */
CBasePlayerWeapon *CHLBot::GetBestRangedWeapon(bool ignoreGlock) const
{
	CBasePlayerWeapon *best = NULL;

	for( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		for( CBasePlayerItem *item = m_rgpPlayerItems[i]; item; item = item->m_pNext )
		{
			CBasePlayerWeapon *gun = (CBasePlayerWeapon *)item->GetWeaponPtr();

			if (gun == NULL)
				continue;

			if (gun->m_iId == WEAPON_CROWBAR || (( gun->m_iId == WEAPON_GLOCK ) && ignoreGlock))
				continue;

			if (gun->iWeight() <= 0)
				continue;

			if (!IsWeaponUsable( gun ))
				continue;

			if (best == NULL || gun->iWeight() > best->iWeight())
				best = gun;
		}
	}

	return best;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if we own the weapon this map pickup would give AND we have
 * ammo for it. Owning it with no ammo counts as not having it - picking up
 * a duplicate weapon gives us its ammo.
 */
bool CHLBot::HasUsableWeapon( CBaseEntity *item ) const
{
	CBasePlayerItem *pickup = (CBasePlayerItem *)item;

	// don't own one at all?
	if (!(pev->weapons & (1 << pickup->m_iId)))
		return false;

	// find our copy and check its ammo
	for( int i = 0; i < MAX_ITEM_TYPES; ++i )
	{
		for( CBasePlayerItem *owned = m_rgpPlayerItems[i]; owned; owned = owned->m_pNext )
		{
			if (owned->m_iId != pickup->m_iId)
				continue;

			CBasePlayerWeapon *gun = (CBasePlayerWeapon *)owned->GetWeaponPtr();

			if (gun == NULL)
				return true;		// non-weapon item; owning it is enough

			if (gun->m_iId == WEAPON_CROWBAR)
				return true;		// the crowbar needs no ammo

			return IsWeaponUsable( gun );
		}
	}

	return false;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the nearest weapon pickup on the map that we don't have (or have
 * no ammo for), or NULL if there are none. Dropped weapon boxes count.
 */
CBaseEntity *CHLBot::FindNearestWantedWeapon( void )
{
	CBaseEntity *close = NULL;
	float closeRangeSq = 1.0e12f;

	for( int i = 0; wantedWeaponClassnames[i]; ++i )
	{
		CBaseEntity *item = NULL;
		while( (item = UTIL_FindEntityByClassname( item, wantedWeaponClassnames[i] )) != NULL )
		{
			// skip weapons that are carried, respawning, or otherwise not on the ground
			if (item->pev->effects & EF_NODRAW)
				continue;

			if (!FNullEnt( item->pev->owner ))
				continue;

			if (HasUsableWeapon( item ))
				continue;

			Vector to = item->pev->origin - pev->origin;
			float rangeSq = DotProduct( to, to );
			if (rangeSq < closeRangeSq)
			{
				close = item;
				closeRangeSq = rangeSq;
			}
		}
	}

	// dropped weapon boxes hold a dead player's arsenal - always worth a look
	CBaseEntity *box = NULL;
	while( (box = UTIL_FindEntityByClassname( box, "weaponbox" )) != NULL )
	{
		if (box->pev->effects & EF_NODRAW)
			continue;

		Vector to = box->pev->origin - pev->origin;
		float rangeSq = DotProduct( to, to );
		if (rangeSq < closeRangeSq)
		{
			close = box;
			closeRangeSq = rangeSq;
		}
	}

	return close;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the nearest weapon pickup that is a genuine UPGRADE - its
 * auto-select weight beats the best weapon we're carrying - within
 * maxRange, or NULL if there is none. This is what lets an armed bot keep
 * improving its arsenal instead of settling for the first gun it grabbed.
 *
 * Weapon boxes are skipped: their contents are unknown, so they can't be
 * judged as an upgrade (FindNearestWantedWeapon still visits them while
 * the bot is unarmed and anything beats nothing).
 */
CBaseEntity *CHLBot::FindNearestWeaponUpgrade( float maxRange )
{
	// the bar to clear: the weight of the best gun we can actually fire
	CBasePlayerWeapon *bestOwned = GetBestRangedWeapon();
	int iOwnedWeight = bestOwned ? bestOwned->iWeight() : 0;
	
	CBaseEntity *close = NULL;
	float closeRangeSq = maxRange * maxRange;

	for( int i = 0; wantedWeaponClassnames[i]; ++i )
	{
		CBaseEntity *item = NULL;
		while( (item = UTIL_FindEntityByClassname( item, wantedWeaponClassnames[i] )) != NULL )
		{
			// skip weapons that are carried, respawning, or otherwise not on the ground
			if (item->pev->effects & EF_NODRAW)
				continue;

			if (!FNullEnt( item->pev->owner ))
				continue;

			// not stronger than what we already carry? not an upgrade
			// (iWeight() reads the ItemInfo table, valid once weapons precache)
			if (((CBasePlayerItem *)item)->iWeight() <= iOwnedWeight)
				continue;

			Vector to = item->pev->origin - pev->origin;
			float rangeSq = DotProduct( to, to );
			if (rangeSq < closeRangeSq)
			{
				close = item;
				closeRangeSq = rangeSq;
			}
		}
	}

	return close;
}

//--------------------------------------------------------------------------------------------------------------

CBaseEntity* CHLBot::FindNearestWantedHealth(bool visible)
{
	CBaseEntity* close = NULL;
	float closeRangeSq = 1.0e12f;

	CBaseEntity* box = NULL;
	while ((box = UTIL_FindEntityByClassname(box, "item_healthkit")) != NULL)
	{
		if (box->pev->effects & EF_NODRAW)
			continue;

		if (visible)
		{
			if (!FVisible(box))
				continue;
		}

		Vector to = box->pev->origin - pev->origin;
		float rangeSq = DotProduct(to, to);
		if (rangeSq < closeRangeSq)
		{
			close = box;
			closeRangeSq = rangeSq;
		}
	}

	return close;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Recompute the path if needed, then follow it toward the given goal.
 */
void CHLBot::MoveTowardGoal( const Vector &goal )
{
	const float goalMovedTolerance = 240.0f;	// repath if a mobile goal strays this far from the path end

	bool needPath = !m_path.IsValid();

	if (!needPath && m_repathTimer.IsElapsed())
		needPath = true;

	if (!needPath && (goal - m_pathGoal).IsLengthGreaterThan( goalMovedTolerance ))
		needPath = true;

	if (needPath)
	{
		ShortestPathCost cost;
		Vector start = pev->origin;

		if (m_path.Compute( &start, &goal, cost ))
		{
			m_pathIndex = 1;		// segment 0 is where we are standing
			m_pathGoal = goal;
		}
		else
		{
			m_path.Invalidate();
		}

		// don't repath every think even on failure - it is expensive
		m_repathTimer.Start( RANDOM_FLOAT( 1.5f, 2.5f ) );
	}

	if (m_path.IsValid())
	{
		FollowPath();
	}
	else
	{
		// no nav mesh here (or no route) - head straight toward the goal
		MoveTowardPos( goal );

		// nothing more interesting to look at? face where we're going
		if (!m_lookatViewTarget)
			SetAimAt( goal + Vector( 0, 0, HalfHumanHeight ) );
	}
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Move toward the given world position WITHOUT touching the view - the
 * GoldSrc counterpart of cavortbots' Bot_MoveToPos. The world direction to
 * the goal is converted into view-relative forwardmove/sidemove, so the legs
 * path correctly no matter where AimAt() is pointing the camera.
 *
 * Exact on the ground: PM_WalkMove flattens and re-normalizes the view's
 * forward/right vectors before applying move speeds, so pitch (aiming up or
 * down at an enemy) doesn't bleed speed.
 *
 * Method (mirrors cavortbots' TF2_MoveTo): flip the offset's Y, rotate it by
 * the view yaw, and the components fall out as (forwardmove, sidemove)
 * directly - the Y flip is what maps the world's counter-clockwise yaw onto
 * the engine's positive-right sidemove. Normalized so distance to the goal
 * doesn't scale the speed.
 */
void CHLBot::MoveTowardPos( const Vector &pos )
{
	// world-space offset to the goal (TF2_MoveTo style: rotate the offset
	// into view-local space instead of going through angles)
	float dx = pos.x - pev->origin.x;
	float dy = pos.y - pev->origin.y;

	if (dx * dx + dy * dy < 1.0f)
		return;		// standing on it - nothing to do

	// flip Y so the rotation below lands in the engine's move space
	// (world yaw turns counter-clockwise, sidemove is positive-right)
	dy = -dy;

	float flRad = pev->v_angle.y * (float)(M_PI / 180.0);
	float s = sin( flRad );
	float c = cos( flRad );

	// rotate: x = along the view (forwardmove), y = across it (sidemove)
	float flForward = c * dx - s * dy;
	float flSide = s * dx + c * dy;

	// normalize so distance to the goal doesn't scale the speed
	float flLen = sqrt( flForward * flForward + flSide * flSide );
	if (flLen < 0.001f)
		return;

	flForward /= flLen;
	flSide /= flLen;

	// drive the movement directly through the speeds RunPlayerMove sends -
	// GoldSrc movement runs on forwardmove/sidemove, so no move buttons are
	// needed (buttons stay reserved for jump/duck/attack intent)
	float flSpeed = GetMoveSpeed();
	m_forwardSpeed = flForward * flSpeed;
	m_strafeSpeed = flSide * flSpeed;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true while the engine's ladder movement owns us - PM_LadderMove
 * flips a player to MOVETYPE_FLY for as long as they cling to a ladder.
 */
bool CHLBot::IsOnLadder( void ) const
{
	return pev->movetype == MOVETYPE_FLY;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Climb the ladder we are on, toward the given goal (above us = up, below
 * us = down), WITHOUT turning the camera's yaw.
 *
 * How ladder physics work (PM_LadderMove, pm_shared.c): movement on a
 * ladder is BUTTON-driven - forwardmove/sidemove values are ignored. The
 * held keys build a view-relative vector (so pitch matters!), and the
 * component pushing INTO the ladder plane is converted into motion up the
 * ladder; the in-plane remainder becomes drift. That means any key can
 * climb given the right pitch - facing away from the ladder, S + looking
 * down climbs UP - which is exactly what lets us keep the yaw untouched.
 *
 * Rather than special-casing facings, we run the engine's own math for
 * each candidate key combo x view pitch and hold whichever climbs fastest
 * toward the goal with the least sideways drift:
 *
 *   velocity = forward(pitch,yaw)*F + right(yaw)*R      (F,R = +-200 per key)
 *   normal   = dot( velocity, ladderNormal )
 *   climb    = lateral - (n x perp)*normal              (the engine's line)
 *
 * The two styles (BOT_LADDER_FAST_CLIMB) only differ in the candidate set:
 * single fore/back key, or fore/back + strafe held together - the engine
 * sums both 200 u/s key vectors without normalizing, so diagonals climb
 * ~1.4x faster.
 */
void CHLBot::LadderClimb( const Vector &goal )
{
	const float climbKeySpeed = 200.0f;		// MAX_CLIMB_SPEED (pm_shared.c)
	const float driftPenalty = 0.5f;		// how much sideways slide costs in the scoring

	// --- find the ladder plane normal -------------------------------------
	// The func_ladder brush itself is non-solid, so traces pass through it;
	// probe for the solid wall it hugs - its plane is parallel to the ladder.
	Vector vecNormal( 0, 0, 0 );
	float flBestFrac = 1.0f;

	for( int i = 0; i < 8; ++i )
	{
		float r = (float)(i * M_PI / 4.0);
		Vector probe( cos( r ) * 48.0f, sin( r ) * 48.0f, 0 );

		TraceResult tr;
		UTIL_TraceLine( pev->origin, pev->origin + probe, ignore_monsters, edict(), &tr );

		if (tr.flFraction < flBestFrac)
		{
			flBestFrac = tr.flFraction;
			vecNormal = tr.vecPlaneNormal;
		}
	}

	if (flBestFrac >= 1.0f)
	{
		// freestanding ladder with no wall behind it - assume it faces us
		// along our yaw (we walked into it to get on)
		float r = pev->v_angle.y * (float)(M_PI / 180.0);
		vecNormal = Vector( -cos( r ), -sin( r ), 0 );
	}

	// --- dismount: the goal is level with us, so we want OFF, not along ---
	// Pushing AWAY from the ladder face while standing at its base makes
	// the engine shove us off it (the onFloor && normal>0 branch of
	// PM_LadderMove). Pick whichever fore/back key pushes away at our yaw.
	float dz = goal.z - pev->origin.z;
	if (fabs( dz ) < JumpCrouchHeight)
	{
		float r = pev->v_angle.y * (float)(M_PI / 180.0);
		float facingAway = cos( r ) * vecNormal.x + sin( r ) * vecNormal.y;

		SetBits( m_buttonFlags, (facingAway > 0.0f) ? IN_FORWARD : IN_BACK );
		ClearBits( m_buttonFlags, IN_JUMP );
		m_forwardSpeed = 0.0f;
		m_strafeSpeed = 0.0f;

		pev->v_angle.x = 0.0f;		// level out - we're stepping off, not climbing
		pev->v_angle.z = 0;
		m_lookatViewTarget = false;
		return;
	}

	// which way along the ladder the path wants us
	float desiredDir = (dz > 0.0f) ? 1.0f : -1.0f;

	// "up the ladder" frame, exactly as the engine builds it
	Vector perp = CrossProduct( Vector( 0, 0, 1 ), vecNormal );
	if (perp.NormalizeInPlace() < 0.001f)
		return;		// degenerate (horizontal "ladder") - nothing sane to solve
	Vector alongLadder = CrossProduct( vecNormal, perp );

	// --- candidate key combos, by style ------------------------------------
#if BOT_LADDER_FAST_CLIMB
	static const int ladderCombos[] =
	{
		IN_FORWARD | IN_MOVELEFT,
		IN_FORWARD | IN_MOVERIGHT,
		IN_BACK    | IN_MOVELEFT,
		IN_BACK    | IN_MOVERIGHT,
	};
#else
	static const int ladderCombos[] =
	{
		IN_FORWARD,
		IN_BACK,
	};
#endif
	const int comboCount = (int)(sizeof( ladderCombos ) / sizeof( ladderCombos[0] ));

	// --- solve: engine math for every combo x pitch, keep the best ---------
	float flYawRad = pev->v_angle.y * (float)(M_PI / 180.0);
	float sy = sin( flYawRad ), cy = cos( flYawRad );

	int iBestCombo = IN_FORWARD;
	float flBestPitch = -60.0f * desiredDir;
	float flBestScore = -1.0e9f;

	for( int c = 0; c < comboCount; ++c )
	{
		float F = 0.0f, R = 0.0f;
		if (ladderCombos[c] & IN_FORWARD)   F += climbKeySpeed;
		if (ladderCombos[c] & IN_BACK)      F -= climbKeySpeed;
		if (ladderCombos[c] & IN_MOVERIGHT) R += climbKeySpeed;
		if (ladderCombos[c] & IN_MOVELEFT)  R -= climbKeySpeed;

		for( float flPitch = -85.0f; flPitch <= 85.0f; flPitch += 10.0f )
		{
			// AngleVectors with roll = 0 (pm_math.c conventions)
			float sp = sin( flPitch * (float)(M_PI / 180.0) );
			float cp = cos( flPitch * (float)(M_PI / 180.0) );

			Vector vpn( cp * cy, cp * sy, -sp );	// forward
			Vector vright( sy, -cy, 0 );			// right

			Vector velocity = vpn * F + vright * R;

			// the engine's decomposition (PM_LadderMove)
			float flNormal = DotProduct( velocity, vecNormal );
			Vector lateral = velocity - vecNormal * flNormal;
			Vector result = lateral - alongLadder * flNormal;

			float flClimb = result.z * desiredDir;
			float flDrift = sqrt( result.x * result.x + result.y * result.y );

			// slight preference for pushing INTO the ladder: pushing away
			// while standing at its base shoves us off it (onFloor case)
			float flScore = flClimb - driftPenalty * flDrift - ((flNormal > 0.0f) ? 25.0f : 0.0f);

			if (flScore > flBestScore)
			{
				flBestScore = flScore;
				iBestCombo = ladderCombos[c];
				flBestPitch = flPitch;
			}
		}
	}

	// --- apply: buttons + pitch; yaw untouched ------------------------------
	SetBits( m_buttonFlags, iBestCombo );
	ClearBits( m_buttonFlags, IN_JUMP );	// jump detaches from the ladder

	// speeds are ignored on ladders; zero them so nothing fights the buttons
	m_forwardSpeed = 0.0f;
	m_strafeSpeed = 0.0f;

	// the climb owns the view pitch this tick (yaw stays where it was);
	// release any aim claim so Upkeep()'s AimAt doesn't wrench it back
	pev->v_angle.x = flBestPitch;
	pev->v_angle.z = 0;
	m_lookatViewTarget = false;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Compute a look-at point a fixed distance AHEAD along our path (based on
 * the lookahead helper TF bots use). Looking at the next waypoint makes a
 * bot stare at the floor two steps in front of it and snap its gaze at
 * every corner; looking N units down the path reads like a player watching
 * where they're going - corners get "seen" early and the gaze sweeps
 * through them smoothly.
 *
 * Walks the remaining segments, spending flLookAheadRange of distance; the
 * point is interpolated on the segment where the budget runs out, then
 * lifted to eye level so the gaze isn't pinned to the ground. If the path
 * is shorter than the budget, the farthest point (path end) is returned.
 *
 * Returns false if there is no valid path to look along.
 */
bool CHLBot::GetLookaheadPos( float flLookAheadRange, Vector *pvBuffer )
{
	if (!m_path.IsValid() || m_pathIndex >= m_path.GetSegmentCount())
		return false;

	float flRemaining = flLookAheadRange;
	Vector segStart = pev->origin;

	// fallback if we run out of path: the farthest point reached
	*pvBuffer = m_path.GetEndpoint();

	for( int i = m_pathIndex; i < m_path.GetSegmentCount(); ++i )
	{
		Vector segEnd = m_path[i]->pos;

		Vector segSub = segEnd - segStart;
		float flSegLen = segSub.Length();
		if (flSegLen <= 0.0f)
			continue;

		if (flRemaining <= flSegLen)
		{
			// lookahead point lands on this segment - interpolate along it
			float flFrac = flRemaining / flSegLen;
			*pvBuffer = segStart + segSub * flFrac;

			// lift it to eye level so the gaze isn't constantly at the ground
			pvBuffer->z += pev->view_ofs.z;
			return true;
		}

		flRemaining -= flSegLen;
		segStart = segEnd;
		*pvBuffer = segEnd;
	}

	// path ran out inside the budget - look at its end, at eye level
	pvBuffer->z += pev->view_ofs.z;
	return true;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Steer along m_path: walk toward the current waypoint (view-independent -
 * see MoveTowardPos()), jump for ledges and marked jump areas, and crouch
 * through crouch areas. The view is only claimed as a fallback when nothing
 * else (combat, health) wants it this think.
 */
void CHLBot::FollowPath( void )
{
	const CNavPath::PathSegment *segment = m_path[ m_pathIndex ];

	// walked off the end of the path?
	if (segment == NULL)
	{
		m_path.Invalidate();
		return;
	}

	// --- ladder segments: mount, climb, dismount ---------------------------
	// A GO_LADDER_UP/DOWN segment's pos is the mount spot the path computed
	// (ladder base/top pushed out along its facing); its area is where the
	// ladder DELIVERS us. The generic waypoint-touch advance below would
	// "complete" the segment while still standing at the bottom, so ladder
	// segments track completion by HEIGHT instead.
	if (segment->ladder)
	{
		const CNavLadder *ladder = segment->ladder;

		bool finished;
		if (segment->how == GO_LADDER_UP)
			finished = (pev->origin.z >= ladder->m_top.z - StepHeight);
		else
			finished = (pev->origin.z <= ladder->m_bottom.z + JumpCrouchHeight);

		if (finished)
		{
			++m_pathIndex;
			if (m_pathIndex >= m_path.GetSegmentCount())
				m_path.Invalidate();
			return;
		}

		if (IsOnLadder())
		{
			// climb toward the delivering end
			if (segment->how == GO_LADDER_UP)
				LadderClimb( ladder->m_top + Vector( 0, 0, HalfHumanHeight ) );
			else
				LadderClimb( ladder->m_bottom );
		}
		else
		{
			// approach: walk to the computed mount spot first, then push
			// straight into the ladder face - touching it latches us on
			Vector2D toMount( segment->pos.x - pev->origin.x, segment->pos.y - pev->origin.y );
			if (toMount.IsLengthGreaterThan( WaypointTouchRange ))
			{
				MoveTowardPos( segment->pos );
			}
			else
			{
				const Vector &face = (segment->how == GO_LADDER_UP) ? ladder->m_bottom : ladder->m_top;
				MoveTowardPos( Vector( face.x, face.y, pev->origin.z ) );
			}

			if (!m_lookatViewTarget)
				SetAimAt( segment->pos + Vector( 0, 0, HalfHumanHeight ) );
		}

		return;
	}

	// close enough to this waypoint? move on to the next one
	Vector toWaypoint2D( segment->pos.x - pev->origin.x, segment->pos.y - pev->origin.y, 0 );
	if (toWaypoint2D.IsLengthLessThan( WaypointTouchRange ) && fabs( segment->pos.z - pev->origin.z ) < JumpCrouchHeight)
	{
		++m_pathIndex;
		if (m_pathIndex >= m_path.GetSegmentCount())
		{
			m_path.Invalidate();
			return;
		}
		segment = m_path[ m_pathIndex ];
	}

	// on a ladder? climbing is its own movement mode - buttons + pitch,
	// solved against the engine's ladder math (see LadderClimb())
	if (IsOnLadder())
	{
		LadderClimb( segment->pos );
		return;
	}

	// move toward the waypoint relative to wherever the view is pointing -
	// the camera is free for combat/health aim set earlier in Update()
	MoveTowardPos( segment->pos );

	// nothing claimed the view this think? look AHEAD along the path
	// (not at the next waypoint - see GetLookaheadPos()) so the gaze
	// sweeps corners naturally like a player watching where they're going
	if (!m_lookatViewTarget)
	{
		const float lookAheadRange = 150.0f;

		Vector lookPos;
		if (GetLookaheadPos(lookAheadRange, &lookPos))
			SetAimAt(lookPos);
		else
			SetAimAt( segment->pos + Vector( 0, 0, HalfHumanHeight ) );
	}

	// UTIL_DrawBeamPoints( pev->origin, segment->pos + Vector( 0, 0, HalfHumanHeight ), 1, 0, 255, 0 );

	// jump up ledges, and wherever the mesh says a jump is required
	if (pev->flags & FL_ONGROUND)
	{
		bool needJump = (segment->pos.z - pev->origin.z) > StepHeight;

		if (segment->area && (segment->area->GetAttributes() & NAV_JUMP))
			needJump = true;

		if (needJump)
			PressJump();
	}

	// crouch through low areas
	if (segment->area && (segment->area->GetAttributes() & NAV_CROUCH))
		SetBits( m_buttonFlags, IN_DUCK );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Detect being stuck. Jump to clear small obstacles; if that doesn't help,
 * throw the path away and recompute.
 */
void CHLBot::UpdateStuckMonitor( void )
{
	const float movedTolerance = 30.0f;
	const float jumpAfter = 1.0f;
	const float repathAfter = 3.0f;

	if ((pev->origin - m_stuckSpot).IsLengthGreaterThan( movedTolerance ))
	{
		// we are moving fine
		m_stuckSpot = pev->origin;
		m_stuckTimer.Start();
		return;
	}

	if (m_stuckTimer.IsGreaterThen( repathAfter ))
	{
		m_path.Invalidate();
		m_repathTimer.Invalidate();
		m_stuckSpot = pev->origin;
		m_stuckTimer.Start();
	}
	else if (m_stuckTimer.IsGreaterThen( jumpAfter ))
	{
		// never jump to unstick on a ladder - IN_JUMP detaches us from it
		if (!IsOnLadder())
			PressJump();
	}
}

//--------------------------------------------------------------------------------------------------------------
// Command interface (CZ CBot pattern)
//--------------------------------------------------------------------------------------------------------------

void CHLBot::ResetCommand( void )
{
	m_forwardSpeed = 0.0f;
	m_strafeSpeed = 0.0f;
	m_verticalSpeed = 0.0f;
	m_buttonFlags = 0;
}

//--------------------------------------------------------------------------------------------------------------
float CHLBot::GetMoveSpeed( void ) const
{
	return (pev->maxspeed > 0.0f) ? pev->maxspeed : BotMoveSpeed;
}

//--------------------------------------------------------------------------------------------------------------
void CHLBot::MoveForward( void )
{
	m_forwardSpeed = GetMoveSpeed();
	SetBits( m_buttonFlags, IN_FORWARD );
	ClearBits( m_buttonFlags, IN_BACK );
}

//--------------------------------------------------------------------------------------------------------------
void CHLBot::StrafeLeft( void )
{
	m_strafeSpeed = -GetMoveSpeed();
	SetBits( m_buttonFlags, IN_MOVELEFT );
	ClearBits( m_buttonFlags, IN_MOVERIGHT );
}

//--------------------------------------------------------------------------------------------------------------
void CHLBot::StrafeRight( void )
{
	m_strafeSpeed = GetMoveSpeed();
	SetBits( m_buttonFlags, IN_MOVERIGHT );
	ClearBits( m_buttonFlags, IN_MOVELEFT );
}

//--------------------------------------------------------------------------------------------------------------
bool CHLBot::PressJump( void )
{
	// don't spam jumps - it kills our ground speed
	// commented so bots can bhop
	// const float minJumpInterval = 0.9f;
	// if (gpGlobals->time - m_jumpTimestamp < minJumpInterval)
		// return false;

	SetBits( m_buttonFlags, IN_JUMP );
	m_jumpTimestamp = gpGlobals->time;
	return true;
}

//--------------------------------------------------------------------------------------------------------------
void CHLBot::PressPrimaryAttack( void )
{
	SetBits( m_buttonFlags, IN_ATTACK );
}

//--------------------------------------------------------------------------------------------------------------
void CHLBot::PressSecondaryAttack( void )
{
	SetBits( m_buttonFlags, IN_ATTACK2 );
}

void CHLBot::SetAimAt(const Vector& spot)
{
	m_lookatViewTarget = true;
	m_viewTarget = spot;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Compute how far to rotate one angle axis this tick: speed proportional
 * to the remaining error (fast when far, easing in when close), hard-capped
 * at maxStep. Within snapTolerance the turn just finishes, so the final
 * micro-correction never crawls.
 */
static float AimStepToward( float diff, float gain, float deltaT, float maxStep, float snapTolerance )
{
	if (fabs( diff ) <= snapTolerance)
		return diff;

	float step = diff * gain * deltaT;

	if (step > maxStep)
		step = maxStep;
	else if (step < -maxStep)
		step = -maxStep;

	return step;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Turn our view angles toward the given world position. This is a smooth,
 * rate-limited turn, not a snap - invoke it every think (see Upkeep()) and
 * the aim converges on the target over a few ticks.
 */
void CHLBot::AimAt( const Vector &spot )
{
	const float turnGain = 24.0f;		///< per-second fraction of the remaining error to turn (higher = snappier)
	const float maxTurnRate = 540.0f;	///< hard cap, degrees per second
	const float snapTolerance = 1.0f;	///< error (degrees) below which the turn just completes

	// ideal angles toward the target, aimed from the eyes - that is where
	// the weapon traces start
	Vector to = spot - (pev->origin + pev->view_ofs);

	// yaw: same convention in both direction angles and view angles,
	// so the engine helper is safe to use here
	float idealYaw = UTIL_VecToYaw( to );

	// pitch: computed directly - the engine's VecToAngles pitch convention
	// (positive = up) is the opposite of view angles (negative = up)
	float dist2D = sqrt( to.x * to.x + to.y * to.y );
	float idealPitch = -(float)( atan2( to.z, dist2D ) * 180.0 / M_PI );

	// clamp pitch to the range players get
	if (idealPitch > 89.0f)
		idealPitch = 89.0f;
	else if (idealPitch < -89.0f)
		idealPitch = -89.0f;

	// scale this tick's turn by real elapsed time, so a skipped think
	// doesn't slow the turn down
	float deltaT = gpGlobals->time - m_flLastAimTime;
	m_flLastAimTime = gpGlobals->time;

	if (deltaT <= 0.0f || deltaT > 0.25f)
	{
		// first aim after spawning (or a long gap) - snap, there is
		// nothing meaningful to smooth from
		pev->v_angle.x = idealPitch;
		pev->v_angle.y = idealYaw;
		pev->v_angle.z = 0;
		pev->ideal_yaw = idealYaw;
		return;
	}

	float maxStep = maxTurnRate * deltaT;

	// rotate toward the ideal angles along the shortest arc
	float yawDiff = AngleNormalize( idealYaw - pev->v_angle.y );
	float pitchDiff = AngleNormalize( idealPitch - pev->v_angle.x );

	pev->v_angle.y = AngleNormalize( pev->v_angle.y + AimStepToward( yawDiff, turnGain, deltaT, maxStep, snapTolerance ) );
	pev->v_angle.x += AimStepToward( pitchDiff, turnGain, deltaT, maxStep, snapTolerance );
	pev->v_angle.z = 0;

	pev->ideal_yaw = pev->v_angle.y;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Send the accumulated movement command to the engine.
 */
void CHLBot::ExecuteCommand( void )
{
	byte adjustedMSec = ThrottledMsec();

	// player model angles are "munged" from view angles
	pev->angles = pev->v_angle;
	pev->angles.x /= -3.0f;

	m_flPreviousCommandTime = gpGlobals->time;

	(*g_engfuncs.pfnRunPlayerMove)( edict(), pev->v_angle, m_forwardSpeed, m_strafeSpeed, m_verticalSpeed,
									m_buttonFlags, 0, adjustedMSec );
}

//--------------------------------------------------------------------------------------------------------------
byte CHLBot::ThrottledMsec( void ) const
{
	// estimate msec for this command from the time since the previous one
	int newMsec = (int)( (gpGlobals->time - m_flPreviousCommandTime) * 1000.0f );
	if (newMsec > 255)
		newMsec = 255;

	return (byte)newMsec;
}
