// airstrafe.h
// Camera-angle air-strafing for pathing bots.
//
// Ported to GoldSrc/hlsdk-portable from cavortbots' airstrafe.inc (SourceMod).
// The physics carries over almost unchanged: TF2's AirAccelerate is inherited
// from GoldSrc's PM_AirAccelerate - same 30 u/s air cap, same per-tick accel
// budget L = sv_airaccelerate * wishspeed * frametime * friction.
//
// WHAT THIS SOLVES
// ----------------
//   "I'm trying to strafe to a specified position. What angle do I need to
//    LOOK at, each tick, to curve toward that point through the air WITHOUT
//    bleeding speed - and gain speed when I can?"
//
//   The whole thing is solved as a single value per tick: the VIEW YAW. The
//   bot holds one strafe key and we point the camera; everything else (the
//   wishdir, the accel) falls out of the engine.
//
// HOW BOT MOVEMENT MAPS TO A WISHDIR
// ----------------------------------
//   The engine builds the wishdir from forwardmove/sidemove rotated by the
//   view yaw. With roll = 0 a single held strafe key gives a world wishdir
//   exactly 90 deg off the view:
//
//       hold MOVERIGHT  ->  wishdir = yaw - 90      =>  yaw = wishdir + 90
//       hold MOVELEFT   ->  wishdir = yaw + 90      =>  yaw = wishdir - 90
//       hold FORWARD    ->  wishdir = yaw
//       hold BACK       ->  wishdir = yaw + 180     =>  yaw = wishdir - 180
//
//   and the two-key diagonals sit 45/135 off. Pick the wishdir we WANT, then
//   derive the yaw to look at. See AirStrafe_ButtonWishOffset().
//
//   GOLDSRC NOTE: pfnRunPlayerMove takes explicit forwardmove/sidemove, so
//   Bot_AirStrafeTo also hands back the speeds matching the chosen keys -
//   feed them straight into your RunPlayerMove call.
//
// PUBLIC API (mirrors the SourceMod original; deltaT = your command interval)
// ---------------------------------------------------------------------------
//   float AirStrafe_GetAirAccel()
//   float AirStrafe_GetGravity( player )
//   float AirStrafe_MaxAccelPerTick( player, deltaT, &wishspeed )    // L
//   float AirStrafe_OptimalOffsetDeg( speed, L, airCap = 30 )        // theta_opt
//   float AirStrafe_ButtonWishOffset( buttons )                      // combo -> wishdir-off-view
//   bool  AirStrafe_IsAirborne( player )
//   bool  AirStrafe_SolveCameraYaw( player, target, deltaT, &yaw, &buttons,
//                                   blendForward = false, speedGoal = -1 )
//   bool  Bot_AirStrafeTo( player, target, deltaT, &ioButtons,
//                          &forwardSpeed, &sideSpeed, ... )          // engaged?
//   void  AirStrafe_Reset( player )                                  // on spawn/death
//   bool  AirStrafe_PredictLandingPos( player, &landing, &airTime, ... )
//   bool  AirStrafe_CanReachBeforeLanding( player, target, deltaT, &time, ... )
//
// NOTE Bot_AirStrafeTo only acts while airborne, clears the other move keys
//      so the wishdir stays clean, and eases pev->v_angle itself. It returns
//      false on the ground so the caller can fall back to normal navigation.

#ifndef AIRSTRAFE_H
#define AIRSTRAFE_H

class CBasePlayer;

// Hardcoded air speed cap used by PM_AirAccelerate's addspeed clamp (the
// famous 30). Not a cvar in GoldSrc; change only if your movement code does.
#define AIRSTRAFE_AIR_CAP        30.0f

// Below this horizontal speed (HU/s) there's no momentum worth strafing off
// of, so we just point at the target and let normal forward accel work.
#define AIRSTRAFE_MIN_SPEED      60.0f

