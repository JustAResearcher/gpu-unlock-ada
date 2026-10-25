/* SPDX-License-Identifier: GPL-2.0-only */
/* Real UefiMain with simulated firmware/ownership effects. Actual asset SHA
 * validation and current Windows SHA run before each simulated release. The
 * ROM/cold-profile boundary remains the core suite's separately tested seam. */
#include "../app.c"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
GU_PLATFORM Gu;GU_ASSET GuAssets[5];
#include "boot4070s_fixtures.h"
enum MUTATION {NONE,FIRST_BYTE,LAST_BYTE,BAD_NAME,BAD_SIZE,MISSING_ASSET,WINDOWS_HASH,WINDOWS_SIZE};
enum EXTRA {NORMAL,SECURE_ON,RELEASE_UNPROVEN,COLD_REFUSED,UNSAFE_RETURN};
struct TEST {const char *name;unsigned fail,fault;BOOLEAN unlock;enum EXTRA extra;int asset;enum MUTATION mutation;};
#define T(n,f) {n,f,0,TRUE,NORMAL,-1,NONE}
#define A(n,m) {"app-booter-" n,0,0,TRUE,NORMAL,0,m},{"app-v67-" n,0,0,TRUE,NORMAL,1,m}, \
 {"app-gsp-" n,0,0,TRUE,NORMAL,2,m},{"app-boot-" n,0,0,TRUE,NORMAL,3,m},{"app-desc-" n,0,0,TRUE,NORMAL,4,m}
