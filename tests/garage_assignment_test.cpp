#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../mod/native/src/guarded_copy.hpp"
#include "../mod/native/src/garage_assignment.hpp"
void ptr(void *p,uintptr_t v){memcpy(p,&v,8);}
int main(){
 uint8_t module[0x60]{},roster[0x1e8]{},records[4][0x98]{},info[4][0x30]{};
 ptr(module+0x50,reinterpret_cast<uintptr_t>(roster));
 ptr(roster+0x1d8,reinterpret_cast<uintptr_t>(records));ptr(roster+0x1e0,reinterpret_cast<uintptr_t>(records)+sizeof records);
 uint8_t ids[]={19,17,0,15};
 for(int i=0;i<4;++i){records[i][0x82]=ids[i];ptr(records[i]+0x88,reinterpret_cast<uintptr_t>(info[i]));unsigned slot=i<2?7:5;memcpy(info[i]+0x28,&slot,4);}
 auto a=current_garage_assignment(reinterpret_cast<uintptr_t>(module),19,guarded_copy);
 assert(a.slot==7&&a.teammate==17); // pitstop slot5 need not be spawn slot7
 unsigned slot=2;memcpy(info[0]+0x28,&slot,4);memcpy(info[1]+0x28,&slot,4);
 a=current_garage_assignment(reinterpret_cast<uintptr_t>(module),19,guarded_copy);assert(a.slot==2); // fresh session assignment
 records[1][0x82]=19;assert(current_garage_assignment(reinterpret_cast<uintptr_t>(module),19,guarded_copy).slot==-1);
 records[1][0x82]=17;ptr(records[1]+0x88,1);assert(current_garage_assignment(reinterpret_cast<uintptr_t>(module),19,guarded_copy).slot==-1);
 ptr(roster+0x1e0,reinterpret_cast<uintptr_t>(records)+1);assert(current_garage_assignment(reinterpret_cast<uintptr_t>(module),19,guarded_copy).slot==-1);
 puts("PASS: current spawn assignment, separate slot spaces, changed session mapping, duplicate IDs and unreadable/malformed data");
}
