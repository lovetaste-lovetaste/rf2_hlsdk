#include "rf_viewmodel.h"
#include "stdio.h"

constexpr int VIEWMODEL_FIRST_CLASS = 1;
constexpr int VIEWMODEL_LAST_CLASS = 9;

// directory that holds every c_ weapon and arms model
// constexpr char CModelDirectory[] = "models/rooster_fortress/c_models/";

// constexpr int GoldSrcStudioMagic = ('T' << 24) + ('S' << 16) + ('D' << 8) + 'I';
// constexpr int GoldSrcStudioVersion = 10;

static const char* const g_ArmsModelsByClass[VIEWMODEL_LAST_CLASS + 1] =
{
	nullptr,											    // 0: CLASS_UNKNOWN
	"models/rooster_fortress/c_models/c_scout_arms.mdl",   // 1: CLASS_SCOUT
	"models/rooster_fortress/c_models/c_heavy_arms.mdl",   // 2: CLASS_HEAVY
	"models/rooster_fortress/c_models/c_soldier_arms.mdl", // 3: CLASS_SOLDIER
	"models/rooster_fortress/c_models/c_pyro_arms.mdl",    // 4: CLASS_PYRO
	"models/rooster_fortress/c_models/c_sniper_arms.mdl",  // 5: CLASS_SNIPER
	"models/rooster_fortress/c_models/c_medic_arms.mdl",   // 6: CLASS_MEDIC
	"models/rooster_fortress/c_models/c_engineer_arms.mdl",// 7: CLASS_ENGINEER
	"models/rooster_fortress/c_models/c_demo_arms.mdl",    // 8: CLASS_DEMOMAN
	"models/rooster_fortress/c_models/c_spy_arms.mdl"      // 9: CLASS_SPY
};

const char* ViewModel_ArmsModelForClass(int playerClass)
{
	if (playerClass < VIEWMODEL_FIRST_CLASS || playerClass > VIEWMODEL_LAST_CLASS)
		return nullptr;

	return g_ArmsModelsByClass[playerClass];
}