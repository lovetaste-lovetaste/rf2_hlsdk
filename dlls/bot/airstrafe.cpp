// airstrafe.cpp
// Camera-angle air-strafing for pathing bots - see airstrafe.h for the
// design documentation and the public API.
//
// Ported from cavortbots' airstrafe.inc (SourceMod/TF2). GoldSrc adaptations:
//   * PM_AirAccelerate math is identical (TF2 inherited it), AIR_CAP included.
//   * SourceMod's arbitrary-size TR_TraceHull becomes UTIL_TraceHull with the
//     engine's fixed hulls: head_hull for the wall probes, the player's own
//     standing/ducked hull for landing prediction. ignore_monsters skips
//     players, matching the original's "world geometry only" filter.
//   * Tick interval is an explicit deltaT parameter (the seconds your bot
//     passes to pfnRunPlayerMove) instead of a global GetTickInterval().
//   * Bot_AirStrafeTo hands back forwardmove/sidemove speeds too, because
//     GoldSrc fakeclients move on explicit speeds, not on buttons alone.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
// IN_* button bits come from common/const.h (pulled in by extdll.h)

#include "airstrafe.h"
#include "bot_util.h"	// UTIL_DrawBeamPoints() for the debug lasers

// GoldSrc maximum clients; per-bot state is indexed by entindex (1-based)
#define AIRSTRAFE_MAX_CLIENTS 32

// Per-bot last strafe side (+1 = was turning left/MOVELEFT, -1 = right). Used
// by the deadband so a near-aligned bot keeps committing to one side for a
// moment instead of jittering. Zero = uninitialised.
static int s_iAirStrafeSide[ AIRSTRAFE_MAX_CLIENTS + 1 ];

// Per-bot key combo the flexible key pick is currently holding, 0 = using the
// preferred strafe keys. Remembered so the keys only change when another
// combo is meaningfully better - human key changes are deliberate.
static int s_iAirStrafeFlexBtn[ AIRSTRAFE_MAX_CLIENTS + 1 ];

// Lazily-cached cvar pointers.
static cvar_t *s_pAirAccelCvar = NULL;
static cvar_t *s_pGravityCvar = NULL;

//------------------------------------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------------------------------------

#define AS_RAD2DEG( r ) ((float)((r) * 180.0 / M_PI))
#define AS_DEG2RAD( d ) ((float)((d) * M_PI / 180.0))

// normalize an angle into [-180, 180] (local copy - keeps this file standalone)
static float AS_AngleNormalize( float flAngle )
{
	flAngle = fmodf( flAngle, 360.0f );
	if (flAngle > 180.0f)
		flAngle -= 360.0f;
	if (flAngle < -180.0f)
		flAngle += 360.0f;
	return flAngle;
}

// step a yaw toward a target yaw by at most flMaxStep degrees, wrapping
// cleanly - this is what makes the view ease instead of snap
static float AS_ApproachYaw( float flCur, float flTarget, float flMaxStep )
{
	float flDiff = AS_AngleNormalize( flTarget - flCur );
	if (flDiff > flMaxStep)
		flDiff = flMaxStep;
	if (flDiff < -flMaxStep)
		flDiff = -flMaxStep;
	return AS_AngleNormalize( flCur + flDiff );
}

// interpolate from angle a to angle b along the shortest arc (t in [0,1])
static float AS_LerpAngle( float a, float b, float t )
{
	return AS_AngleNormalize( a + AS_AngleNormalize( b - a ) * t );
}

// valid, in-range entindex for the per-bot state arrays
static int AS_StateIndex( CBasePlayer *player )
{
	int i = player->entindex();
	return (i >= 1 && i <= AIRSTRAFE_MAX_CLIENTS) ? i : 0;
}

//------------------------------------------------------------------------------------------------------------
float AirStrafe_GetAirAccel( void )
{
	if (s_pAirAccelCvar == NULL)
		s_pAirAccelCvar = CVAR_GET_POINTER( "sv_airaccelerate" );
	return s_pAirAccelCvar ? s_pAirAccelCvar->value : 10.0f;
}

