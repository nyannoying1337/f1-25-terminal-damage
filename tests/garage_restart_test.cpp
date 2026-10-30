#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>
namespace {
void log_sink(const char*,...){}
unsigned destroyed,freed,restarted,hud_updates;
uint32_t hud_masks[2]{};
uintptr_t expected,expected_allocator;
void destroy(void *p){assert(reinterpret_cast<uintptr_t>(p)==expected);++destroyed;}
void release(uintptr_t a,uintptr_t p){assert(a==expected_allocator && p==expected);++freed;}
template<class T> void put(uint8_t *p,size_t at,T value){memcpy(p+at,&value,sizeof value);}
}
int main(){
 SwoftyApi api{};api.log=log_sink;swofty::api=&api;
 uint8_t module[0x180]{},allocator[8]{},vt[0x40]{},crew[8]{},replacement[8]{};
 put(module,0x18,reinterpret_cast<uintptr_t>(allocator));put(allocator,0,reinterpret_cast<uintptr_t>(vt));
 put(vt,0x38,reinterpret_cast<uintptr_t>(&release));
 auto base=reinterpret_cast<uintptr_t>(module);expected=reinterpret_cast<uintptr_t>(crew);expected_allocator=reinterpret_cast<uintptr_t>(allocator);
 hud_visibility_manager=+[]()->void*{return hud_masks;};
 update_hud_visibility=+[](void *p){assert(p==hud_masks);++hud_updates;};
 hud_masks[0]=4|32;hud_masks[1]=4|16;
 release_push_hud_hide(false);assert(hud_masks[0]==36&&hud_updates==0);
 release_push_hud_hide(true);assert(hud_masks[0]==32&&hud_masks[1]==20&&hud_updates==1);
 release_push_hud_hide(true);assert(hud_updates==1); // no repeated refresh
 hud_masks[0]=4;
 reset_session();player_vehicle=3;
 put(module,0x68+3*8,expected);owned_crew_module=base;owned_crew_instance=expected;owned_crew_vehicle=3;destroy_crew=destroy;
 player_broken=true;pushing=true;retire_requested=true;player_pit=8;terminal_mask=0x18;
 crew_seq_t=0.1;crew_msg_in=30;pending_crew=2;retire_in=40;
 retired_engine_off_vehicle=3;engine_off_vehicle=3;pit_clearance_hold_vehicle=3;
 void *module_ptr=module;crew_module=&module_ptr;
 orig_damage_session_restart=+[](void *m){uintptr_t p=1;guarded_read(reinterpret_cast<uintptr_t>(m)+0x68+3*8,p);assert(p==0);assert(!player_broken&&!pushing&&crew_seq_t==-1&&crew_msg_in==-1&&pending_crew==-1);++restarted;};
 on_damage_session_restart(module);
 assert(destroyed==1&&freed==1&&restarted==1&&!owned_crew_instance&&retire_in==-1&&engine_off_vehicle==-1);
 assert(hud_masks[0]==0&&hud_masks[1]==20&&hud_updates==2);
 // General lifecycle callback must do the same cleanup without calling the
 // garage-only restart helper (that helper never ran in Nick's race restart).
 put(module,0x68+3*8,expected);owned_crew_module=base;owned_crew_instance=expected;owned_crew_vehicle=3;
 player_broken=true;pushing=true;crew_msg_in=30;crew_seq_t=0.1;retire_in=40;
 orig_damage_session_restart=+[](void *self){assert(self==reinterpret_cast<void *>(123));assert(!player_broken&&!pushing&&crew_msg_in==-1&&crew_seq_t==-1&&retire_in==-1);++restarted;};
 on_damage_session_restart(reinterpret_cast<void *>(123));
 assert(destroyed==2&&freed==2&&restarted==2&&!owned_crew_instance);
 orig_damage_session_restart=+[](void*){++restarted;};
 // Repeated restart cannot double free; foreign/replaced crews are left to stock.
 on_damage_session_restart(module);assert(destroyed==2&&freed==2&&restarted==3);
 put(module,0x68+3*8,reinterpret_cast<uintptr_t>(replacement));owned_crew_module=base;owned_crew_instance=expected;owned_crew_vehicle=3;
 assert(remove_owned_race_crew(module));assert(destroyed==2&&!owned_crew_instance);
 uintptr_t p=0;guarded_read(base+0x68+3*8,p);assert(p==reinterpret_cast<uintptr_t>(replacement));
 // No validated allocator: do not detach/destroy an instance that cannot be freed.
 put(module,0x68+3*8,expected);put(module,0x18,uintptr_t(0));owned_crew_module=base;owned_crew_instance=expected;owned_crew_vehicle=3;
 assert(!remove_owned_race_crew(module));guarded_read(base+0x68+3*8,p);assert(p==expected&&destroyed==2);
 // Controller flag SET and CLEAR alone cannot erase a crash or audio latch.
 reset_session();player_vehicle=3;player_broken=true;terminal_mask=0x18;retired_engine_off_vehicle=3;
 orig_set_controller=+[](void*,uint64_t,uint32_t,int32_t)->uint64_t{return 0;};
 on_set_controller(nullptr,3,1,10);on_set_controller(nullptr,3,0,10);on_set_controller(nullptr,3,0,3);
 assert(player_broken&&terminal_mask==0x18&&retired_engine_off_vehicle==3);
 puts("PASS: push HUD reason released with other/secondary reasons preserved; owned crew removed before stock restart; foreign/replaced crews preserved; no double free or flag-only reset");
}
