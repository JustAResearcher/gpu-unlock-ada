/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise real UEFI access/lifetime gates with fake EFI protocols. No GPU IO. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../src/app.c"

static unsigned reads,writes,unmaps,freed,pool_frees;
static BOOLEAN fail_read,fail_write,fail_unmap;
static EFI_STATUS read_status,write_status,config_read_status,config_write_status;
static EFI_STATUS alloc_status,map_status,unmap_status,free_status,pool_status,stall_status;
static EFI_STATUS enable_status,disable_status,get_status;
static UINT32 config[64];
static UINT64 attributes;
static unsigned allocations,maps,disables,config_reads;
static BOOLEAN short_map,disable_noop,allocate_null;
static UINT8 backing[8192] __attribute__((aligned(4096)));
static UINT32 value;
static EFI_STATUS EFIAPI mem_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
 UINT8 bar,UINT64 offset,UINTN count,VOID *out) {
 (void)p;(void)w;(void)bar;assert(count==1);reads++;
 if(fail_read)return EFI_DEVICE_ERROR;
 switch(offset) {
 case 0x82381c:*(UINT32*)out=value;break;
 case 0x823820:*(UINT32*)out=8;break;
 case 0x823818:*(UINT32*)out=0;break;
 case 0x823804:*(UINT32*)out=0xffffffff;break;
 default:*(UINT32*)out=0x12345678;
 }
 return read_status;
}
static EFI_STATUS EFIAPI mem_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
 UINT8 bar,UINT64 offset,UINTN count,VOID *data) {
 (void)p;(void)w;(void)bar;(void)offset;(void)count;(void)data;
 writes++;return fail_write?EFI_DEVICE_ERROR:write_status;
}
static EFI_STATUS EFIAPI config_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
 UINT32 offset,UINTN count,VOID *data) {
 (void)p;assert(count==1);config_reads++;memcpy(data,(UINT8*)config+offset,1U<<w);return config_read_status;
}
static EFI_STATUS EFIAPI config_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
 UINT32 offset,UINTN count,VOID *data) {
 (void)p;assert(count==1);memcpy((UINT8*)config+offset,data,1U<<w);writes++;return config_write_status;
}
static EFI_STATUS EFIAPI unmap(EFI_PCI_IO_PROTOCOL *p,VOID *mapping) {
 (void)p;(void)mapping;unmaps++;return fail_unmap?EFI_DEVICE_ERROR:unmap_status;
}
static EFI_STATUS EFIAPI free_buffer(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu) {
 (void)p;(void)pages;(void)cpu;freed++;return free_status;
}
static EFI_STATUS EFIAPI allocate_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE type,
 EFI_MEMORY_TYPE memory,UINTN pages,VOID **cpu,UINT64 attrs) {
 (void)p;(void)type;(void)memory;(void)attrs;assert(pages<=2);allocations++;
 *cpu=allocate_null?NULL:backing;return alloc_status;
}
static EFI_STATUS EFIAPI map_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION op,
 VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *dma,VOID **mapping) {
 (void)p;assert(op==EfiPciIoOperationBusMasterCommonBuffer && cpu==backing);maps++;
 if(short_map)*bytes-=4096;*dma=0x800000;*mapping=(VOID*)0x9876000;return map_status;
}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type,UINTN bytes,VOID **p) {
 (void)type;*p=malloc(bytes);assert(*p);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pool(VOID *p) {
 pool_frees++;if(pool_status==EFI_SUCCESS)free(p);return pool_status;
}
static EFI_STATUS EFIAPI stall(UINTN usec) {(void)usec;return stall_status;}
static EFI_STATUS EFIAPI pci_attributes(EFI_PCI_IO_PROTOCOL *p,
 EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 attrs,UINT64 *out) {
 (void)p;
 switch(op) {
 case EfiPciIoAttributeOperationSupported:*out=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;return EFI_SUCCESS;
 case EfiPciIoAttributeOperationGet:*out=attributes;return get_status;
 case EfiPciIoAttributeOperationEnable:
  assert(attrs==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);attributes|=attrs;config[1]|=4;return enable_status;
 case EfiPciIoAttributeOperationDisable:
  assert(attrs==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);disables++;
  if(!disable_noop){attributes&=~attrs;config[1]&=~4U;}return disable_status;
 default:assert(0);return EFI_UNSUPPORTED;
 }
}
VOID GuLog(const char *fmt,...) {(void)fmt;}
VOID GuLogText(const char *text) {(void)text;}
BOOLEAN GuLogReady(VOID) {return TRUE;}
static EFI_BOOT_SERVICES bs;
static EFI_PCI_IO_PROTOCOL pci;
static VOID reset(VOID) {
 memset(&bs,0,sizeof(bs));memset(&pci,0,sizeof(pci));
 bs.AllocatePool=allocate_pool;bs.FreePool=free_pool;bs.Stall=stall;
 pci.Mem.Read=mem_read;pci.Mem.Write=mem_write;pci.Pci.Read=config_read;
 pci.Pci.Write=config_write;pci.Unmap=unmap;pci.FreeBuffer=free_buffer;
 pci.AllocateBuffer=allocate_buffer;pci.Map=map_buffer;pci.Attributes=pci_attributes;
 reads=writes=unmaps=freed=pool_frees=0;fail_read=fail_write=fail_unmap=FALSE;
 allocations=maps=disables=config_reads=0;short_map=disable_noop=allocate_null=FALSE;
 read_status=write_status=config_read_status=config_write_status=EFI_SUCCESS;
 alloc_status=map_status=unmap_status=free_status=pool_status=stall_status=EFI_SUCCESS;
 enable_status=disable_status=get_status=EFI_SUCCESS;
 memset(config,0,sizeof(config));config[0]=GU_BOARD_ID;config[1]=3;
 attributes=EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_IO;
 memset(backing,0xa5,sizeof(backing));
 value=0x88888888;GuInit(&bs,&pci);Gu.bar_base=0x90000000;Gu.bar_size=0x1000000;
 Gu.owned=Gu.armed=Gu.display_ok=TRUE;
}
static VOID add_dma(VOID) {
 GU_DMA *d=calloc(1,sizeof(*d));assert(d);d->cpu=(VOID*)0x1234000;
 d->dma=0x800000;d->mapped=TRUE;d->pages=1;Gu.allocations=d;Gu.dma_count=1;
}
static VOID warning_tests(VOID) {
 UINT32 out=0;VOID *cpu;UINT64 dma;
 reset();read_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuRead32(4)==0xffffffff && Gu.fault && reads==1);
 reset();config_read_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuConfigRead(EfiPciIoWidthUint32,0,&out)==EFI_DEVICE_ERROR && Gu.fault);
 reset();stall_status=EFI_WARN_UNKNOWN_GLYPH;GuStall(100000);
 assert(Gu.fault && Gu.ticks==100000);
 reset();cpu=malloc(16);assert(cpu);pool_status=EFI_WARN_UNKNOWN_GLYPH;GuFree(cpu);
 assert(Gu.fault && Gu.retained && pool_frees==1);free(cpu);
 /* A positive warning cannot release a known DMA allocation, even if the
    protocol reports that it may have partially completed the operation. */
 reset();add_dma();unmap_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaFree((VOID*)0x1234000,0x800000)==EFI_DEVICE_ERROR);
 assert(Gu.fault && Gu.retained && Gu.allocations->mapped && unmaps==1 && !freed && !pool_frees);
 free(Gu.allocations);
 reset();add_dma();free_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaFree((VOID*)0x1234000,0x800000)==EFI_DEVICE_ERROR);
 assert(Gu.fault && Gu.retained && !Gu.allocations->mapped && unmaps==1 && freed==1 && !pool_frees);
 free(Gu.allocations);