//------------------------------------------------------------------------------------------------------------
/**
 * Effective downward gravity (HU/s^2): sv_gravity scaled by the per-entity
 * pev->gravity multiplier (0 on players means unscaled).
 */
float AirStrafe_GetGravity( CBasePlayer *player )
{
	if (s_pGravityCvar == NULL)
		s_pGravityCvar = CVAR_GET_POINTER( "sv_gravity" );
	float flGrav = s_pGravityCvar ? s_pGravityCvar->value : 800.0f;

	if (player->pev->gravity > 0.0f)
		flGrav *= player->pev->gravity;

	return flGrav;
}

//------------------------------------------------------------------------------------------------------------
bool AirStrafe_IsAirborne( CBasePlayer *player )
{
	if (player->pev->flags & FL_ONGROUND)
		return false;

	if (player->pev->waterlevel > 1)
		return false;		// swimming is its own movement mode

	if (player->pev->movetype == MOVETYPE_FLY)
		return false;		// on a ladder - PM_LadderMove owns the player, not air physics

	return true;
}

//------------------------------------------------------------------------------------------------------------
/**
 * World wishdir angle offset (degrees) that a held movement key combo
 * produces relative to the view yaw - the inverse of the table in the header.
 * The engine's wishdir sits atan2( -side, forward ) off the view.
 * Returns 0 for an empty (or fully cancelled) combo.
 */
float AirStrafe_ButtonWishOffset( int iButtons )
{
	float f = 0.0f, s = 0.0f;
	if (iButtons & IN_FORWARD)   f += 1.0f;
	if (iButtons & IN_BACK)      f -= 1.0f;
	if (iButtons & IN_MOVERIGHT) s += 1.0f;
	if (iButtons & IN_MOVELEFT)  s -= 1.0f;

	if (f == 0.0f && s == 0.0f)
		return 0.0f;

	return AS_RAD2DEG( atan2( -s, f ) );
}

//------------------------------------------------------------------------------------------------------------
void AirStrafe_Reset( CBasePlayer *player )
{
	int i = AS_StateIndex( player );
	s_iAirStrafeSide[i] = 0;
	s_iAirStrafeFlexBtn[i] = 0;
}

//------------------------------------------------------------------------------------------------------------
// Air-accel budget (L)
//------------------------------------------------------------------------------------------------------------
float AirStrafe_MaxAccelPerTick( CBasePlayer *player, float flDeltaT, float *pflWishspeedOut )
{
	float flWishspeed = player->pev->maxspeed;
	if (flWishspeed <= 0.0f)
		flWishspeed = 270.0f;		// sane HLDM fallback

	if (pflWishspeedOut)
		*pflWishspeedOut = flWishspeed;

	return AirStrafe_GetAirAccel() * flWishspeed * flDeltaT * AIRSTRAFE_SURF_FRICTION;
}

//------------------------------------------------------------------------------------------------------------
// Optimal offset angle (theta_opt)
//
// New speed^2 after a tick = v^2 + 2*v*g*cos(t) + g^2, where g = applied
// accel and g = min(L, CAP - v*cos(t)). Splitting on whether the accel is
// budget-capped (g = L) or addspeed-capped (g = CAP - v*cos(t)) and
// maximising gives a peak exactly at cos(t) = max(0, CAP - L) / v.
//------------------------------------------------------------------------------------------------------------
float AirStrafe_OptimalOffsetDeg( float flSpeed, float flL, float flAirCap )
{
	if (flSpeed < 1.0f)
		return 0.0f;

	float flCos = (flAirCap - flL) / flSpeed;	// can be negative when L > airCap
	if (flCos < 0.0f)
		flCos = 0.0f;		// never helps speed to point past 90
	if (flCos > 1.0f)
		flCos = 1.0f;		// very low speed -> just accel forward

	return AS_RAD2DEG( acos( flCos ) );
}

//------------------------------------------------------------------------------------------------------------
// Wall avoidance (sharpen-vs-speed bias)
//------------------------------------------------------------------------------------------------------------
#if AIRSTRAFE_WALL_AVOID

