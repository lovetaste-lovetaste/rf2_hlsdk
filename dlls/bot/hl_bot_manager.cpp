// hl_bot_manager.cpp
// Bot and navigation mesh manager for Half-Life deathmatch.
// See hl_bot_manager.h for an overview.

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"

#include "hl_bot_manager.h"
#include "hl_bot.h"
#include "bot_util.h"
#include "bot_phrases.h"
#include "nav_area.h"

//--------------------------------------------------------------------------------------------------------------
// Globals
//--------------------------------------------------------------------------------------------------------------
CHLBotManager *TheHLBots = NULL;

//--------------------------------------------------------------------------------------------------------------
// Bot/nav cvars (declared extern in bot_util.h)
//--------------------------------------------------------------------------------------------------------------
cvar_t cv_bot_traceview		= { "bot_traceview", "0", FCVAR_SERVER };
cvar_t cv_bot_stop			= { "bot_stop", "0", FCVAR_SERVER };
cvar_t cv_bot_show_nav		= { "bot_show_nav", "0", FCVAR_SERVER };
cvar_t cv_bot_show_danger	= { "bot_show_danger", "0", FCVAR_SERVER };
cvar_t cv_bot_nav_edit		= { "bot_nav_edit", "0", FCVAR_SERVER };
cvar_t cv_bot_nav_zdraw		= { "bot_nav_zdraw", "4", FCVAR_SERVER };
cvar_t cv_bot_debug			= { "bot_debug", "0", FCVAR_SERVER };
cvar_t cv_bot_quicksave		= { "bot_quicksave", "0", FCVAR_SERVER };

void Bot_RegisterCvars( void )
{
	CVAR_REGISTER( &cv_bot_traceview );
	CVAR_REGISTER( &cv_bot_stop );
	CVAR_REGISTER( &cv_bot_show_nav );
	CVAR_REGISTER( &cv_bot_show_danger );
	CVAR_REGISTER( &cv_bot_nav_edit );
	CVAR_REGISTER( &cv_bot_nav_zdraw );
	CVAR_REGISTER( &cv_bot_debug );
	CVAR_REGISTER( &cv_bot_quicksave );
}

//--------------------------------------------------------------------------------------------------------------
// The nav "place" currently used by the nav editing code when painting
// place names onto areas.
//--------------------------------------------------------------------------------------------------------------
static unsigned int s_navPlace = UNDEFINED_PLACE;

unsigned int GetNavPlace( void )
{
	return s_navPlace;
}

