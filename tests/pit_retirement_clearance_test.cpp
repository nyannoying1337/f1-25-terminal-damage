#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
namespace {
bool option=true; unsigned setters=0;
void log_sink(const char *, ...) {}
int watch(const char *,SwoftySettingFn,void *){return 0;}
int setting(const char *name,uint32_t type,SwoftySettingValue *out){
 out->type=type;
 if(!strcmp(name,"keep_control")||!strcmp(name,"retire_in_pit_box")||!strcmp(name,"garage_push")){out->toggle=true;return 1;}
 if(!strcmp(name,"pit_retire_clearance")){out->toggle=option;return 1;}return 0;
}
void visible(void *self,bool on){++setters;unsigned flags=0;memcpy(&flags,static_cast<uint8_t *>(self)+0x40,4);flags=(flags&~0x20u)|(on?0x20:0);memcpy(static_cast<uint8_t *>(self)+0x40,&flags,4);}
void unknown(void *,bool){assert(false);}
bool shown(uint8_t *p){unsigned flags;memcpy(&flags,p+0x40,4);return flags&0x20;}
void put(void *p,uintptr_t v){memcpy(p,&v,8);}
}
int main(){
 SwoftyApi api{};api.version=4;api.log=log_sink;api.setting=setting;api.watch_setting=watch;swofty::api=&api;
 uint8_t instance[0x460]{},proxy[0xb0]{},data[0x110]{},actors[4][0x250]{};
 uintptr_t vt[0x24]{},other_vt[0x24]{},list[4]{};
 model_local_entry=reinterpret_cast<uintptr_t>(&visible);orig_model_local=visible;
 vt[0x98/8]=model_local_entry;other_vt[0x98/8]=reinterpret_cast<uintptr_t>(&unknown);
 for(unsigned i=0;i<4;++i){put(actors[i],reinterpret_cast<uintptr_t>(i==2?other_vt:vt));unsigned id=i+10;memcpy(actors[i]+0x248,&id,4);visible(actors[i],i!=1);list[i]=reinterpret_cast<uintptr_t>(actors[i]);}
 // Scene0 has two supported actors and one unknown virtual implementation.
 uint64_t count=3;memcpy(data+0x100,&count,8);put(proxy+0xa0,reinterpret_cast<uintptr_t>(data));put(proxy+0x78,reinterpret_cast<uintptr_t>(list));
 put(instance+0xe0,reinterpret_cast<uintptr_t>(proxy));
 // Car proxy scene21 must not be enumerated.
 put(instance+0x188,reinterpret_cast<uintptr_t>(proxy));
 unsigned slot=3,session=15;memcpy(instance+0x60,&slot,4);repair_session_kind=reinterpret_cast<uintptr_t>(&session);
 for(auto &g:pit_slot_of)g=-1;pit_slot_of[7]=pit_slot_of[12]=3;player_vehicle=7;player_pit=2;player_broken=true;session_ended=false;retire_in=-1;
 instance[0xc0]=7;instance[0xc1]=12;
 assert(current_pit_models(reinterpret_cast<uintptr_t>(instance)).size()==2);
 // Unknown teammate blocks. Proxy cache450=0 does not block the MODEL API.
 apply_pit_clearance(instance);assert(shown(actors[0])&&pit_models.empty());
 observed_pit_state[12]=2;apply_pit_clearance(instance);assert(!shown(actors[0])&&!shown(actors[1])&&shown(actors[2]));assert(pit_models.size()==2);
 unsigned before=setters;apply_pit_clearance(instance);assert(setters==before); // no repeated hide calls
 // Normal game requests remain recorded while the retirement override stays hidden.
 on_model_local(actors[1],true);assert(!shown(actors[1]));
 on_model_local(actors[0],false);assert(!shown(actors[0]));
 player_broken=false;apply_pit_clearance(instance);assert(pit_models.empty());assert(!shown(actors[0])&&shown(actors[1]));
 // Teammate entering pit lane releases the override with latest desired states.
 visible(actors[0],true);player_broken=true;apply_pit_clearance(instance);assert(!shown(actors[0]));
 observed_pit_state[12]=3;apply_pit_clearance(instance);assert(shown(actors[0])&&pit_models.empty());
 observed_pit_state[12]=2;session=5;apply_pit_clearance(instance);assert(pit_models.empty());
 session=15;option=false;apply_pit_clearance(instance);assert(pit_models.empty());option=true;
 // Double stack: do not release crew-ready while teammate occupies the shared box.
 uint8_t lane[0x50]{},manager[0xc00]{},pitting[0x110]{};
 put(lane+0x48,reinterpret_cast<uintptr_t>(manager));put(manager+0x938+7*16,reinterpret_cast<uintptr_t>(pitting));
 pit_lane_system=reinterpret_cast<uintptr_t>(lane);player_pit=3;pushing=true;prepare_at_box=true;
 observed_pit_state[12]=5;apply_pit_clearance(instance);release_retirement_box_entry();assert(pitting[0x104]==0);
 observed_pit_state[12]=15;apply_pit_clearance(instance);release_retirement_box_entry();assert(pitting[0x104]==0); // still in lane
 observed_pit_state[12]=2;apply_pit_clearance(instance);release_retirement_box_entry();assert(pitting[0x104]==1);
 option=false;pitting[0x104]=0;assert(release_retirement_box_entry()&&pitting[0x104]==1);
 observed_pit_state[12]=5;pitting[0x104]=0;assert(!release_retirement_box_entry()&&pitting[0x104]==0);
 option=true;observed_pit_state[12]=2;
 pitting[0x104]=0;player_pit=2;apply_pit_clearance(instance);release_retirement_box_entry();assert(pitting[0x104]==0); // not at box approach yet
 player_pit=3;prepare_at_box=false;apply_pit_clearance(instance);release_retirement_box_entry();assert(pitting[0x104]==0);
 player_pit=2;prepare_at_box=true;
 // Completed push: keep tyre crew hidden at InGarage and through DNF cleanup.
 player_pit=8;apply_pit_clearance(instance);assert(pit_clearance_hold_vehicle==7);
 player_pit=9;apply_pit_clearance(instance);assert(!shown(actors[0])&&!pit_models.empty());
 reset_car_state(true);assert(pit_clearance_hold_vehicle==7&&pit_clearance_requested());
 on_model_local(actors[0],true);assert(!shown(actors[0])); // normal re-seat request stays suppressed
 // A teammate can still use the shared box after our retirement.
 observed_pit_state[12]=3;apply_pit_clearance(instance);assert(shown(actors[0])&&pit_models.empty());
 observed_pit_state[12]=2;apply_pit_clearance(instance);assert(!pit_models.empty());
 // Restart preparation releases the latch before the normal wear reset.
 orig_set_controller=+[](void *,uint64_t,uint32_t,int32_t)->uint64_t{return 42;};
 assert(on_set_controller(nullptr,7,1,10)==42);assert(pit_clearance_hold_vehicle==7);
 reset_for_session_restart(nullptr);
 apply_pit_clearance(instance);assert(pit_models.empty()&&shown(actors[0]));
 // Flashback/live repair reset also clears the latch.
 pit_slot_of[7]=pit_slot_of[12]=3;observed_pit_state[12]=2;player_broken=true;player_pit=8;apply_pit_clearance(instance);assert(pit_clearance_hold_vehicle==7);
 reset_car_state(false);apply_pit_clearance(instance);assert(pit_clearance_hold_vehicle==-1&&pit_models.empty());
 player_broken=true;player_pit=2;retire_in=-1;apply_pit_clearance(instance);
 // Replaced/removed models are forgotten, never invoked through saved pointers.
 apply_pit_clearance(instance);assert(!pit_models.empty());count=0;memcpy(data+0x100,&count,8);player_broken=false;before=setters;apply_pit_clearance(instance);assert(pit_models.empty()&&setters==before);
 // Live race mapping still requires distinct IDs, player membership and a bounded slot.
 pit_slot_of[7]=pit_slot_of[12]=-1;learn_live_pit_slot(reinterpret_cast<uintptr_t>(instance));assert(pit_slot_of[7]==3&&pit_slot_of[12]==3);
 pit_slot_of[7]=-1;instance[0xc1]=7;learn_live_pit_slot(reinterpret_cast<uintptr_t>(instance));assert(pit_slot_of[7]==-1);
 // Untargeted visibility calls forward unchanged without probing unrelated objects.
 on_model_local(actors[3],false);assert(!shown(actors[3]));
 puts("PASS: real model visibility, proxy-cache independence, latest desired restoration, teammate/session/toggle guards, unknown setter and stale model exclusion");
}
