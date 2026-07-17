// hl_bot_manager.h
// Bot and navigation mesh manager for Half-Life deathmatch.
//
// Owns the nav mesh lifecycle (load on map start, in-game generation via
// "bot_nav_generate", editing via bot_nav_edit) and thinks all CHLBots once
// per server frame. Modeled on the CZ bot manager, but standalone - the CZ
// bot AI itself is deliberately NOT ported.
//
// Hooked from client.cpp (ServerActivate/ServerDeactivate/StartFrame/
// ClientDisconnect) and game.cpp (GameDLLInit registers cvars and installs
// the manager).

#ifndef HL_BOT_MANAGER_H
#define HL_BOT_MANAGER_H

#include <vector>

#include "nav.h"
#include "nav_node.h"
#include "nav_area.h"	// NavEditCmdType

class CBasePlayer;
class CHLBot;

//--------------------------------------------------------------------------------------------------------------
// The nav "place" currently used by the nav editing code when painting
// place names onto areas (see EditNavAreas() in nav_area.cpp).
// Free functions so nav_area.cpp has no dependency on the manager class.
//--------------------------------------------------------------------------------------------------------------
unsigned int GetNavPlace( void );
void SetNavPlace( unsigned int place );

//--------------------------------------------------------------------------------------------------------------
class CHLBotManager
{
public:
	CHLBotManager();

	void ServerActivate( void );				///< new map has spawned - load its nav mesh
	void ServerDeactivate( void );				///< map is changing - free nav data
	void StartFrame( void );					///< invoked once per server frame
	void ClientDisconnect( CBasePlayer *player );

	void ServerCommand( const char *pcmd );		///< handle the "bot_*" server commands
	void AddServerCommands( void );

	bool IsNavMeshLoaded( void ) const			{ return m_navLoaded; }

	const char *GetNavMapFilename( void ) const;	///< return the filename for this map's .nav file

private:
	bool m_navLoaded;							///< true if a nav mesh is loaded for the current map
	NavEditCmdType m_editCmd;					///< queued nav edit command, consumed each frame

	// in-game nav mesh generation (see comment block in hl_bot_manager.cpp)
	bool m_isGenerating;						///< true while incrementally sampling walkable space
	CNavNode *m_currentNode;					///< sampling frontier node
	NavDirType m_generationDir;					///< direction being sampled this step
	std::vector<Vector> m_walkableSeeds;		///< seed spots (player spawns) to sample from
	int m_seedIndex;							///< next seed to use

	void BeginNavGeneration( void );
	void UpdateNavGeneration( void );
	bool SampleStep( void );
	CNavNode *AddSampledNode( const Vector *destPos, const Vector *normal, NavDirType dir, CNavNode *source );
	CNavNode *GetNextWalkableSeedNode( void );

	void AddServerCommand( const char *cmd );
};

extern CHLBotManager *TheHLBots;

// game.cpp hooks
void Bot_RegisterCvars( void );
void InstallBotControl( void );

#endif // HL_BOT_MANAGER_H