void SetNavPlace( unsigned int place )
{
	s_navPlace = place;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Engine callback for all "bot_*" server commands registered below.
 */
static void Bot_ServerCommand( void )
{
	if (TheHLBots)
		TheHLBots->ServerCommand( CMD_ARGV( 0 ) );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Create the bot manager singleton. Invoked once from GameDLLInit().
 */
void InstallBotControl( void )
{
	if (TheHLBots == NULL)
	{
		TheHLBots = new CHLBotManager;
		TheHLBots->AddServerCommands();
	}
}

//--------------------------------------------------------------------------------------------------------------
// Nav edit commands: console command name -> nav edit action.
// These act on the nav area under the local player's crosshair while
// bot_nav_edit is 1 (listen server only). Bind them to keys for editing.
//--------------------------------------------------------------------------------------------------------------
static const struct NavEditCommand
{
	const char *name;
	NavEditCmdType cmd;
}
navEditCommands[] =
{
	{ "bot_nav_mark",					EDIT_MARK },
	{ "bot_nav_mark_unnamed",			EDIT_MARK_UNNAMED },
	{ "bot_nav_delete",					EDIT_DELETE },
	{ "bot_nav_split",					EDIT_SPLIT },
	{ "bot_nav_merge",					EDIT_MERGE },
	{ "bot_nav_connect",				EDIT_CONNECT },
	{ "bot_nav_disconnect",				EDIT_DISCONNECT },
	{ "bot_nav_begin_area",				EDIT_BEGIN_AREA },
	{ "bot_nav_end_area",				EDIT_END_AREA },
	{ "bot_nav_splice",					EDIT_SPLICE },
	{ "bot_nav_crouch",					EDIT_ATTRIB_CROUCH },
	{ "bot_nav_jump",					EDIT_ATTRIB_JUMP },
	{ "bot_nav_precise",				EDIT_ATTRIB_PRECISE },
	{ "bot_nav_no_jump",				EDIT_ATTRIB_NO_JUMP },
	{ "bot_nav_toggle_place_mode",		EDIT_TOGGLE_PLACE_MODE },
	{ "bot_nav_toggle_place_painting",	EDIT_TOGGLE_PLACE_PAINTING },
	{ "bot_nav_place_floodfill",		EDIT_PLACE_FLOODFILL },
	{ "bot_nav_place_pick",				EDIT_PLACE_PICK },
	{ "bot_nav_warp",					EDIT_WARP_TO_MARK },
	{ "bot_nav_corner_select",			EDIT_SELECT_CORNER },
	{ "bot_nav_corner_raise",			EDIT_RAISE_CORNER },
	{ "bot_nav_corner_lower",			EDIT_LOWER_CORNER },

	{ NULL, EDIT_NONE }
};

//--------------------------------------------------------------------------------------------------------------
CHLBotManager::CHLBotManager()
{
	m_navLoaded = false;
	m_isGenerating = false;
	m_currentNode = NULL;
	m_generationDir = NORTH;
	m_seedIndex = 0;
	m_editCmd = EDIT_NONE;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the filename for this map's navigation mesh.
 */
const char *CHLBotManager::GetNavMapFilename( void ) const
{
	static char filename[256];
	sprintf( filename, "maps/%s.nav", STRING( gpGlobals->mapname ) );
	return filename;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Invoked when a new map has spawned (from ServerActivate() in client.cpp).
 * Loads this map's navigation mesh, if one exists.
 */
void CHLBotManager::ServerActivate( void )
{
	m_isGenerating = false;
	m_editCmd = EDIT_NONE;
	SetNavPlace( UNDEFINED_PLACE );
	EditNavAreasReset();
	InitBotTrig();

	NavErrorType result = LoadNavigationMap();		// frees any previous map's nav data first
	m_navLoaded = (result == NAV_OK);

	if (result == NAV_OK)
		CONSOLE_ECHO( "Loaded navigation map '%s'.\n", GetNavMapFilename() );
	else if (result == NAV_CANT_ACCESS_FILE)
		CONSOLE_ECHO( "No navigation map for this map. Use 'bot_nav_generate' to create one.\n" );
	else
		CONSOLE_ECHO( "ERROR: Navigation map '%s' is unusable (error %d).\n", GetNavMapFilename(), (int)result );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Invoked when the map is changing or the server is shutting down.
 */
void CHLBotManager::ServerDeactivate( void )
{
	DestroyNavigationMap();
	TheBotPhrases->Reset();		// place names are per-map; the next nav file re-registers its own

	m_navLoaded = false;
	m_isGenerating = false;
	m_currentNode = NULL;
	m_walkableSeeds.clear();
}

//--------------------------------------------------------------------------------------------------------------
void CHLBotManager::ClientDisconnect( CBasePlayer *player )
{
	// hook point: nothing to clean up yet
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Handle the "bot_*" server commands registered in AddServerCommands().
 */
void CHLBotManager::ServerCommand( const char *pcmd )
{
	if (FStrEq( pcmd, "bot_add" ))
	{
		CHLBot::CreateBot( (CMD_ARGC() >= 2) ? CMD_ARGV( 1 ) : NULL );
		return;
	}

	if (FStrEq( pcmd, "bot_nav_generate" ))
	{
		BeginNavGeneration();
		return;
	}

	if (FStrEq( pcmd, "bot_nav_save" ))
	{
		if (SaveNavigationMap( GetNavMapFilename() ))
			CONSOLE_ECHO( "Navigation map saved to '%s'.\n", GetNavMapFilename() );
		else
			CONSOLE_ECHO( "ERROR: Could not save navigation map to '%s'.\n", GetNavMapFilename() );
		return;
	}

	if (FStrEq( pcmd, "bot_nav_load" ))
	{
		NavErrorType result = LoadNavigationMap();
		m_navLoaded = (result == NAV_OK);
		CONSOLE_ECHO( (result == NAV_OK) ? "Navigation map loaded.\n" : "ERROR: Could not load navigation map.\n" );
		return;
	}

	if (FStrEq( pcmd, "bot_nav_check" ))
	{
		SanityCheckNavigationMap( STRING( gpGlobals->mapname ) );
		return;
	}

	if (FStrEq( pcmd, "bot_nav_place_name" ))
	{
		// set the current place used by place painting, e.g: bot_nav_place_name Rooftop
		if (CMD_ARGC() < 2)
		{
			CONSOLE_ECHO( "Usage: bot_nav_place_name <name>\n" );
			return;
		}

		SetNavPlace( TheBotPhrases->NameToID( CMD_ARGV( 1 ) ) );
		CONSOLE_ECHO( "Current nav place set to '%s'.\n", CMD_ARGV( 1 ) );
		return;
	}

	// nav editing commands - queued and consumed once per frame by StartFrame()
	for( int i = 0; navEditCommands[i].name; ++i )
	{
		if (FStrEq( pcmd, navEditCommands[i].name ))
		{
			if (cv_bot_nav_edit.value == 0.0f)
				CONSOLE_ECHO( "Set bot_nav_edit to 1 to edit the navigation mesh.\n" );
			else
				m_editCmd = navEditCommands[i].cmd;
			return;
		}
	}
}

//--------------------------------------------------------------------------------------------------------------
void CHLBotManager::AddServerCommand( const char *cmd )
{
	(*g_engfuncs.pfnAddServerCommand)( (char *)cmd, Bot_ServerCommand );
}

//--------------------------------------------------------------------------------------------------------------
void CHLBotManager::AddServerCommands( void )
{
	AddServerCommand( "bot_add" );
	AddServerCommand( "bot_nav_generate" );
	AddServerCommand( "bot_nav_save" );
	AddServerCommand( "bot_nav_load" );
	AddServerCommand( "bot_nav_check" );
	AddServerCommand( "bot_nav_place_name" );

	for( int i = 0; navEditCommands[i].name; ++i )
		AddServerCommand( navEditCommands[i].name );
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Invoked once per server frame (from StartFrame() in client.cpp).
 */
void CHLBotManager::StartFrame( void )
{
	// think all bots
	for( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CBasePlayer *player = static_cast<CBasePlayer *>( UTIL_PlayerByIndex( i ) );

		if (player == NULL || FNullEnt( player->pev ))
			continue;

		if (!(player->pev->flags & FL_FAKECLIENT))
			continue;

		// every fake client we create is a CHLBot (see CHLBot::CreateBot),
		// so a static_cast is safe - and works with RTTI disabled
		CHLBot *bot = static_cast<CHLBot *>( player );
		bot->BotThink();
	}

	if (m_isGenerating)
		UpdateNavGeneration();

	// nav mesh editing and debug drawing (listen server only)
	if (cv_bot_nav_edit.value != 0.0f)
	{
		EditNavAreas( m_editCmd );
		m_editCmd = EDIT_NONE;
	}

	if (cv_bot_show_danger.value != 0.0f)
		DrawDanger();
}

//--------------------------------------------------------------------------------------------------------------
//
// In-game navigation mesh generation.
//
// A walkable-space sampler: starting from every player spawn point, walkable
// space is flood-filled with a grid of CNavNodes; when sampling is done,
// GenerateNavigationAreaMesh() merges the nodes into nav areas and the
// result is saved as maps/<mapname>.nav.
//
//--------------------------------------------------------------------------------------------------------------

/**
 * Start generating the nav mesh. Progresses incrementally in StartFrame().
 */
void CHLBotManager::BeginNavGeneration( void )
{
	DestroyNavigationMap();
	m_navLoaded = false;

	// collect walkable seed spots - every player spawn point on the map
	m_walkableSeeds.clear();
	m_seedIndex = 0;

	static const char *seedClassnames[] = { "info_player_deathmatch", "info_player_start", NULL };
	for( int i = 0; seedClassnames[i]; ++i )
	{
		CBaseEntity *spot = NULL;
		while( (spot = UTIL_FindEntityByClassname( spot, seedClassnames[i] )) != NULL )
			m_walkableSeeds.push_back( spot->pev->origin );
	}

	if (m_walkableSeeds.empty())
	{
		CONSOLE_ECHO( "ERROR: No player spawn points found - cannot generate a navigation mesh.\n" );
		return;
	}

	CONSOLE_ECHO( "Generating navigation mesh from %d seed spots...\n", (int)m_walkableSeeds.size() );

	m_currentNode = NULL;
	m_isGenerating = true;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Take a budgeted number of sampling steps this frame, finishing the mesh
 * when sampling is complete. Budgeting avoids hitching the server.
 */
void CHLBotManager::UpdateNavGeneration( void )
{
	const int samplesPerFrame = 300;

	for( int i = 0; i < samplesPerFrame; ++i )
	{
		if (SampleStep())
			continue;

		// sampling is finished - build nav areas from the nodes and save
		CONSOLE_ECHO( "Sampling complete (%d nodes). Building nav areas...\n", CNavNode::GetListLength() );

		GenerateNavigationAreaMesh();
		CONSOLE_ECHO( "Created %d nav areas.\n", (int)TheNavAreaList.size() );

		// connect the fresh areas through the map's func_ladder entities so
		// paths can route up/down ladders immediately. Ladders aren't stored
		// in the .nav file - LoadNavigationMap() rebuilds them the same way
		// on every map load.
		BuildLadders();
		CONSOLE_ECHO( "Built %d nav ladders.\n", (int)TheNavLadderList.size() );

		if (SaveNavigationMap( GetNavMapFilename() ))
			CONSOLE_ECHO( "Navigation map saved to '%s'.\n", GetNavMapFilename() );
		else
			CONSOLE_ECHO( "ERROR: Could not save navigation map to '%s'.\n", GetNavMapFilename() );

		m_isGenerating = false;
		m_navLoaded = true;
		return;
	}
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Take one sampling step: try to walk one grid cell outward from the
 * current node in an unexplored direction.
 * Returns false when the entire walkable space has been sampled.
 */
bool CHLBotManager::SampleStep( void )
{
	// find a node with an unexplored direction
	while( true )
	{
		if (m_currentNode == NULL)
		{
			// exhausted this seed's reachable space - try the next seed
			m_currentNode = GetNextWalkableSeedNode();

			if (m_currentNode == NULL)
				return false;	// all seeds explored - sampling is complete
		}

		bool foundDir = false;
		for( int dir = NORTH; dir < NUM_DIRECTIONS; ++dir )
		{
			if (!m_currentNode->HasVisited( (NavDirType)dir ))
			{
				m_generationDir = (NavDirType)dir;
				foundDir = true;
				break;
			}
		}

		if (foundDir)
			break;

		// every direction from this node is explored - pop back to its parent
		m_currentNode = m_currentNode->GetParent();
	}

	m_currentNode->MarkAsVisited( m_generationDir );

	const Vector &from = *m_currentNode->GetPosition();

	Vector to = from;
	AddDirectionVector( &to, m_generationDir, GenerationStepSize );

	// find the ground at the destination, allowing for a crouch-jump-height step up
	Vector probe( to.x, to.y, from.z + JumpCrouchHeight );

	float ground;
	Vector normal;
	if (!GetGroundHeight( &probe, &ground, &normal ))
		return true;	// no ground that way - sampling continues elsewhere

	// too steep to stand on?
	if (normal.z < MaxUnitZSlope)
		return true;

	float deltaZ = ground - from.z;

	if (deltaZ > JumpCrouchHeight)	// too high to reach, even with a crouch-jump
		return true;

	if (deltaZ < -DeathDrop)		// falling this far would kill us
		return true;

	// check that the path between the two spots is clear at torso height
	TraceResult result;
	Vector traceFrom( from.x, from.y, from.z + HalfHumanHeight );
	Vector traceTo( to.x, to.y, ground + HalfHumanHeight );
	UTIL_TraceLine( traceFrom, traceTo, ignore_monsters, NULL, &result );

	if (result.flFraction != 1.0f || 0 != result.fStartSolid)
		return true;

	to.z = ground;
	AddSampledNode( &to, &normal, m_generationDir, m_currentNode );

	return true;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Add a sampled node at the given position, connecting it to the node we
 * stepped from. Reuses an existing node if one is already there.
 */
CNavNode *CHLBotManager::AddSampledNode( const Vector *destPos, const Vector *normal, NavDirType dir, CNavNode *source )
{
	CNavNode *node = const_cast<CNavNode *>( CNavNode::GetNode( destPos ) );

	bool isNew = false;
	if (node == NULL)
	{
		node = new CNavNode( destPos, normal, source );
		isNew = true;
	}

	source->ConnectTo( node, dir );

	// if the ground is nearly level, assume we can walk back the other way too
	if (fabs( source->GetPosition()->z - destPos->z ) < StepHeight)
	{
		node->ConnectTo( source, OppositeDirection( dir ) );
		node->MarkAsVisited( OppositeDirection( dir ) );
	}

	if (isNew)
	{
		// if a standing player doesn't fit here, mark the spot as crouch-only
		TraceResult result;
		Vector floorPos( destPos->x, destPos->y, destPos->z + 1.0f );
		Vector ceiling( destPos->x, destPos->y, destPos->z + HumanHeight - 1.0f );
		UTIL_TraceLine( floorPos, ceiling, ignore_monsters, NULL, &result );

		if (result.flFraction != 1.0f)
			node->SetAttributes( NAV_CROUCH );

		// new nodes become the sampling frontier (depth-first exploration)
		m_currentNode = node;
	}

	return node;
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Create a fresh sampling node at the next spawn point whose surroundings
 * have not been reached yet, or NULL when every seed has been used.
 * Multiple seeds matter on maps with areas that are not walkably connected
 * (e.g. rooms separated by a door or a drop).
 */
CNavNode *CHLBotManager::GetNextWalkableSeedNode( void )
{
	while( m_seedIndex < (int)m_walkableSeeds.size() )
	{
		Vector pos = m_walkableSeeds[ m_seedIndex ];
		++m_seedIndex;

		// align to the sampling grid and drop onto the ground
		SnapToGrid( &pos );

		Vector probe( pos.x, pos.y, pos.z + HalfHumanHeight );

		float ground;
		Vector normal;
		if (!GetGroundHeight( &probe, &ground, &normal ))
			continue;

		pos.z = ground;

		// skip seeds that sampling has already reached
		if (CNavNode::GetNode( &pos ))
			continue;

		return new CNavNode( &pos, &normal );
	}

	return NULL;
}
