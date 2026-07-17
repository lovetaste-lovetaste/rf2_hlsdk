// bot_phrases.cpp
// Minimal place-name directory for the navigation mesh.
// See bot_phrases.h for why this exists.

#include "extdll.h"
#include "util.h"

#include "shared_util.h"	// CloneString()
#include "bot_phrases.h"

// The one and only place directory.
// A static instance (instead of new/delete) keeps its lifetime independent
// of the bot manager and avoids static-initialization-order problems.
static BotPhraseManager s_botPhrases;
BotPhraseManager *TheBotPhrases = &s_botPhrases;

//--------------------------------------------------------------------------------------------------------------
BotPhraseManager::~BotPhraseManager()
{
	Reset();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Forget all known place names. Safe to call between maps; any names still
 * referenced by a nav file are re-registered when that file is loaded.
 */
void BotPhraseManager::Reset( void )
{
	for ( std::vector<char *>::iterator it = m_placeNames.begin(); it != m_placeNames.end(); ++it )
		delete [] *it;

	m_placeNames.clear();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the Place ID for the given name, registering the name if it has
 * not been seen before. Comparison is case-insensitive, matching the
 * original CS behavior.
 */
Place BotPhraseManager::NameToID( const char *name )
{
	if (name == NULL || *name == '\0')
		return UNDEFINED_PLACE;

	for ( unsigned int i = 0; i < m_placeNames.size(); ++i )
	{
		if (!stricmp( m_placeNames[i], name ))
			return (Place)(i + 1);
	}

	// new place - register it
	m_placeNames.push_back( CloneString( name ) );
	return (Place)m_placeNames.size();
}

//--------------------------------------------------------------------------------------------------------------
/**
 * Return the name of the given Place ID, or NULL if the ID is unknown.
 */
const char *BotPhraseManager::IDToName( Place place ) const
{
	if (place == UNDEFINED_PLACE || place > m_placeNames.size())
		return NULL;

	return m_placeNames[ place - 1 ];
}
