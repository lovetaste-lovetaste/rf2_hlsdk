// tf2 shotgun

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "nodes.h"
#include "player.h"
#include "gamerules.h"
#include "rf_weaponbase.h"

class CRFShotgun : public CRFBasePlayerWeapon
{
public:
#if !CLIENT_DLL
	int		Save(CSave& save);
	int		Restore(CRestore& restore);
	static	TYPEDESCRIPTION m_SaveData[];
#endif
	void Spawn(void);
	void Precache(void);
	int iItemSlot();
	int GetItemInfo(ItemInfo* p);
	int AddToPlayer(CBasePlayer* pPlayer);

	void PrimaryAttack(void);
	void SecondaryAttack(void);
	BOOL Deploy();
	void Reload(void);
	void WeaponIdle(void);
	void ItemPostFrame(void);
	int m_fInReload;
	float m_flNextReload;
	int m_iShell;

	virtual BOOL UseDecrement(void)
	{
#if CLIENT_WEAPONS
		return TRUE;
#else
		return FALSE;
#endif
	}

private:
	unsigned short m_usDoubleFire;
	unsigned short m_usSingleFire;
};


// special deathmatch shotgun spreads
#define VECTOR_CONE_DM_SHOTGUN	Vector( 0.08716, 0.04362, 0.00 )// 10 degrees by 5 degrees
#define VECTOR_CONE_DM_DOUBLESHOTGUN Vector( 0.17365, 0.04362, 0.00 ) // 20 degrees by 5 degrees

enum rf_shotgun_e
{
	SHOTGUN_IDLE = 0,
	SHOTGUN_FIRE,
	SHOTGUN_FIRE2,
	SHOTGUN_RELOAD,
	SHOTGUN_PUMP,
	SHOTGUN_START_RELOAD,
	SHOTGUN_DRAW,
	SHOTGUN_HOLSTER,
	SHOTGUN_IDLE4,
	SHOTGUN_IDLE_DEEP
};

LINK_ENTITY_TO_CLASS(weapon_shotgun_rf, CRFShotgun)

int CRFShotgun::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = "buckshot";
	p->iMaxAmmo1 = BUCKSHOT_MAX_CARRY;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = SHOTGUN_MAX_CLIP;
	p->iSlot = SLOT_PRIMARY; // todo: if engineer, make it primary. else, make it secondary
	p->iPosition = 1;
	p->iFlags = 0;
	p->iId = m_iId = WEAPON_SHOTGUN;
	p->iWeight = SHOTGUN_WEIGHT;

	return 1;
}

int CRFShotgun::iItemSlot()
{
	// todo: make this universal
	// the hl1 weapons already do this manually ( without the iteminfoarray, just manually placed numbers ), so just make it easier so i dont have to go out of my way
	// 
	// THIS SHOULD BE HOW EVERY WEAPON IN THE GAME DOES THIS!!!!!
	return ItemInfoArray[m_iId].iSlot + 1;
}