static const struct TEST Tests[]={
 {"stock-default",0,0,FALSE,NORMAL,-1,NONE},
 T("open-refused",1),T("asset-load-refused",2),T("rom-profile-refused",3),T("windows-stage-refused",4),
 T("choice-refused",5),T("ownership-preflight-refused",6),T("release-failed-halts",7),T("shared-load-failed-halts",8),
 T("transaction-failed-halts",9),T("finish-failed-halts",10),T("close-failed-halts",11),T("final-check-failed-halts",12),
 {"secureboot-refused",0,0,TRUE,SECURE_ON,-1,NONE},T("u-once-correct-device-returned-windows-halts",0),
 {"release-not-proven-halts",0,0,TRUE,RELEASE_UNPROVEN,-1,NONE},
 {"cold-entry-refused-halts",0,0,TRUE,COLD_REFUSED,-1,NONE},{"unsafe-result-halts",0,0,TRUE,UNSAFE_RETURN,-1,NONE},
 {"asset-log-fault",0,2,TRUE,NORMAL,-1,NONE},{"rom-log-fault",0,3,TRUE,NORMAL,-1,NONE},
 {"windows-log-fault",0,4,TRUE,NORMAL,-1,NONE},{"choice-log-fault",0,5,TRUE,NORMAL,-1,NONE},
 {"preflight-log-fault",0,6,TRUE,NORMAL,-1,NONE},{"release-log-fault-halts",0,7,TRUE,NORMAL,-1,NONE},
 {"transaction-log-fault-halts",0,9,TRUE,NORMAL,-1,NONE},{"finish-log-fault-halts",0,10,TRUE,NORMAL,-1,NONE},
 A("hash-first",FIRST_BYTE),A("hash-last",LAST_BYTE),A("wrong-name",BAD_NAME),A("wrong-size",BAD_SIZE),A("missing",MISSING_ASSET),
 {"current-windows-last-byte-rejected",0,0,TRUE,NORMAL,-1,WINDOWS_HASH},
 {"current-windows-size-rejected",0,0,TRUE,NORMAL,-1,WINDOWS_SIZE}
};
#undef A
#undef T
static const struct TEST *Current;static jmp_buf Escape;
static unsigned Checks,Failures,Calls,Releases,SharedLoads,Transactions,SharedFrees,Finishes,WindowsCalls,Closes,Choices,Halts,ClearMasters,ResetCalls,VariableWrites;
static BOOLEAN Mutated,Silenced,AssetsVerified,WindowsVerified;
static struct pci_dev *CoreDevice;
static EFI_STATUS Returned;
static EFI_PCI_IO_PROTOCOL Io;static EFI_BOOT_SERVICES Bs;static EFI_RUNTIME_SERVICES Rs;static EFI_SYSTEM_TABLE St;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"SEMANTIC_FAIL case=%s line=%d %s\n",Current->name,__LINE__,#x);longjmp(Escape,2);}}while(0)
#define BOOT4070S_APP 1
#include "boot4070s_profile_traps.h"
static EFI_STATUS Tick(unsigned n){CHECK(++Calls==n);if(Current->fault==n)Gu.fault=TRUE;return Current->fail==n?EFI_DEVICE_ERROR:EFI_SUCCESS;}
VOID GuInit(EFI_BOOT_SERVICES *b,EFI_PCI_IO_PROTOCOL *p){memset(&Gu,0,sizeof(Gu));Gu.bs=b;Gu.pci=p;}
VOID GuLog(const char *f,...){(void)f;}
VOID GuLogText(const char *f){(void)f;}
BOOLEAN GuCheckConfig(const UINT32 c[64],UINTN s,UINTN b,UINTN d,UINTN f,GU_CAPS *caps){
 CHECK(c[0]==GU_BOARD_ID&&!s&&b==1&&!d&&!f);*caps=(GU_CAPS){0x60,0x78,0x68,0,2};return TRUE;
}
static EFI_STATUS EFIAPI GetVariable(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *n,VOID *v){
 (void)name;(void)guid;*attrs=0;*n=1;*(UINT8 *)v=Current->extra==SECURE_ON;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetVariable(CHAR16 *name,EFI_GUID *guid,UINT32 attrs,UINTN n,VOID *v){
 (void)name;(void)guid;(void)attrs;(void)n;(void)v;VariableWrites++;CHECK(0);return EFI_WRITE_PROTECTED;
}
static VOID EFIAPI Reset(EFI_RESET_TYPE type,EFI_STATUS status,UINTN size,VOID *data){
 (void)type;(void)status;(void)size;(void)data;ResetCalls++;CHECK(0);
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out){
 CHECK(kind==ByProtocol&&!memcmp(guid,&PciGuid,16)&&!key);*n=1;*out=malloc(sizeof(**out));CHECK(*out);(*out)[0]=(EFI_HANDLE)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **p){CHECK(h==(EFI_HANDLE)2&&!memcmp(guid,&PciGuid,16));*p=&Io;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *out){
 CHECK(p==&Io&&w==EfiPciIoWidthUint32&&!off&&(n==1||n==64));memset(out,0,n*4);((UINT32 *)out)[0]=GU_BOARD_ID;if(n==64)((UINT32 *)out)[2]=0x030000a1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f){CHECK(p==&Io);*s=*d=*f=0;*b=1;return EFI_SUCCESS;}
EFI_STATUS GuBootOpen(EFI_HANDLE i,EFI_SYSTEM_TABLE *t){CHECK(i==(EFI_HANDLE)1&&t==&St);return Tick(1);}
EFI_STATUS GuBootLoadAssets(VOID){
 EFI_STATUS s=Tick(2);if(s!=EFI_SUCCESS)return s;BootBindAssets();
 int i=Current->asset;UINTN changed=0;
 if(i>=0){
  if(Current->mutation==FIRST_BYTE||Current->mutation==LAST_BYTE){changed=Current->mutation==LAST_BYTE?BootSizes[i]-1:0;BootBytes[i][changed]^=1;}
  if(Current->mutation==BAD_NAME)GuAssets[i].name="not-the-pinned-asset";
  if(Current->mutation==BAD_SIZE)GuAssets[i].firmware.size++;
  if(Current->mutation==MISSING_ASSET)GuAssets[i].firmware.data=NULL;
 }
 AssetsVerified=GuAd104AssetsValid();
 if(i>=0&&(Current->mutation==FIRST_BYTE||Current->mutation==LAST_BYTE))BootBytes[i][changed]^=1;
 return AssetsVerified?EFI_SUCCESS:EFI_CRC_ERROR;
}
EFI_STATUS GuAd104Prepare(VOID){
 CHECK(AssetsVerified);EFI_STATUS s=Tick(3);if(s==EFI_SUCCESS)Gu.profile_verified=Gu.prom_verified=TRUE;return s;
}
EFI_STATUS GuBootLoadWindows(VOID){
 CHECK(AssetsVerified&&Gu.profile_verified&&Gu.prom_verified);EFI_STATUS s=Tick(4);if(s!=EFI_SUCCESS)return s;
 if(Current->mutation==WINDOWS_HASH)BootBytes[BOOT_WINDOWS][BootSizes[BOOT_WINDOWS]-1]^=1;
 UINTN size=BootSizes[BOOT_WINDOWS]-(Current->mutation==WINDOWS_SIZE?1:0);
 WindowsVerified=size==BootSizes[BOOT_WINDOWS]&&GuHashMatches(BootBytes[BOOT_WINDOWS],size,BootWindowsHash);
 if(Current->mutation==WINDOWS_HASH)BootBytes[BOOT_WINDOWS][BootSizes[BOOT_WINDOWS]-1]^=1;
 return WindowsVerified?EFI_SUCCESS:EFI_CRC_ERROR;
}
EFI_STATUS GuBootChoose(BOOLEAN *u){
 CHECK(AssetsVerified&&Gu.prom_verified&&Gu.profile_verified&&WindowsVerified);Choices++;
 EFI_STATUS s=Tick(5);*u=Current->unlock;return s;
}
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci){
 CHECK(t==&St&&self==(EFI_HANDLE)1&&pci==(EFI_HANDLE)2&&AssetsVerified&&WindowsVerified&&Gu.profile_verified&&Gu.prom_verified&&Choices==1);return Tick(6);
}
VOID GuBootSay(const CHAR16 *s){CHECK(s&&!Silenced);}
VOID GuBootSilence(VOID){CHECK(Calls==6);Silenced=TRUE;}
EFI_STATUS GuMsiRelease(VOID){CHECK(Silenced&&AssetsVerified&&WindowsVerified&&Choices==1);Releases++;Mutated=TRUE;Gu.owned=TRUE;return Tick(7);}
BOOLEAN GuMsiReleased(VOID){return Mutated&&Gu.owned&&Current->extra!=RELEASE_UNPROVEN;}
BOOLEAN GuAd104ColdEntry(VOID){return Current->extra!=COLD_REFUSED;}
int gpu_unlock_610_load_shared(struct pci_dev *d,struct gpu_unlock_610_shared *shared){
 SharedLoads++;CHECK(d&&d->vendor==0x10de&&d->device==0x2783&&Gu.owned&&Gu.armed&&Gu.display_ok);
 CHECK(shared&&d->current_state==PCI_D0&&d->pm_cap==Gu.pm_cap);CoreDevice=d;return Tick(8)==EFI_SUCCESS?0:-EIO;
}
int gpu_unlock_610_unlock(struct pci_dev *d,const struct gpu_unlock_610_shared *s){
 CHECK(d==CoreDevice&&d->device==0x2783&&s&&SharedLoads==1);Transactions++;Gu.started=TRUE;Gu.quiesced=TRUE;Gu.flrs=1;return Tick(9)==EFI_SUCCESS?0:-EIO;
}
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *s){CHECK(s&&SharedLoads==1);SharedFrees++;}
BOOLEAN GuSafeToReturn(VOID){return Current->extra!=UNSAFE_RETURN;}
EFI_STATUS GuMsiFinish(VOID){CHECK(Transactions==1&&Gu.quiesced);Finishes++;EFI_STATUS s=Tick(10);if(s==EFI_SUCCESS)Gu.owned=FALSE;return s;}
EFI_STATUS GuClearMaster(VOID){CHECK(Mutated);ClearMasters++;return EFI_SUCCESS;}
BOOLEAN GuMsiMutated(VOID){return Mutated;}
BOOLEAN GuMsiReadyForWindows(VOID){CHECK(Closes==1&&!Gu.owned&&Gu.quiesced);return Current->fail!=12;}
EFI_STATUS GuBootClose(VOID){Closes++;return Current->fail==11?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS GuBootWindows(VOID){
 CHECK(WindowsVerified&&Closes==1);WindowsCalls++;
 if(Current->unlock)CHECK(Calls==10&&!Gu.owned&&Transactions==1&&Finishes==1);
 else CHECK(Calls==5&&!Releases&&!Transactions&&!Finishes);
 return EFI_ABORTED;
}
VOID GuBootHalt(VOID){CHECK(Mutated);Halts++;longjmp(Escape,1);}
static void Setup(void){
 Calls=Releases=SharedLoads=Transactions=SharedFrees=Finishes=WindowsCalls=Closes=Choices=Halts=ClearMasters=ResetCalls=VariableWrites=0;
 Mutated=Silenced=AssetsVerified=WindowsVerified=FALSE;CoreDevice=NULL;Returned=EFI_SUCCESS;
 memset(&Gu,0,sizeof(Gu));memset(GuAssets,0,sizeof(GuAssets));memset(&Bs,0,sizeof(Bs));memset(&Rs,0,sizeof(Rs));memset(&St,0,sizeof(St));memset(&Io,0,sizeof(Io));
 Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.FreePool=Free;Rs.GetVariable=GetVariable;Rs.SetVariable=SetVariable;Rs.ResetSystem=Reset;
 St.BootServices=&Bs;St.RuntimeServices=&Rs;Io.Pci.Read=Read;Io.GetLocation=Location;
}
static void Verify(BOOLEAN halted){
 BOOLEAN good=!Current->fail&&!Current->fault&&Current->extra==NORMAL&&Current->mutation==NONE;
 if(good){
  CHECK(WindowsCalls==1&&Closes==1);
  if(Current->unlock)CHECK(halted&&Halts==1&&Releases==1&&SharedLoads==1&&Transactions==1&&SharedFrees==1&&Finishes==1&&!ClearMasters);
  else CHECK(!halted&&!Mutated&&!Releases&&!SharedLoads&&!Transactions&&!Finishes);
 }else {
  CHECK(!WindowsCalls);
  if(Mutated){CHECK(halted&&Halts==1);if(Gu.started)CHECK(ClearMasters==1);}
  else CHECK(!halted&&!Halts&&!Releases&&!SharedLoads&&!Transactions&&Returned!=EFI_SUCCESS);
 }
 if(Current->asset>=0){CHECK(Calls==2&&!AssetsVerified&&!Choices&&!Releases&&!SharedLoads);}
 if(Current->mutation==WINDOWS_HASH||Current->mutation==WINDOWS_SIZE)CHECK(Calls==4&&!WindowsVerified&&!Choices&&!Releases&&!SharedLoads);
 if(Current->extra==SECURE_ON)CHECK(!Calls&&!Closes);
 if(Current->fail==7)CHECK(Mutated&&GuMsiReleased()&&Halts==1&&!SharedLoads&&!Transactions);
 if(Current->fail&&Current->fail<=7)CHECK(!SharedLoads&&!Transactions);
 CHECK(Releases<=1&&SharedLoads<=1&&Transactions<=1&&Finishes<=1&&WindowsCalls<=1&&Choices<=1&&Halts<=1);
 CHECK(!ResetCalls&&!VariableWrites);
}
int main(int argc,char **argv){
 BootLoadFixtures(argc,argv);volatile unsigned ran=0;
 for(volatile unsigned i=0;i<sizeof(Tests)/sizeof(Tests[0]);i++){
  Current=&Tests[i];if(argc==4&&strcmp(argv[3],Current->name))continue;ran++;Setup();
  int jump=setjmp(Escape);if(!jump)Returned=UefiMain((EFI_HANDLE)1,&St);
  if(jump!=2){Verify(jump==1);printf("PASS %s\n",Current->name);}else Failures++;
 }
 BootFreeFixtures();if(!ran)BootFixtureError("case-name","no matching test");
 printf("boot4070s app flow: %u cases, %u checks, %u semantic failures; actual asset/Windows hashes, simulated ownership/core.\n",ran,Checks,Failures);
 return Failures?1:0;
}
