#pragma once
#include <swofty/game/base.hpp>

namespace swofty::game {

struct GameWeatherSystem {
    uint8_t _pad_0x0[0x1d98];
    TimeOfDaySystem *tod; // L2 experimental
    uint8_t _pad_0x1da0[0x8];
    TimeOfDayOwner *tod_owner; // L2 experimental
    uint8_t _pad_0x1db0[0x152];
    uint8_t weekend_valid; // L2 experimental
    uint8_t _pad_0x1f03[0x1];
    uint32_t season_index; // L2 experimental
    uint32_t weekend_index; // L2 experimental
    uint8_t _pad_0x1f0c[0x4];
    uint32_t base_seed; // L2 experimental
    uint32_t forced_seed; // L2 experimental
    uint32_t weekend; // L2 experimental
    uint8_t _pad_0x1f1c[0x101e];
    uint8_t dataset_dynamic[8]; // L2 experimental
};
static_assert(offsetof(GameWeatherSystem, tod) == 0x1d98);
static_assert(offsetof(GameWeatherSystem, tod_owner) == 0x1da8);
static_assert(offsetof(GameWeatherSystem, weekend_valid) == 0x1f02);
static_assert(offsetof(GameWeatherSystem, season_index) == 0x1f04);
static_assert(offsetof(GameWeatherSystem, weekend_index) == 0x1f08);
static_assert(offsetof(GameWeatherSystem, base_seed) == 0x1f10);
static_assert(offsetof(GameWeatherSystem, forced_seed) == 0x1f14);
static_assert(offsetof(GameWeatherSystem, weekend) == 0x1f18);
static_assert(offsetof(GameWeatherSystem, dataset_dynamic) == 0x2f3a);

struct TimeOfDayClock {
    uint8_t _pad_0x0[0x68];
    double time_a; // L2 experimental
    double time_b; // L2 experimental
    uint8_t _pad_0x78[0x8];
    float time_scale; // L2 experimental
    float length; // L2 experimental
    uint8_t _pad_0x88[0x200];
};
static_assert(offsetof(TimeOfDayClock, time_a) == 0x68);
static_assert(offsetof(TimeOfDayClock, time_b) == 0x70);
static_assert(offsetof(TimeOfDayClock, time_scale) == 0x80);
static_assert(offsetof(TimeOfDayClock, length) == 0x84);
static_assert(sizeof(TimeOfDayClock) == 0x288);

struct TimeOfDayOwner {
    uint8_t _pad_0x0[0x2d0];
    TimeOfDaySystem *tod; // L2 experimental
};
static_assert(offsetof(TimeOfDayOwner, tod) == 0x2d0);

struct WeatherState {
    uint32_t type; // L2 experimental
    float intensity; // L2 experimental
    float end; // L2 experimental
    float transition; // L2 experimental
};
static_assert(offsetof(WeatherState, type) == 0x0);
static_assert(offsetof(WeatherState, intensity) == 0x4);
static_assert(offsetof(WeatherState, end) == 0x8);
static_assert(offsetof(WeatherState, transition) == 0xc);
static_assert(sizeof(WeatherState) == 0x10);

struct WeatherDataset {
    uint8_t _pad_0x0[0xb90];
    WeatherState states[38]; // L2 experimental
    uint32_t state_total; // L2 experimental
    uint8_t _pad_0xdf4[0x4];
    uint32_t state_count; // L2 experimental
    uint8_t _pad_0xdfc[0x4];
};
static_assert(offsetof(WeatherDataset, states) == 0xb90);
static_assert(offsetof(WeatherDataset, state_total) == 0xdf0);
static_assert(offsetof(WeatherDataset, state_count) == 0xdf8);
static_assert(sizeof(WeatherDataset) == 0xe00);

struct WeatherTypeRange {
    float weight; // L2 experimental
    uint8_t _pad_0x4[0x8];
    float intensity_min; // L2 experimental
    float intensity_max; // L2 experimental
};
static_assert(offsetof(WeatherTypeRange, weight) == 0x0);
static_assert(offsetof(WeatherTypeRange, intensity_min) == 0xc);
static_assert(offsetof(WeatherTypeRange, intensity_max) == 0x10);
static_assert(sizeof(WeatherTypeRange) == 0x14);

struct TimeOfDaySystem {
    uint8_t _pad_0x0[0x2c8];
    TimeOfDayClock clocks[10]; // L2 experimental
    uint8_t _pad_0x1c18[0xa3c];
    uint8_t scripted_scenario; // L2 experimental
    uint8_t _pad_0x2655[0x6f];
    uint8_t active_dataset; // L2 experimental
    uint8_t _pad_0x26c5[0x13];
    WeatherDataset datasets[10]; // L2 experimental
    uint8_t _pad_0xb2d8[0x64];
    uint32_t seed; // L2 experimental
    uint8_t _pad_0xb340[0x10];
    float dataset_length[10]; // L2 experimental
    WeatherTypeRange type_ranges[6]; // L2 experimental
    uint8_t _pad_0xb3f0[0x36];
    uint8_t exact_timing; // L2 experimental
    uint8_t _pad_0xb427[0x2];
    uint8_t ready_a; // L2 experimental
    uint8_t ready_b; // L2 experimental
    uint8_t _pad_0xb42b[0x2];
    uint8_t random_override; // L2 experimental
    uint8_t _pad_0xb42e[0x4];
    uint8_t busy; // L2 experimental
    uint8_t _pad_0xb433[0x3];
    uint8_t override_active; // L2 experimental
};
static_assert(offsetof(TimeOfDaySystem, clocks) == 0x2c8);
static_assert(offsetof(TimeOfDaySystem, scripted_scenario) == 0x2654);
static_assert(offsetof(TimeOfDaySystem, active_dataset) == 0x26c4);
static_assert(offsetof(TimeOfDaySystem, datasets) == 0x26d8);
static_assert(offsetof(TimeOfDaySystem, seed) == 0xb33c);
static_assert(offsetof(TimeOfDaySystem, dataset_length) == 0xb350);
static_assert(offsetof(TimeOfDaySystem, type_ranges) == 0xb378);
static_assert(offsetof(TimeOfDaySystem, exact_timing) == 0xb426);
static_assert(offsetof(TimeOfDaySystem, ready_a) == 0xb429);
static_assert(offsetof(TimeOfDaySystem, ready_b) == 0xb42a);
static_assert(offsetof(TimeOfDaySystem, random_override) == 0xb42d);
static_assert(offsetof(TimeOfDaySystem, busy) == 0xb432);
static_assert(offsetof(TimeOfDaySystem, override_active) == 0xb436);

struct WeatherAndTimeOfDayComponent {
    uint8_t _pad_0x0[0x2a0];
    GameWeatherSystem *weather_system; // L2 experimental
    uint8_t _pad_0x2a8[0x1c];
    uint32_t random_seed; // L2 experimental
};
static_assert(offsetof(WeatherAndTimeOfDayComponent, weather_system) == 0x2a0);
static_assert(offsetof(WeatherAndTimeOfDayComponent, random_seed) == 0x2c4);

struct WeatherSessionSettings {
    uint8_t _pad_0x0[0x4];
    uint32_t session_type; // L2 experimental
    uint8_t _pad_0x8[0xb0c];
    uint32_t mode; // L2 experimental
    uint8_t exact_timing; // L2 experimental
};
static_assert(offsetof(WeatherSessionSettings, session_type) == 0x4);
static_assert(offsetof(WeatherSessionSettings, mode) == 0xb14);
static_assert(offsetof(WeatherSessionSettings, exact_timing) == 0xb18);

using Sess_Build_GroupResult_t = void(void *weekend, uint8_t group, void *result);
inline constexpr fn<"Sess_Build_GroupResult", Sess_Build_GroupResult_t> Sess_Build_GroupResult{}; // L2 experimental
using Sess_Find_GroupRowPosition_t = int32_t *(void *weekend, void *result, uint32_t driver_key);
inline constexpr fn<"Sess_Find_GroupRowPosition", Sess_Find_GroupRowPosition_t> Sess_Find_GroupRowPosition{}; // L2 experimental
using Sess_GameWeatherSystem_SetupSession_t = void(void *ws, void *settings, void *config);
inline constexpr fn<"Sess_GameWeatherSystem_SetupSession", Sess_GameWeatherSystem_SetupSession_t> Sess_GameWeatherSystem_SetupSession{}; // L2 experimental, GameWeatherSystem::setupSession.
using Sess_GameWeatherSystem_UpdateRandomSeed_t = void(void *ws, uint32_t seed, uint32_t weekend);
inline constexpr fn<"Sess_GameWeatherSystem_UpdateRandomSeed", Sess_GameWeatherSystem_UpdateRandomSeed_t> Sess_GameWeatherSystem_UpdateRandomSeed{}; // L2 experimental, GameWeatherSystem::updateRandomSeed.
using Sess_Record_GroupTimes_t = void(void *weekend, uint8_t group, void *entries);
inline constexpr fn<"Sess_Record_GroupTimes", Sess_Record_GroupTimes_t> Sess_Record_GroupTimes{}; // L2 experimental
using Sess_Tod_BuildSessionDataset_Thunk_t = void(void *tod_owner, const uint8_t *types, const float *hold_s, const float *trans_s, const uint8_t *random, uint32_t count, float len_s, uint16_t session, uint8_t use_count);
inline constexpr fn<"Sess_Tod_BuildSessionDataset_Thunk", Sess_Tod_BuildSessionDataset_Thunk_t> Sess_Tod_BuildSessionDataset_Thunk{}; // L2 experimental
using Sess_TyreAllocation_OnSessionStart_t = void(void *component);
inline constexpr fn<"Sess_TyreAllocation_OnSessionStart", Sess_TyreAllocation_OnSessionStart_t> Sess_TyreAllocation_OnSessionStart{}; // L2 experimental
using Sess_TyreClass_FromName_t = void *(void *classes, const void *name, uint8_t check_weekend_format);
inline constexpr fn<"Sess_TyreClass_FromName", Sess_TyreClass_FromName_t> Sess_TyreClass_FromName{}; // L2 experimental
using Sess_Tyres_ClosestAvailableCompound_t = uint32_t(void *tyre_rules, uint8_t vehicle, const uint32_t *preference, float wear_scale);
inline constexpr fn<"Sess_Tyres_ClosestAvailableCompound", Sess_Tyres_ClosestAvailableCompound_t> Sess_Tyres_ClosestAvailableCompound{}; // L2 experimental
using Sess_WeatherComponent_SetSystemRandomSeed_t = void(void *component);
inline constexpr fn<"Sess_WeatherComponent_SetSystemRandomSeed", Sess_WeatherComponent_SetSystemRandomSeed_t> Sess_WeatherComponent_SetSystemRandomSeed{}; // L2 experimental, WeatherAndTimeOfDayComponent::setWeatherSystemRandomSeed
using Sess_Weekend_Start_t = void(void *weekend, const void *entrants, bool begin);
inline constexpr fn<"Sess_Weekend_Start", Sess_Weekend_Start_t> Sess_Weekend_Start{}; // L2 experimental, game::championship::Weekend::start(const
inline constexpr data<"g_Sess_TyreRulesActive", uint8_t> g_Sess_TyreRulesActive{}; // L2 experimental
inline constexpr data<"g_Sess_TyreRulesMode", uint8_t> g_Sess_TyreRulesMode{}; // L2 experimental
inline constexpr data<"g_Sess_TyreRulesSessionType", uint32_t> g_Sess_TyreRulesSessionType{}; // L2 experimental
inline constexpr data<"k_Sess_TyreComponentType", void *> k_Sess_TyreComponentType{}; // L2 experimental
inline constexpr call<"site_Sess_AiVehicleRegisterCall"> site_Sess_AiVehicleRegisterCall{}; // L2 experimental

}