#if GU_ENABLE_UNLOCK
 reset();write_status=EFI_WARN_UNKNOWN_GLYPH;GuWrite32(4,1);
 assert(Gu.fault && writes==1);GuWrite32(4,2);assert(writes==1);
 reset();config_write_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuConfigWrite(EfiPciIoWidthUint32,4,&out)==EFI_DEVICE_ERROR && Gu.fault);
 reset();alloc_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaAllocate(8192,&cpu,&dma)==EFI_DEVICE_ERROR);
 assert(!cpu && !dma && Gu.fault && Gu.retained && Gu.allocations);
 assert(backing[0]==0xa5 && !maps && !unmaps && !freed && !pool_frees);
 free(Gu.allocations);
 reset();alloc_status=EFI_WARN_UNKNOWN_GLYPH;allocate_null=TRUE;
 assert(GuDmaAllocate(8192,&cpu,&dma)==EFI_DEVICE_ERROR);
 assert(!cpu && !dma && Gu.fault && !Gu.retained && !Gu.allocations);
 assert(!maps && !unmaps && !freed && pool_frees==1);
 reset();map_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaAllocate(8192,&cpu,&dma)==EFI_DEVICE_ERROR);
 assert(!cpu && !dma && Gu.fault && Gu.retained && Gu.allocations);
 assert(allocations==1 && maps==1 && !unmaps && !freed && !pool_frees);
 free(Gu.allocations);
 reset();short_map=TRUE;unmap_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaAllocate(8192,&cpu,&dma)==EFI_DEVICE_ERROR);
 assert(!cpu && !dma && Gu.fault && Gu.retained && Gu.allocations->mapped);
 assert(unmaps==1 && !freed && !pool_frees);free(Gu.allocations);
 reset();short_map=TRUE;free_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuDmaAllocate(8192,&cpu,&dma)==EFI_DEVICE_ERROR);
 assert(!cpu && !dma && Gu.fault && Gu.retained && !Gu.allocations->mapped);
 assert(unmaps==1 && freed==1 && !pool_frees);free(Gu.allocations);
 /* Enable can mutate before warning. Containment must still run through
    the same protocol and independently verify hardware BME is clear. */
 reset();enable_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuEnableMaster()==EFI_DEVICE_ERROR && Gu.fault && Gu.master_attributes_pending && (config[1]&4));
 assert(GuClearMaster()==EFI_SUCCESS && !(config[1]&4) && !Gu.master_attributes_pending);
 reset();get_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuEnableMaster()==EFI_DEVICE_ERROR && Gu.fault && !Gu.master_attributes_pending && !(config[1]&4));
 reset();assert(GuEnableMaster()==EFI_SUCCESS);disable_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuClearMaster()==EFI_DEVICE_ERROR && Gu.fault && Gu.retained && Gu.master_attributes_pending);
 assert(!(config[1]&4) && Gu.master_attribute_status==EFI_DEVICE_ERROR);
 reset();assert(GuEnableMaster()==EFI_SUCCESS);disable_status=EFI_WARN_UNKNOWN_GLYPH;disable_noop=TRUE;
 assert(GuClearMaster()==EFI_DEVICE_ERROR && Gu.fault && Gu.retained && Gu.master_attributes_pending);
 assert(!(config[1]&4) && writes==1 && Gu.master_attribute_status==EFI_DEVICE_ERROR);
 reset();assert(GuEnableMaster()==EFI_SUCCESS);get_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuClearMaster()==EFI_DEVICE_ERROR && Gu.fault && Gu.retained && Gu.master_attributes_pending);
 assert(!(config[1]&4) && Gu.master_attribute_status==EFI_DEVICE_ERROR);
 reset();assert(GuEnableMaster()==EFI_SUCCESS);disable_noop=TRUE;config_write_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuClearMaster()==EFI_DEVICE_ERROR && Gu.fault && Gu.retained && Gu.master_attributes_pending);
 assert(!(config[1]&4) && Gu.master_hardware_status==EFI_DEVICE_ERROR);
 /* A failed FLR delay does not authorize post-reset configuration access. */
 reset();Gu.saved=TRUE;Gu.pcie_cap=0x40;config[0x44/4]=1U<<28;
 stall_status=EFI_WARN_UNKNOWN_GLYPH;
 assert(GuFlr()==EFI_DEVICE_ERROR && Gu.fault && Gu.started && Gu.flrs==1);
 assert(config_reads==5 && writes==1);
