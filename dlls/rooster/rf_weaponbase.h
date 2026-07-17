enum slot_e
{
	SLOT_PRIMARY,
	SLOT_SECONDARY,
	SLOT_MELEE,
	SLOT_ITEM1,
	SLOT_ITEM2
};

enum viewmodel_e
{
	PRIMARY_DRAW,
	PRIMARY_IDLE,
	PRIMARY_FIRE,
	PRIMARY_RELOAD,
	PRIMARY_RELOAD_START,
	PRIMARY_RELOAD_END,

	SECONDARY_DRAW,
	SECONDARY_IDLE,
	SECONDARY_FIRE,
	SECONDARY_RELOAD,
	SECONDARY_RELOAD_START,
	SECONDARY_RELOAD_END,

	MELEE_DRAW,
	MELEE_IDLE,
	MELEE_SWING_A,
	MELEE_SWING_B,
	MELEE_SWING_CRIT,

	ITEM1_DRAW,
	ITEM1_IDLE,

	ITEM2_DRAW,
	ITEM2_IDLE,

	LAST_ENUM_ANIM
};
// todo: make a function that first looks for the sequences with a string before using the index numbers directly
// ex: look for "primary_shoot" or "primary_fire" first. if those sequences exist, use the index they are in. if they do not, then default to using the index enum number
// would allow flexibility with creating viewemodels

enum ViewAnimState
{
	ATTACK,
	ATTACK_ALT,	// example of why i make FindSequenceViewmodel use a case rather than just a hardcoded things, as some unlocks use seperate viewmodel anims
	FIRE,
	RELOAD,
	RELOAD_START,
	RELOAD_END,
	IDLE,
	DRAW
};