/**
 * Sweep a thin hull from vStart along a world heading (degrees) for flDist.
 * Returns the hit fraction [0,1]; 1.0 = clear. head_hull (16x16x18 half
 * extents) is the closest engine hull to the original's 16x16x32 probe;
 * ignore_monsters keeps this a pure world-geometry probe like the original.
 */
static float AirStrafe_WallProbe( CBasePlayer *player, const Vector &vStart, float flAngDeg, float flDist )
{
	float r = AS_DEG2RAD( flAngDeg );
	Vector vEnd( vStart.x + cos( r ) * flDist, vStart.y + sin( r ) * flDist, vStart.z );

	TraceResult tr;
	UTIL_TraceHull( vStart, vEnd, ignore_monsters, head_hull, player->edict(), &tr );

	return tr.flFraction;
}

/**
 * Probe the velocity heading for a wall the strafe curve is about to clip.
 * Returns a danger bias in [0,1] (0 = clear, 1 = wall right on top of us) and
 * sets *piOpenSide to the side to escape toward (+1 left / -1 right).
 *
 * Cheap on a clear path: a single forward probe; the two side probes only run
 * when that forward probe actually finds a wall.
 */
static float AirStrafe_WallAvoid( CBasePlayer *player, const Vector &vPos, float flVelAng,
	float flSpeed, int iSideDefault, int *piOpenSide )
{
	*piOpenSide = iSideDefault;

	Vector vStart = vPos;
	vStart.z += AIRSTRAFE_WALL_CAST_Z;

	float flLook = flSpeed * AIRSTRAFE_WALL_LOOKAHEAD_TIME;
	if (flLook < AIRSTRAFE_WALL_LOOKAHEAD_MIN)
		flLook = AIRSTRAFE_WALL_LOOKAHEAD_MIN;
	if (flLook > AIRSTRAFE_WALL_LOOKAHEAD_MAX)
		flLook = AIRSTRAFE_WALL_LOOKAHEAD_MAX;

	// forward (velocity) probe - where momentum carries us if we under-turn
	float fracC = AirStrafe_WallProbe( player, vStart, flVelAng, flLook );
	if (fracC >= 1.0f)
		return 0.0f;		// clear ahead -> 1 trace only

	// wall ahead - which side is clearer? escape toward it; if neither is
	// clearly clearer, keep sharpening the turn we were already making
	float fracL = AirStrafe_WallProbe( player, vStart, flVelAng + AIRSTRAFE_WALL_SIDE_ANGLE, flLook );
	float fracR = AirStrafe_WallProbe( player, vStart, flVelAng - AIRSTRAFE_WALL_SIDE_ANGLE, flLook );

	if (fracL - fracR > AIRSTRAFE_WALL_SIDE_HYST)
		*piOpenSide = 1;	// left clearer
	else if (fracR - fracL > AIRSTRAFE_WALL_SIDE_HYST)
		*piOpenSide = -1;	// right clearer

	// closer wall = stronger bias; smoothstep for a graceful ramp-in
	float b = 1.0f - fracC;
	if (b < 0.0f) b = 0.0f;
	if (b > 1.0f) b = 1.0f;
	return b * b * (3.0f - 2.0f * b);
}

#endif // AIRSTRAFE_WALL_AVOID