#else
 (void)dma;
#endif
}
int main(void) {
 UINT32 c[64]={0};c[0]=GU_BOARD_ID;c[2]=0x03000000;c[11]=GU_BOARD_SUBSYSTEM;
 reset();assert(GuTargetConfig(c,GU_PROFILE_SEGMENT,GU_PROFILE_BUS,GU_PROFILE_DEVICE,0));
 c[0]=0x268410de;assert(!GuTargetConfig(c,0,1,0,0));c[0]=GU_BOARD_ID;
 assert(!GuTargetConfig(c,0,1,0,1));assert(!GuTargetConfig(c,0,256,0,0));
#if GU_ENABLE_UNLOCK
 c[11]^=1;assert(!GuTargetConfig(c,GU_PROFILE_SEGMENT,GU_PROFILE_BUS,GU_PROFILE_DEVICE,0));
 c[11]^=1;assert(!GuTargetConfig(c,GU_PROFILE_SEGMENT,GU_PROFILE_BUS^1,GU_PROFILE_DEVICE,0));
#endif
 reset();assert(GuRead32(4)==0x12345678 && reads==1 && !Gu.fault);
 fail_read=TRUE;assert(GuRead32(4)==0xffffffff && Gu.fault);
 fail_read=FALSE;assert(GuRead32(4)==0xffffffff && reads==2);
 reset();assert(GuRead32(2)==0xffffffff && Gu.fault && reads==0);
 reset();assert(GuRead32(0x1000000)==0xffffffff && Gu.fault && reads==0);
#if GU_ENABLE_UNLOCK
 reset();assert(VerifySelectors("test")==EFI_SUCCESS);
 value=0x77777777;assert(EFI_ERROR(VerifySelectors("lost-after-reset")));
 reset();fail_read=TRUE;assert(EFI_ERROR(VerifySelectors("transport-error")));
#endif
#if !GU_ENABLE_UNLOCK
 /* Even deliberately setting every runtime gate cannot enable diagnostic IO. */
 reset();GuWrite32(4,1);assert(Gu.fault && writes==0);
 reset();assert(GuConfigWrite(EfiPciIoWidthUint32,4,&value)==EFI_ACCESS_DENIED && writes==0);
 assert(GuEnableMaster()==EFI_ACCESS_DENIED);assert(GuClearMaster()==EFI_ACCESS_DENIED);
 VOID *cpu=NULL;UINT64 dma=0;assert(GuDmaAllocate(4096,&cpu,&dma)==EFI_ACCESS_DENIED);
 assert(!cpu && !dma && !Gu.allocations && !writes);
#else
 reset();GuWrite32(4,1);assert(writes==1 && !Gu.fault);
 reset();Gu.armed=FALSE;GuWrite32(4,1);assert(!writes && Gu.fault);
 reset();Gu.owned=FALSE;GuWrite32(4,1);assert(!writes && Gu.fault);
 reset();Gu.display_ok=FALSE;GuWrite32(4,1);assert(!writes && Gu.fault);
 reset();fail_write=TRUE;GuWrite32(4,1);assert(writes==1 && Gu.fault);
 GuWrite32(4,1);assert(writes==1);
#endif
 reset();add_dma();GuStarted();assert(!GuSafeToReturn());
 assert(GuDmaFree((VOID*)0x1234000,0x800000)==EFI_ACCESS_DENIED);
 assert(Gu.retained && !unmaps && !freed);free(Gu.allocations);
 reset();add_dma();GuStarted();GuQuiesced();assert(GuSafeToReturn());
 assert(GuDmaFree((VOID*)0x1234000,0x800000)==EFI_SUCCESS);
 assert(unmaps==1 && freed==1 && pool_frees==1 && !Gu.allocations && !Gu.dma_count);
 reset();add_dma();GuStarted();Gu.fault=TRUE;GuQuiesced();assert(!Gu.quiesced);
 assert(EFI_ERROR(GuDmaFree((VOID*)0x1234000,0x800000)) && !unmaps);free(Gu.allocations);
 reset();add_dma();Gu.master_attributes_pending=TRUE;assert(!GuSafeToReturn());
 assert(EFI_ERROR(GuDmaFree((VOID*)0x1234000,0x800000)) && !unmaps);free(Gu.allocations);
 reset();add_dma();fail_unmap=TRUE;
 assert(EFI_ERROR(GuDmaFree((VOID*)0x1234000,0x800000)) && Gu.retained && !freed);
 free(Gu.allocations);
 warning_tests();
 puts(GU_ENABLE_UNLOCK?"experimental UEFI guards: PASS":"diagnostic UEFI guards: PASS");
 return 0;
}
