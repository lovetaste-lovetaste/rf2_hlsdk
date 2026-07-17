// bot_phrases.h
// Minimal place-name directory for the navigation mesh.
//
// The CS/CZ bot code mapped nav mesh "place" names (e.g. "BombsiteA") to
// numeric Place IDs through its BotPhraseManager, which was tangled up with
// the radio chatter system. The nav code only ever uses the name<->ID
// mapping, so this small standalone registry replaces it for Rooster
// Fortress (see the @todo in nav_file.cpp that asked for exactly this).
//
// IDs are assigned sequentially starting at 1 the first time a name is seen.
// Place names are stored as strings inside the .nav file itself, so the
// mapping stays consistent across saves and loads even if IDs change order.
//
// NOTE: Like the other bot headers, include this after extdll.h/util.h.

#ifndef BOT_PHRASES_H
#define BOT_PHRASES_H

#include <vector>
#include "nav.h"		// for the Place typedef and UNDEFINED_PLACE

class BotPhraseManager
{
public:
	~BotPhraseManager();

	void Reset( void );							///< forget all known places (invoked on map change)

	Place NameToID( const char *name );			///< return ID for a place name, registering it if new
	const char *IDToName( Place place ) const;	///< return name for a place ID, or NULL if unknown

private:
	std::vector<char *> m_placeNames;			///< index + 1 == Place ID
};

extern BotPhraseManager *TheBotPhrases;

#endif // BOT_PHRASES_H