//------------------------------------------------------------------------------------------------------------
// The controller: velocity + target -> view yaw + strafe key
//------------------------------------------------------------------------------------------------------------
bool AirStrafe_SolveCameraYaw( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	float *pflOutYaw, int *piOutButtons, bool bBlendForward, float flSpeedGoal )
{
	*piOutButtons = 0;

	int iState = AS_StateIndex( player );
	Vector vPos = player->pev->origin;

	// horizontal direction to the target
	Vector2D vToTarget( vTarget.x - vPos.x, vTarget.y - vPos.y );
	if (vToTarget.LengthSquared() < 1.0f)
		return false;		// target is right on us - no heading
	float flTargetAng = AS_RAD2DEG( atan2( vToTarget.y, vToTarget.x ) );

	// horizontal velocity
	Vector2D vVel( player->pev->velocity.x, player->pev->velocity.y );
	float flSpeed = vVel.Length();

	if (flSpeedGoal < 0.0f)
	{
		float flMax = player->pev->maxspeed;
		if (flMax <= 0.0f)
			flMax = 270.0f;
		flSpeedGoal = flMax * AIRSTRAFE_PATH_BIAS_MULT;
	}
	if (flSpeedGoal < 1.0f)
		flSpeedGoal = 1.0f;

	// --- fallback: no usable momentum to carve off of -> run at the target ---
	// (can't make speed out of ~nothing; pick up some run speed on the way)
	if (flSpeed < AIRSTRAFE_MIN_SPEED)
	{
		*pflOutYaw = AS_AngleNormalize( flTargetAng );
		*piOutButtons = IN_FORWARD;
		s_iAirStrafeFlexBtn[iState] = 0;	// fresh start for the key pick too
		return true;
	}

	float flVelAng = AS_RAD2DEG( atan2( vVel.y, vVel.x ) );

	// speed-optimal offset for the current speed and airaccelerate
	float flWishspeed;
	float flL = AirStrafe_MaxAccelPerTick( player, flDeltaT, &flWishspeed );
	float flTheta = AirStrafe_OptimalOffsetDeg( flSpeed, flL );

	// -- SPEED-PRIORITY FADE --------------------------------------------------
	// flBias: 0 = "slow, just build speed" ... 1 = "fast enough, hug the path".
	// Smoothstepped so behaviour fades instead of snapping.
	float flBias = flSpeed / flSpeedGoal;
	if (flBias < 0.0f) flBias = 0.0f;
	if (flBias > 1.0f) flBias = 1.0f;
	flBias = flBias * flBias * (3.0f - 2.0f * flBias);

	// seed the carve direction toward the path the first time we engage
	if (s_iAirStrafeSide[iState] == 0)
		s_iAirStrafeSide[iState] = (AS_AngleNormalize( flTargetAng - flVelAng ) >= 0.0f) ? 1 : -1;

	// "build speed" heading: a point 90 deg to the side we're already turning,
	// so chasing it makes us keep carving the same way = a circle, the fastest
	// way to add speed when slow. Blend toward the real path point by flBias:
	// slow -> near-pure circle; fast -> the path itself. As speed climbs the
	// spiral closes onto the path by itself.
	float flCircleAng = flVelAng + (float)s_iAirStrafeSide[iState] * AIRSTRAFE_CIRCLE_LEAD;
	float flChaseAng = AS_LerpAngle( flCircleAng, flTargetAng, flBias );

	// from here it's a normal strafe solve, just toward flChaseAng (which
	// already bakes in the speed-vs-path trade)
	float flChasePhi = AS_AngleNormalize( flChaseAng - flVelAng );
	float flAbsChase = fabs( flChasePhi );

	// hysteresis near alignment, and near dead-behind too, so a braking bot
	// whose chase sits at ~180 doesn't flip its turn side every tick
	int iSide;
	if (s_iAirStrafeSide[iState] != 0
		&& (flAbsChase < AIRSTRAFE_ALIGN_DEADBAND || flAbsChase > 180.0f - AIRSTRAFE_ALIGN_DEADBAND))
		iSide = s_iAirStrafeSide[iState];
	else
		iSide = (flChasePhi >= 0.0f) ? 1 : -1;
	s_iAirStrafeSide[iState] = iSide;

	// offset magnitude: theta_opt for maximum speed gain, only widened toward
	// the chase heading as we get fast enough to spend speed-gain on turn
	// rate. Allowed past 90, where the wishdir points partly behind the
	// velocity and the bot BRAKES while it turns - the overshoot save.
	float flUse = flTheta;
	if (flAbsChase > flTheta)
		flUse = flTheta + (flAbsChase - flTheta) * flBias;

	// desired world wishdir = velocity heading rotated +-flUse toward the chase
	float flWishAng = flVelAng + (float)iSide * flUse;

#if AIRSTRAFE_WALL_AVOID
	// -- WALL-AVOIDANCE BIAS --------------------------------------------------
	// If the curve is about to clip a wall, blend the wishdir from the speed
	// angle toward a sharp, braking turn on the open side. The closer the
	// wall, the more it wins; clear of the wall it contributes nothing.
	int iOpenSide;
	float flWall = AirStrafe_WallAvoid( player, vPos, flVelAng, flSpeed, iSide, &iOpenSide );
	if (flWall > 0.0f)
	{
		float flWallOffset = flTheta + (AIRSTRAFE_WALL_MAX_OFFSET - flTheta) * flWall;
		float flWallAng = flVelAng + (float)iOpenSide * flWallOffset;
		flWishAng = AS_LerpAngle( flWishAng, flWallAng, flWall );

		// the blend may have moved the wishdir to the other side of velocity,
		// so re-derive the side for a correct key/yaw mapping below
		iSide = (AS_AngleNormalize( flWishAng - flVelAng ) >= 0.0f) ? 1 : -1;
	}
#endif

	// convert wishdir -> (view yaw, key combo). The preferred keys are the
	// pure strafes (or W+strafe in blend mode), which park the camera 90 (or
	// 45) off the wishdir - close to the travel direction in normal strafing.
	float flPrefOff = bBlendForward ? 45.0f : 90.0f;
	if (iSide < 0)
		flPrefOff = -flPrefOff;

	int iPrefBtn = (iSide > 0) ? IN_MOVELEFT : IN_MOVERIGHT;
	if (bBlendForward)
		iPrefBtn |= IN_FORWARD;

	// how far off the travel direction a pure strafe key would park the
	// camera. Near zero in normal strafing; grows big only when the wishdir
	// points far behind the velocity (flew past the target, braking back)
	float flStrafeDev = fabs( AS_AngleNormalize( flWishAng - (float)iSide * 90.0f - flVelAng ) );

	// -- FLEXIBLE KEY PICK (see the define block in airstrafe.h) --------------
	// two thresholds so the choice can't flap: flex engages past VIEW_DEV, and
	// only hands back to the preferred strafe keys once comfortably inside it
	bool bFlex;
	if (s_iAirStrafeFlexBtn[iState] == 0)
		bFlex = (flStrafeDev > AIRSTRAFE_FLEX_VIEW_DEV);
	else
		bFlex = (flStrafeDev > AIRSTRAFE_FLEX_VIEW_DEV - AIRSTRAFE_FLEX_SWITCH_HYST);

	if (!bFlex)
	{
		s_iAirStrafeFlexBtn[iState] = 0;
		*piOutButtons = iPrefBtn;
		*pflOutYaw = AS_AngleNormalize( flWishAng - flPrefOff );
		return true;
	}

	// any movement key is fair game now: take the combo that keeps the camera
	// closest to the travel direction, like a human braking on S (then easing
	// through S+A -> A as the brake unwinds) while still looking roughly
	// where they're flying. The combos sit 45 apart, so the winner parks the
	// camera within 22.5.
	static const int iFlexCombo[8] =
	{
		IN_FORWARD,
		IN_FORWARD | IN_MOVELEFT,
		IN_MOVELEFT,
		IN_BACK    | IN_MOVELEFT,
		IN_BACK,
		IN_BACK    | IN_MOVERIGHT,
		IN_MOVERIGHT,
		IN_FORWARD | IN_MOVERIGHT
	};

	int iBestBtn = 0;
	float flBestDev = 999.0f;
	float flLastDev = -1.0f;
	for( int i = 0; i < 8; i++ )
	{
		float flDev = fabs( AS_AngleNormalize( flWishAng - AirStrafe_ButtonWishOffset( iFlexCombo[i] ) - flVelAng ) );

		if (iFlexCombo[i] == s_iAirStrafeFlexBtn[iState])
			flLastDev = flDev;

		if (flDev < flBestDev)
		{
			flBestDev = flDev;
			iBestBtn = iFlexCombo[i];
		}
	}

	// stick with the keys already held unless the winner is meaningfully better
	if (flLastDev >= 0.0f && flLastDev <= flBestDev + AIRSTRAFE_FLEX_SWITCH_HYST)
		iBestBtn = s_iAirStrafeFlexBtn[iState];

	s_iAirStrafeFlexBtn[iState] = iBestBtn;
	*piOutButtons = iBestBtn;
	*pflOutYaw = AS_AngleNormalize( flWishAng - AirStrafe_ButtonWishOffset( iBestBtn ) );
	return true;
}