// +-this many degrees of "aligned" before we let the weave flip sides. Wider
// = smoother, lazier S-weave; narrower = tighter to the path but twitchier.
#define AIRSTRAFE_ALIGN_DEADBAND 12.0f

// How fast the bot is allowed to swing its view (deg/sec). Human-plausible
// smooth-tracking speed; also the source of natural tracking lag.
#define AIRSTRAFE_TURN_RATE      600.0f

// Air surface friction; 1.0 in the air for all practical cases.
#define AIRSTRAFE_SURF_FRICTION  1.0f

// Speed (multiple of max speed) at which the bot fully commits to hugging the
// path. Below it, it increasingly prioritises BUILDING speed over accuracy.
#define AIRSTRAFE_PATH_BIAS_MULT 1.25f

// "Build speed" heading offset from velocity (deg). 90 = carve a pure circle,
// the fastest way to add speed when slow. Blended toward the path by speed.
#define AIRSTRAFE_CIRCLE_LEAD    90.0f

// ---- FLEXIBLE KEY PICK ------------------------------------------------------
// The pure strafe keys are only PREFERRED. When they'd park the camera more
// than VIEW_DEV degrees off the travel direction (classic case: flew past the
// target, the wishdir now points backward), the bot shops all eight key
// combos for the one keeping the camera closest to the travel direction -
// like a human easing onto S to brake back onto a target. SWITCH_HYST is how
// much better (deg) another combo must be before the held keys change.
#define AIRSTRAFE_FLEX_VIEW_DEV    45.0f
#define AIRSTRAFE_FLEX_SWITCH_HYST 12.0f

// ---- WALL AVOIDANCE ---------------------------------------------------------
// When the strafe curve is about to clip a wall, sharpen (and brake) the turn
// instead of holding the speed-optimal angle. 0 disables all probing.
#define AIRSTRAFE_WALL_AVOID          1

// Seconds of velocity to look ahead for a wall; probe dist = speed * this.
#define AIRSTRAFE_WALL_LOOKAHEAD_TIME 0.35f
#define AIRSTRAFE_WALL_LOOKAHEAD_MIN  48.0f
#define AIRSTRAFE_WALL_LOOKAHEAD_MAX  340.0f

// Left/right escape probes are this many degrees off the velocity heading.
#define AIRSTRAFE_WALL_SIDE_ANGLE     55.0f
// How much clearer one side must read before we steer toward it.
#define AIRSTRAFE_WALL_SIDE_HYST      0.10f

// Sharpest offset from velocity (deg) at full wall danger. >90 = the wishdir
// points partly backward, so the bot brakes as it turns.
#define AIRSTRAFE_WALL_MAX_OFFSET     130.0f

// Probe height above the origin. (The original cast a custom 16x16x32 hull;
// GoldSrc only has fixed hulls, so probes use head_hull, 16x16x18 - close.)
#define AIRSTRAFE_WALL_CAST_Z         40.0f

// ---- LANDING PREDICTION & STRAFE REACHABILITY -------------------------------

// Seconds of arc flown per hull sweep in AirStrafe_PredictLandingPos.
#define AIRSTRAFE_PREDICT_STEP        0.06f
// Stop predicting past this much flight (bottomless pits).
#define AIRSTRAFE_PREDICT_MAX_TIME    3.0f
// Wall slides allowed in one prediction before the arc is called degenerate.
#define AIRSTRAFE_PREDICT_MAX_CLIPS   8
// "Made it" radius for AirStrafe_CanReachBeforeLanding, about a player width.
#define AIRSTRAFE_REACH_RADIUS        60.0f
// The live controller trails the ideal sim a little (eased view, wall
// avoidance), so the reachability budget is discounted by this to stay honest.
#define AIRSTRAFE_REACH_MARGIN        0.9f
// Standable ground cutoff for a plane normal's z, matches the engine.
#define AIRSTRAFE_WALL_NORMAL_THRESHOLD 0.7f

