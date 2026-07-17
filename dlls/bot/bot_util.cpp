// bot_util.cpp
// Utility functions shared by the nav mesh code and the HL bot.
// Trimmed-down port of the CS/CZ bot_util.cpp - see bot_util.h.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "gamerules.h"

#include "bot_util.h"
#include "nav.h"		// NormalizeAnglePositive()

// beam sprite for nav mesh debug drawing (set in BotPrecache())
static short s_iBeamSprite = 0;

//--------------------------------------------------------------------------------------------------------------
/**
 * Precache assets the nav/bot debug code uses. Invoked from ServerActivate().
 */
void BotPrecache( void )
{
	// beam sprite for nav mesh debug drawing, plus the nav editing UI sounds
	s_iBeamSprite = PRECACHE_MODEL( "sprites/smoke.spr" );
	PRECACHE_SOUND( "buttons/bell1.wav" );
	PRECACHE_SOUND( "buttons/blip1.wav" );
	PRECACHE_SOUND( "buttons/blip2.wav" );
	PRECACHE_SOUND( "buttons/button11.wav" );
	PRECACHE_SOUND( "buttons/latchunlocked2.wav" );
	PRECACHE_SOUND( "buttons/lightswitch2.wav" );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the number of clients (humans and bots) in the game.
 */
int UTIL_ClientsInGame( void )
{
	int iCount = 0;

	for( int iIndex = 1; iIndex <= gpGlobals->maxClients; iIndex++ )
	{
		CBaseEntity *pPlayer = UTIL_PlayerByIndex( iIndex );

		if (pPlayer == NULL)
			continue;

		if (FNullEnt( pPlayer->pev ))
			continue;

		if (FStrEq( STRING( pPlayer->pev->netname ), "" ))
			continue;

		iCount++;
	}

	return iCount;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return true if the given name is already in use by another player.
 */
bool UTIL_IsNameTaken( const char *name )
{
	for( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBaseEntity *player = UTIL_PlayerByIndex( i );

		if (player == NULL)
			continue;

		if (FNullEnt( player->pev ))
			continue;

		if (FStrEq( STRING( player->pev->netname ), "" ))
			continue;

		if (!stricmp( name, STRING( player->pev->netname ) ))
			return true;
	}

	return false;
}

//--------------------------------------------------------------------------------------------------------------
void UTIL_DrawBeamPoints( Vector vecStart, Vector vecEnd, int iLifetime, byte bRed, byte bGreen, byte bBlue )
{
	MESSAGE_BEGIN( MSG_PVS, SVC_TEMPENTITY, vecStart );
		WRITE_BYTE( TE_BEAMPOINTS );
		WRITE_COORD( vecStart.x );
		WRITE_COORD( vecStart.y );
		WRITE_COORD( vecStart.z );
		WRITE_COORD( vecEnd.x );
		WRITE_COORD( vecEnd.y );
		WRITE_COORD( vecEnd.z );
		WRITE_SHORT( s_iBeamSprite );
		WRITE_BYTE( 0 );			// startframe
		WRITE_BYTE( 0 );			// framerate
		WRITE_BYTE( iLifetime );	// life
		WRITE_BYTE( 10 );			// width
		WRITE_BYTE( 0 );			// noise
		WRITE_BYTE( bRed );
		WRITE_BYTE( bGreen );
		WRITE_BYTE( bBlue );
		WRITE_BYTE( 255 );			// brightness
		WRITE_BYTE( 0 );			// speed
	MESSAGE_END();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the alive player closest to the given position, and optionally the distance to them.
 */
CBasePlayer *UTIL_GetClosestPlayer( const Vector *pos, float *distance )
{
	CBasePlayer *closePlayer = NULL;
	float closeDistSq = 1.0e12f;

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *player = static_cast<CBasePlayer *>( UTIL_PlayerByIndex( i ) );

		if (!IsEntityValid( player ))
			continue;

		if (!player->IsAlive())
			continue;

		Vector to = player->pev->origin - *pos;
		float distSq = DotProduct( to, to );
		if (distSq < closeDistSq)
		{
			closeDistSq = distSq;
			closePlayer = player;
		}
	}

	if (distance)
		*distance = sqrt( closeDistSq );

	return closePlayer;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the alive player on the given team closest to the given position,
 * and optionally the distance to them.
 */
CBasePlayer *UTIL_GetClosestPlayer( const Vector *pos, int team, float *distance )
{
	CBasePlayer *closePlayer = NULL;
	float closeDistSq = 1.0e12f;

	for( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *player = static_cast<CBasePlayer *>( UTIL_PlayerByIndex( i ) );

		if (!IsEntityValid( player ))
			continue;

		if (!player->IsAlive())
			continue;

		if (BotTeamID( player ) != team)
			continue;

		Vector to = player->pev->origin - *pos;
		float distSq = DotProduct( to, to );
		if (distSq < closeDistSq)
		{
			closeDistSq = distSq;
			closePlayer = player;
		}
	}

	if (distance)
		*distance = sqrt( closeDistSq );

	return closePlayer;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the listen server host - the local player the nav editing code follows.
 */
CBaseEntity *UTIL_GetLocalPlayer( void )
{
	return UTIL_PlayerByIndex( 1 );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return a numeric team ID for the player: the gamerules team index in
 * teamplay, or 0 in ordinary deathmatch where everyone is an enemy.
 */
int BotTeamID( CBasePlayer *player )
{
	if (g_pGameRules == NULL || !g_pGameRules->IsTeamplay())
		return 0;

	// team indices start at 0; shift by one so 0 can mean "no team"
	return g_pGameRules->GetTeamIndex( player->m_szTeamName ) + 1;
}

//--------------------------------------------------------------------------------------------------------------
void CONSOLE_ECHO( const char *pszMsg, ... )
{
	va_list argptr;
	static char szStr[1024];

	va_start( argptr, pszMsg );
	vsnprintf( szStr, sizeof( szStr ), pszMsg, argptr );
	va_end( argptr );

	(*g_engfuncs.pfnServerPrint)( szStr );
}

//--------------------------------------------------------------------------------------------------------------
void CONSOLE_ECHO_LOGGED( const char *pszMsg, ... )
{
	va_list argptr;
	static char szStr[1024];

	va_start( argptr, pszMsg );
	vsnprintf( szStr, sizeof( szStr ), pszMsg, argptr );
	va_end( argptr );

	(*g_engfuncs.pfnServerPrint)( szStr );
	UTIL_LogPrintf( "%s", szStr );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Show a hint message on every player's screen (used by the nav analysis code).
 */
void HintMessageToAllPlayers( const char *message )
{
	hudtextparms_t textParms;

	textParms.x = -1.0f;
	textParms.y = -1.0f;
	textParms.fadeinTime = 1.0f;
	textParms.fadeoutTime = 5.0f;
	textParms.holdTime = 5.0f;
	textParms.fxTime = 0.0f;
	textParms.r1 = 100;
	textParms.g1 = 255;
	textParms.b1 = 100;
	textParms.r2 = 255;
	textParms.g2 = 255;
	textParms.b2 = 255;
	textParms.effect = 0;
	textParms.channel = 0;

	UTIL_HudMessageAll( textParms, message );
}

//--------------------------------------------------------------------------------------------------------------
// Fast table-based trig, used by the danger-drawing code
//--------------------------------------------------------------------------------------------------------------
#define COS_TABLE_SIZE 256
static float cosTable[ COS_TABLE_SIZE ];

void InitBotTrig( void )
{
	for( int i = 0; i < COS_TABLE_SIZE; ++i )
	{
		float angle = 2.0f * M_PI * (float)i / (float)(COS_TABLE_SIZE - 1);
		cosTable[i] = cos( angle );
	}
}

float BotCOS( float angle )
{
	angle = NormalizeAnglePositive( angle );
	int i = angle * (COS_TABLE_SIZE - 1) / 360.0f;
	return cosTable[i];
}

float BotSIN( float angle )
{
	angle = NormalizeAnglePositive( angle - 90 );
	int i = angle * (COS_TABLE_SIZE - 1) / 360.0f;
	return cosTable[i];
}