//------------------------------------------------------------------------------------------------------------
// Apply it
//------------------------------------------------------------------------------------------------------------
bool Bot_AirStrafeTo( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	int *piIoButtons, float *pflForwardSpeed, float *pflSideSpeed,
	bool bBlendForward, float flSpeedGoal, float flTurnRate, bool bDebug )
{
	if (player == NULL || !player->IsAlive())
		return false;

	// air-strafing is only meaningful off the ground
	if (!AirStrafe_IsAirborne( player ))
		return false;

	float flYaw;
	int iButtons;
	if (!AirStrafe_SolveCameraYaw( player, vTarget, flDeltaT, &flYaw, &iButtons, bBlendForward, flSpeedGoal ))
		return false;

	// clear the other movement keys so the wishdir stays clean, hold the combo
	*piIoButtons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
	*piIoButtons |= iButtons;

	// GoldSrc fakeclients move on explicit speeds: hand back the
	// forwardmove/sidemove matching the held keys for RunPlayerMove
	float flMoveSpeed = player->pev->maxspeed;
	if (flMoveSpeed <= 0.0f)
		flMoveSpeed = 270.0f;

	*pflForwardSpeed = 0.0f;
	*pflSideSpeed = 0.0f;
	if (iButtons & IN_FORWARD)   *pflForwardSpeed += flMoveSpeed;
	if (iButtons & IN_BACK)      *pflForwardSpeed -= flMoveSpeed;
	if (iButtons & IN_MOVERIGHT) *pflSideSpeed += flMoveSpeed;
	if (iButtons & IN_MOVELEFT)  *pflSideSpeed -= flMoveSpeed;

	// ease the view toward the solved yaw
	float flMaxStep = flTurnRate * flDeltaT;
	player->pev->v_angle.y = AS_ApproachYaw( player->pev->v_angle.y, flYaw, flMaxStep );

	// pitch doesn't affect the horizontal wishdir when only strafe keys are
	// held, so ease it toward level for a natural look-ahead posture.
	// (The original eased pitch toward vTarget[2] - a world Z fed into an
	// angle slot, which reads like a slip; "toward level" matches its comment.)
	player->pev->v_angle.x = AS_ApproachYaw( player->pev->v_angle.x, 0.0f, flMaxStep );
	if (player->pev->v_angle.x > 89.0f)
		player->pev->v_angle.x = 89.0f;
	if (player->pev->v_angle.x < -89.0f)
		player->pev->v_angle.x = -89.0f;
	player->pev->v_angle.z = 0.0f;

	if (bDebug)
	{
		Vector vPos = player->pev->origin;
		Vector2D vVel( player->pev->velocity.x, player->pev->velocity.y );

		// velocity (blue)
		UTIL_DrawBeamPoints( vPos, vPos + Vector( vVel.x, vVel.y, 0 ), 1, 0, 0, 255 );

		// wishdir from the solved yaw (green), reconstructed from the combo
		float r = AS_DEG2RAD( flYaw + AirStrafe_ButtonWishOffset( iButtons ) );
		UTIL_DrawBeamPoints( vPos, vPos + Vector( cos( r ) * 200.0f, sin( r ) * 200.0f, 0 ), 1, 0, 255, 0 );
	}

	return true;
}

