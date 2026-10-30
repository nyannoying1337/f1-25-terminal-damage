// SPDX-License-Identifier: LicenseRef-Terminal-Damage-Attribution-1.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../mod/native/src/terminal_damage_probe.cpp"
#include <cassert>

namespace {
unsigned notifications=0;
TdPlayerState last{};
uint64_t self_token=0;
void discard_log(const char *, ...) {}
uint64_t controller(void *,uint64_t,uint32_t,int32_t) {return 44;}
void changed(const TdPlayerState *state,void *user) {
    assert(user==&notifications);
    ++notifications;last=*state;
    TdPlayerState current{};
    assert(td_state::api.get_state(&current,sizeof current));
    assert(current.state==state->state); // reentrant reads don't deadlock
}
void remove_self(const TdPlayerState *,void *) {td_state::api.unsubscribe(self_token);}
void tick() {publish_player_state();td_state::service.dispatch();}
}
int main()
{
    SwoftyApi sdk{};sdk.log=discard_log;swofty::api=&sdk;
    orig_set_controller=controller;
    auto api=TerminalDamageGetApi(1);
    assert(api && api->size==sizeof(TdApi));
    assert(!TerminalDamageGetApi(2));
    TdPlayerState state{};
    assert(!api->get_state(nullptr,sizeof state));
    assert(!api->get_state(&state,sizeof state-1));
    assert(api->get_state(&state,sizeof state) && state.state==TD_UNAVAILABLE && state.vehicle_id==-1);
    assert(!api->subscribe(nullptr,nullptr));
    auto token=api->subscribe(changed,&notifications);assert(token);
    tick();assert(notifications==1 && last.state==TD_UNAVAILABLE);
    tick();assert(notifications==1);
    player_vehicle=3;session_ended=false;reset_car_state(false);tick();
    assert(last.state==TD_RUNNING && !last.player_out && last.vehicle_id==3);
    player_broken=true;terminal_mask=8;tick();
    assert(last.state==TD_LIMPING && last.damage_mask==8 && !last.player_out);
    retire_requested=true;tick();assert(last.state==TD_RETIRING && !last.player_out);
    player_pit=9;tick();assert(last.state==TD_IN_GARAGE && !last.player_out);
    assert(on_set_controller(nullptr,3,1,4)==44);tick();
    assert(last.state==TD_RETIRED && last.player_out && last.vehicle_id==3);
    reset_car_state(true);tick();assert(last.state==TD_RETIRED);
    reset_session(true);session_ended=true;tick();
    assert(last.state==TD_RETIRED && last.vehicle_id==3);
    reset_session();session_ended=false;player_vehicle=3;tick();
    assert(last.state==TD_RUNNING && !last.player_out); // actual restart reset
    player_broken=true;terminal_mask=8;tick();
    reset_car_state(false);tick();assert(last.state==TD_RUNNING && !last.damage_mask);
    // A retained post-damage rewind stays damaged, not falsely running.
    player_broken=true;terminal_mask=8;tick();assert(last.state==TD_LIMPING);
    reset_car_state(false);tick();
    api->unsubscribe(token);unsigned before=notifications;
    terminal_mask=2;tick();assert(notifications==before);
    self_token=api->subscribe(remove_self,nullptr);tick();tick(); // no lock held in callback
    assert(api->get_state(&state,sizeof state) && state.damage_mask==2);
    puts("PASS: state ABI, notifications, retirement confirmation, DNF retention, reset and unsubscribe");
}