//------------------------------------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------------------------------------
float AirStrafe_GetAirAccel( void );				///< sv_airaccelerate (default 10)
float AirStrafe_GetGravity( CBasePlayer *player );	///< sv_gravity scaled by pev->gravity
bool AirStrafe_IsAirborne( CBasePlayer *player );	///< off the ground and not swimming
float AirStrafe_ButtonWishOffset( int iButtons );	///< key combo -> wishdir offset from view yaw (deg)
void AirStrafe_Reset( CBasePlayer *player );		///< clear per-bot strafe state (call on spawn/death)

//------------------------------------------------------------------------------------------------------------
// Core math
//------------------------------------------------------------------------------------------------------------

/// L = sv_airaccelerate * wishspeed * deltaT * friction: the most speed (HU/s)
/// the engine can add along the wishdir in one tick. deltaT = your command
/// interval (the msec you pass to RunPlayerMove, in seconds).
float AirStrafe_MaxAccelPerTick( CBasePlayer *player, float flDeltaT, float *pflWishspeedOut );

/// The angle (deg, 0..90) to hold the wishdir off the current velocity for
/// maximum speed gain this tick: cos(theta_opt) = max(0, CAP - L) / speed.
float AirStrafe_OptimalOffsetDeg( float flSpeed, float flL, float flAirCap = AIRSTRAFE_AIR_CAP );

//------------------------------------------------------------------------------------------------------------
// The controller: velocity + target -> view yaw + key combo
//------------------------------------------------------------------------------------------------------------

/// Works out the view yaw to look at and the key combo to hold so the bot
/// curves toward vTarget through the air while preserving/building momentum.
/// Returns false only if vTarget is right on top of the bot (no heading).
bool AirStrafe_SolveCameraYaw( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	float *pflOutYaw, int *piOutButtons, bool bBlendForward = false, float flSpeedGoal = -1.0f );

/// Drive one tick of air-strafing toward vTarget: solves the yaw, eases
/// pev->v_angle toward it at flTurnRate deg/s, and hands back the buttons and
/// forwardmove/sidemove speeds for your RunPlayerMove call (the four move
/// keys in *piIoButtons are cleared and replaced with the solved combo).
/// No-op (returns false) on the ground - fall back to normal navigation.
bool Bot_AirStrafeTo( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	int *piIoButtons, float *pflForwardSpeed, float *pflSideSpeed,
	bool bBlendForward = false, float flSpeedGoal = -1.0f,
	float flTurnRate = AIRSTRAFE_TURN_RATE, bool bDebug = false );

//------------------------------------------------------------------------------------------------------------
// Landing prediction and reachability (decision helpers - not per-tick)
//------------------------------------------------------------------------------------------------------------

/// Predict where the player comes down if they stop steering right now, by
/// flying the velocity forward in analytic ballistic segments swept as hull
/// traces. Walls clip the velocity and the fall continues; a surface counts
/// as landing when its normal clears AIRSTRAFE_WALL_NORMAL_THRESHOLD.
/// Returns true if a floor was found (or already grounded); on false,
/// *pvLandingOut holds the last simulated position (still a usable cap).
/// Costs ~one trace per PREDICT_STEP of flight - call per decision, not per tick.
bool AirStrafe_PredictLandingPos( CBasePlayer *player, Vector *pvLandingOut, float *pflAirTimeOut,
	float flMaxTime = AIRSTRAFE_PREDICT_MAX_TIME, bool bDebug = false );

/// "If I commit my remaining airtime to strafing at vTarget, do I get there
/// before the ground takes the choice away?" Steps the horizontal plane per
/// tick with the real AirAccelerate rule under the same policy as the
/// controller. Kinematic only - geometry in the way is NOT checked.
/// Pass a cached airtime through flAirTime to skip re-tracing (<0 = predict).
bool AirStrafe_CanReachBeforeLanding( CBasePlayer *player, const Vector &vTarget, float flDeltaT,
	float *pflTimeOut, float flRadius = AIRSTRAFE_REACH_RADIUS, float flAirTime = -1.0f );

#endif // AIRSTRAFE_H