//------------------------------------------------------------------------------------------------------------
// Landing prediction (where does this flight put us down)
//------------------------------------------------------------------------------------------------------------
bool AirStrafe_PredictLandingPos( CBasePlayer *player, Vector *pvLandingOut, float *pflAirTimeOut,
	float flMaxTime, bool bDebug )
{
	*pflAirTimeOut = 0.0f;

	if (player == NULL || !player->IsAlive())
		return false;

	Vector vPos = player->pev->origin;
	*pvLandingOut = vPos;

	// already standing on something - we land where we are
	if (!AirStrafe_IsAirborne( player ))
		return true;

	Vector vVel = player->pev->velocity;

	// the player's real hull (crouch included) so ledges catch us like they
	// catch the engine. GoldSrc origins are hull centers, so tracing the
	// matching fixed hull along the origin path is exact.
	int iHull = (player->pev->flags & FL_DUCKING) ? head_hull : human_hull;

	float flGrav = AirStrafe_GetGravity( player );
	float flTime = 0.0f;
	int iClips = 0;

	while( flTime < flMaxTime )
	{
		float flStep = AIRSTRAFE_PREDICT_STEP;
		if (flTime + flStep > flMaxTime)
			flStep = flMaxTime - flTime;

		// closed-form segment endpoint under gravity: the engine's half-tick
		// gravity integration reproduces this exactly at tick boundaries
		Vector vEnd( vPos.x + vVel.x * flStep,
					 vPos.y + vVel.y * flStep,
					 vPos.z + vVel.z * flStep - 0.5f * flGrav * flStep * flStep );

		TraceResult tr;
		UTIL_TraceHull( vPos, vEnd, ignore_monsters, iHull, player->edict(), &tr );

		if (tr.fStartSolid)
		{
			// wedged inside something - no honest prediction from here
			*pvLandingOut = vPos;
			*pflAirTimeOut = flTime;
			return false;
		}

		if (tr.flFraction >= 1.0f)
		{
			// clear segment - keep falling
			if (bDebug)
				UTIL_DrawBeamPoints( vPos, vEnd, 1, 255, 128, 0 );
			vPos = vEnd;
			vVel.z -= flGrav * flStep;
			flTime += flStep;
			continue;
		}

		if (bDebug)
			UTIL_DrawBeamPoints( vPos, tr.vecEndPos, 1, 255, 128, 0 );

		flTime += flStep * tr.flFraction;
		vVel.z -= flGrav * flStep * tr.flFraction;
		vPos = tr.vecEndPos;

		// standable ground - this is the landing
		if (tr.vecPlaneNormal.z >= AIRSTRAFE_WALL_NORMAL_THRESHOLD)
		{
			*pvLandingOut = vPos;
			*pflAirTimeOut = flTime;
			if (bDebug)
				UTIL_DrawBeamPoints( vPos, vPos + Vector( 0, 0, 32 ), 1, 0, 255, 0 );
			return true;
		}

		// wall or ceiling - slide along the plane like the engine's
		// ClipVelocity and keep falling
		if (++iClips > AIRSTRAFE_PREDICT_MAX_CLIPS)
			break;

		float flBack = DotProduct( vVel, tr.vecPlaneNormal );
		vVel = vVel - tr.vecPlaneNormal * flBack;

		// step a hair off the plane so the next sweep doesn't start on it
		vPos = vPos + tr.vecPlaneNormal * 0.1f;
	}

	// no floor inside the budget (bottomless pit, huge fall, clip loop) -
	// hand back where the sim ended so the caller still has something to use
	*pvLandingOut = vPos;
	*pflAirTimeOut = flTime;
	return false;
}

//------------------------------------------------------------------------------------------------------------
// Strafe reachability (can we make it there before touchdown)
//------------------------------------------------------------------------------------------------------------
bool AirStrafe_CanReachBeforeLanding( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	float *pflTimeOut, float flRadius, float flAirTime )
{
	*pflTimeOut = -1.0f;

	if (player == NULL || !player->IsAlive())
		return false;

	// grounded = no airtime to spend; mirror Bot_AirStrafeTo's contract
	if (!AirStrafe_IsAirborne( player ))
		return false;

	Vector vPos = player->pev->origin;
	Vector vVel = player->pev->velocity;

	float flGrav = AirStrafe_GetGravity( player );
	if (flGrav < 1.0f)
		flGrav = 1.0f;		// zero g would divide below - clamp

	// -- clock 1: ballistic airtime -------------------------------------------
	if (flAirTime < 0.0f)
	{
		Vector vLand;
		AirStrafe_PredictLandingPos( player, &vLand, &flAirTime );
		// a false return leaves the sim-end time in flAirTime, still an honest cap
	}

	// -- clock 2: the target's height window ----------------------------------
	float flUp = vTarget.z - vPos.z;

	// how much higher the arc still rises; zero once falling
	float flRise = (vVel.z > 0.0f) ? (vVel.z * vVel.z) / (2.0f * flGrav) : 0.0f;
	if (flUp > flRise + flRadius)
		return false;		// above the apex - strafing adds no height

	// descending root of z0 + vz*t - g*t^2/2 = ztarget - radius: the last
	// moment the arc is still level with (or above) the target. The apex
	// check above guarantees the discriminant is non-negative.
	float flDisc = vVel.z * vVel.z + 2.0f * flGrav * (flRadius - flUp);
	float flDeadline = (flDisc >= 0.0f) ? (vVel.z + sqrt( flDisc )) / flGrav : 0.0f;

	float flBudget = (flAirTime < flDeadline) ? flAirTime : flDeadline;
	flBudget *= AIRSTRAFE_REACH_MARGIN;
	if (flBudget <= 0.0f)
		return false;

	// -- the horizontal chase, real AirAccelerate per tick ---------------------
	float flWishspeed;
	float flL = AirStrafe_MaxAccelPerTick( player, flDeltaT, &flWishspeed );
	float flCap = (flWishspeed < AIRSTRAFE_AIR_CAP) ? flWishspeed : AIRSTRAFE_AIR_CAP;

	float px = vPos.x, py = vPos.y;
	float vx = vVel.x, vy = vVel.y;
	float flR2 = flRadius * flRadius;

	int iTicks = (int)ceil( flBudget / flDeltaT );
	for( int i = 0; i <= iTicks; i++ )
	{
		float dx = vTarget.x - px;
		float dy = vTarget.y - py;
		float flDist2 = dx * dx + dy * dy;
		if (flDist2 <= flR2)
		{
			*pflTimeOut = (float)i * flDeltaT;
			return true;
		}

		// unit direction to the target
		float flDist = sqrt( flDist2 );
		float ux = dx / flDist;
		float uy = dy / flDist;

		float flSpeed2 = vx * vx + vy * vy;
		float wx, wy;
		if (flSpeed2 < 1.0f)
		{
			// no heading to strafe off of - just push straight at it
			wx = ux;
			wy = uy;
		}
		else
		{
			float flSpd = sqrt( flSpeed2 );
			float fx = vx / flSpd;
			float fy = vy / flSpd;

			// cos-space version of AirStrafe_OptimalOffsetDeg - saves the
			// acos/cos round trip in the hot loop
			float flCosOpt = 0.0f;
			if (flL < flCap)
			{
				flCosOpt = (flCap - flL) / flSpd;
				if (flCosOpt > 1.0f)
					flCosOpt = 1.0f;
			}

			// offset = clamp( bearing to target, theta_opt .. 180 ). In cos
			// space that's just capping cos(phi) at cos(theta_opt). Aligned
			// target -> theta_opt (free speed gain); past 90 the wishdir
			// points behind the velocity and the sim BRAKES, so a target we
			// flew past still reads reachable through the same S-key save the
			// live controller actually performs.
			float flCosUse = fx * ux + fy * uy;
			if (flCosUse > flCosOpt)
				flCosUse = flCosOpt;
			float flSinUse = sqrt( 1.0f - flCosUse * flCosUse );

			// rotate the velocity heading by that offset toward the target side
			float flSide = (fx * uy - fy * ux >= 0.0f) ? 1.0f : -1.0f;
			wx = fx * flCosUse - fy * flSinUse * flSide;
			wy = fy * flCosUse + fx * flSinUse * flSide;
		}

		// the exact engine rule (see THE AIR-ACCEL RULE in airstrafe.h)
		float flCur = vx * wx + vy * wy;
		float flAdd = flCap - flCur;
		if (flAdd > 0.0f)
		{
			float flAcc = (flL < flAdd) ? flL : flAdd;
			vx += wx * flAcc;
			vy += wy * flAcc;
		}

		px += vx * flDeltaT;
		py += vy * flDeltaT;
	}

	return false;
}
