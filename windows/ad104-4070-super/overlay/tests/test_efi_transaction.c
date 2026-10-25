/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ /* SPDX-License-Identifier: GPL-2.0-only */
/* Real retained core + compatibility layer + UEFI port; only EFI services are fake. */
#include "../port.h"
#ifdef GU_ENTRY_LATCHED
/* Test fixture baseline only, never a production fixed-selector definition. */
#ifndef GU_STOCK_PROFILE_SS0
#define GU_STOCK_PROFILE_SS0 0x23703005U
#endif
static UINT32 test_entry_ss0=0x62543025U,test_entry_ss1=4;
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#undef EPROTO
#undef EBADMSG
#undef EOVERFLOW
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EALREADY
#include "../compat/compat.h"
#include "../vendor/unlock_610.h"
#include "../vendor/prep_610.h"
#include "../vendor/vbios_610.h"
#include "../profile.h"
#include "captured_config4070s.h"

#ifndef GU_UEFI_PORT
#error This integration test must execute the real UEFI compatibility path.
#endif
#define MAX_BUFFERS 24000
#define MAX_POOLS 26000
#define GSP 0x110000U
#define SEC2 0x840000U
#define SS0 0x82381cU
#define SS1 0x823820U
#define PLM 0x823804U
enum scenario {
  SUCCESS, FWSEC_TIMEOUT, POST_FLR_TIMEOUT, BME_READBACK_FAIL,
  MMIO_WRITE_FAIL, FLUSH_FAIL, FLR_WRITE_FAIL, UNMAP_FAIL,
  MAP_FAIL, MAP_PARTIAL, RESTORE_FAIL, DISPLAY_CHANGED, SELECTOR_LOST,
  FWSEC_NO_WPR, FWSEC_RUNNING, MARKER_MISSING, HALT_MISSING,
  WPR_CFG_CLOSED, WPR_CLOSED, FEAT_CLOSED, READOUT_UNCHANGED, FWSEC_ERROR_CODE,
#ifdef GU_ENTRY_LATCHED
  CORE_ENTRY_SS0, CORE_ENTRY_SS1, BEFORE_FW_SS0, BEFORE_FW_SS1,
  SELECTOR_ENTRY_SS0, SELECTOR_ENTRY_SS1, PRE_FLR_SS1, LATE_FLR_SS1,
#endif
  REBAR_WRITE_0, REBAR_WRITE_1, REBAR_WRITE_2,
  REBAR_READBACK_0, REBAR_READBACK_1, REBAR_READBACK_2,
  REBAR_READBACK_ERROR_0, REBAR_READBACK_ERROR_1, REBAR_READBACK_ERROR_2,
  REBAR_DRIFT_HEADER, REBAR_DRIFT_CAP0, REBAR_DRIFT_CTL0,
  REBAR_DRIFT_CAP1, REBAR_DRIFT_CTL1, REBAR_DRIFT_CAP2, REBAR_DRIFT_CTL2,
  REBAR_READ_ERROR_HEADER, REBAR_READ_ERROR_CAP0, REBAR_READ_ERROR_CTL0,
  REBAR_READ_ERROR_CAP1, REBAR_READ_ERROR_CTL1, REBAR_READ_ERROR_CAP2, REBAR_READ_ERROR_CTL2,
  REBAR_DRIFT_CTL0_BIT14, REBAR_DRIFT_CTL0_BIT15, REBAR_DRIFT_CTL0_BIT16, REBAR_DRIFT_CTL0_BIT31,
  REBAR_DRIFT_CTL1_BIT14, REBAR_DRIFT_CTL1_BIT15, REBAR_DRIFT_CTL1_BIT16, REBAR_DRIFT_CTL1_BIT31,
  REBAR_DRIFT_CTL2_BIT14, REBAR_DRIFT_CTL2_BIT15, REBAR_DRIFT_CTL2_BIT16, REBAR_DRIFT_CTL2_BIT31,
  REBAR_DECODE_STUCK,
  FLR_IDENTITY_TIMEOUT, FLR_IDENTITY_WRONG, FLR_IDENTITY_READ_ERROR,
  SCENARIO_COUNT
};
static const char *case_names[]={"success","fwsec-timeout","post-flr-gfw-timeout",
  "bme-readback-failure","mmio-write-failure","flush-failure","flr-write-failure",
  "unmap-failure","map-failure","partial-map","pci-restore-failure","display-changed","selector-lost",
  "fwsec-halted-no-wpr","fwsec-running-then-halt",
  "marker-missing","halt-missing","wpr-cfg-closed","wpr-closed","feat-closed","readout-unchanged","fwsec-error-code"
#ifdef GU_ENTRY_LATCHED
  ,"core-entry-ss0","core-entry-ss1","before-fw-ss0","before-fw-ss1",
  "selector-entry-ss0","selector-entry-ss1","pre-flr-ss1","late-flr-ss1"
#endif
  ,"rebar-write-0","rebar-write-1","rebar-write-2",
  "rebar-readback-0","rebar-readback-1","rebar-readback-2",
  "rebar-readback-error-0","rebar-readback-error-1","rebar-readback-error-2",
  "rebar-drift-header","rebar-drift-cap0","rebar-drift-ctl0",
  "rebar-drift-cap1","rebar-drift-ctl1","rebar-drift-cap2","rebar-drift-ctl2",
  "rebar-read-error-header","rebar-read-error-cap0","rebar-read-error-ctl0",
  "rebar-read-error-cap1","rebar-read-error-ctl1","rebar-read-error-cap2","rebar-read-error-ctl2",
  "rebar-drift-ctl0-bit14","rebar-drift-ctl0-bit15","rebar-drift-ctl0-bit16","rebar-drift-ctl0-bit31",
  "rebar-drift-ctl1-bit14","rebar-drift-ctl1-bit15","rebar-drift-ctl1-bit16","rebar-drift-ctl1-bit31",
  "rebar-drift-ctl2-bit14","rebar-drift-ctl2-bit15","rebar-drift-ctl2-bit16","rebar-drift-ctl2-bit31",
  "rebar-decode-stuck","flr-identity-timeout","flr-identity-wrong","flr-identity-read-error"
};
typedef struct {VOID *cpu;UINTN bytes;UINT64 dma;BOOLEAN mapped,freed;} TEST_BUFFER;
typedef struct {VOID *cpu;BOOLEAN freed;} TEST_POOL;
static TEST_BUFFER buffers[MAX_BUFFERS];
static TEST_POOL pools[MAX_POOLS];
static EFI_BOOT_SERVICES bs;
static EFI_PCI_IO_PROTOCOL pci;
static UINT8 config[4096],prom[1024*1024];
static UINT32 core_captured_config[64];
static BOOLEAN core_use_capture;
static UINT32 registers[0x1000000/4];
static UINT8 gsp_dmem[0xe00];
static struct {
  enum scenario mode;
  UINTN buffer_count,pool_count,pool_live,find_hint;
  UINTN maps,unmaps,frees,live_maps,live_buffers,mmio_writes,pci_writes;
  UINTN gsp_starts,sec2_starts,flrs,flushes,dma_reads,radix_bytes;
  UINTN dmem_reads,pio_reads,pio_writes,gsp_poll_reads,trace_count;
  UINTN master_enables,master_disables;
  UINTN config_reads,rebar_reads[7],rebar_writes[3],bar_writes,decode_enables;
  UINT32 entry_read_error,restore_read_error,restore_write_error,restore_readback_error;
  UINT32 restore_readback_mismatch,restore_drift_offset,restore_drift_mask;
  UINT32 rebar_verified;
  BOOLEAN isolated_rebar,decode_off_verified,decode_stuck,config_error_injected;
  UINT64 pci_attributes;
  UINT32 write_trace[8192][4];
  UINT64 next_dma,delay_since_flr,elapsed,meta_dma;
  BOOLEAN bme_read_failed,unmap_failed,write_failed,restore_failed;
  char log[65536];
} fake;
static const UINT32 rebar_offsets[7]={0xbb0,0xbb4,0xbb8,0xbbc,0xbc0,0xbc4,0xbc8};
static const UINT32 rebar_words[7]={0xc1c10015,0x100,0x460,0x7fc00,0xe01,0x200,0x503};
static const UINT32 rebar_controls[3]={0xbb8,0xbc0,0xbc8};
static const UINT32 rebar_values[3]={0x460,0xe01,0x503};

static void fail(const char *expression,const char *file,int line) {
  size_t length=strlen(fake.log);
  fprintf(stderr,"ASSERTION FAILED [%s] %s:%d: %s\n",case_names[fake.mode],file,line,expression);
  fprintf(stderr,"started=%u quiesced=%u fault=%u retained=%u GSP=%zu SEC2=%zu FLR=%zu mappings=%zu buffers=%zu\n",
    Gu.started,Gu.quiesced,Gu.fault,Gu.retained,fake.gsp_starts,fake.sec2_starts,
    fake.flrs,fake.live_maps,fake.live_buffers);
  fprintf(stderr,"%s",fake.log+(length>5000?length-5000:0));
  exit(1);
}
#define CHECK(x) ((x)?(void)0:fail(#x,__FILE__,__LINE__))
static VOID trace_write(UINT32 kind,UINT32 width,UINT32 offset,UINT32 value) {
  CHECK(fake.trace_count<8192);UINT32 *r=fake.write_trace[fake.trace_count++];
  r[0]=kind;r[1]=width;r[2]=offset;r[3]=value;
}
VOID GuLog(const char *format,...) {
  va_list args;size_t used=strlen(fake.log);
  va_start(args,format);vsnprintf(fake.log+used,sizeof(fake.log)-used,format,args);va_end(args);
}
VOID GuLogText(const char *text) {GuLog("%s",text);}
static UINT64 le64(const VOID *pointer) {
  const UINT8 *p=pointer;UINT64 value=0;for(int i=7;i>=0;i--)value=(value<<8)|p[i];return value;
}
static UINT32 le32(const VOID *pointer) {
  const UINT8 *p=pointer;return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24);
}
static VOID quiet_check(void) {CHECK(!fake.flrs || fake.delay_since_flr>=100000);}
static TEST_BUFFER *cpu_buffer(VOID *cpu) {
  if(fake.buffer_count && buffers[fake.buffer_count-1].cpu==cpu)return &buffers[fake.buffer_count-1];
  for(UINTN step=0;step<fake.buffer_count;step++) {
    UINTN index=(fake.find_hint+step)%fake.buffer_count;
    if(buffers[index].cpu==cpu){fake.find_hint=(index+1)%fake.buffer_count;return &buffers[index];}
  }
  CHECK(0);return NULL;
}
static UINT8 *dma_pointer(UINT64 address,UINTN bytes) {
  UINTN low=0,high=fake.buffer_count;
  while(low<high) {
    UINTN mid=low+(high-low)/2;
    if(buffers[mid].dma<=address)low=mid+1;else high=mid;
  }
  CHECK(low>0);TEST_BUFFER *b=&buffers[low-1];
  CHECK(b->mapped && !b->freed && address>=b->dma && address-b->dma<=b->bytes);
  CHECK(bytes<=b->bytes-(UINTN)(address-b->dma));
  CHECK(address!=(UINT64)(UINTN)b->cpu+(address-b->dma));
  fake.dma_reads++;return (UINT8 *)b->cpu+(UINTN)(address-b->dma);
}
static VOID check_fwsec_image(void) {
#ifdef GU_AD104_TRANSACTION
  struct gpu_unlock_fwsec fw;UINT32 sig_at;
  CHECK(!gpu_unlock_vbios_fwsec(prom,sizeof(prom),&fw));
  CHECK(fw.image_off==0x45b44 && fw.image_size==0x10500 && fw.data_off==0xf700 && fw.dmem_size==0xd80);
  CHECK(!gpu_unlock_fwsec_sig_offset(fw.sig_versions,1,384,&sig_at));
  UINT64 last_source=((UINT64)registers[(GSP+0x110)/4]<<8)|((UINT64)registers[(GSP+0x128)/4]<<40);
  UINT8 *image=dma_pointer(last_source-fw.data_off,fw.image_size);
  CHECK(!memcmp(gsp_dmem,image+fw.data_off,sizeof(gsp_dmem)));
  CHECK(!memcmp(gsp_dmem+fw.pkc_off,prom+fw.sig_off+sig_at,384));
  UINT8 expected[0x10500];memcpy(expected,prom+fw.image_off,sizeof(expected));
  CHECK(!gpu_unlock_fwsec_patch_frts(expected,&fw,0x2ff800000ULL));
  memcpy(expected+fw.data_off+fw.pkc_off,prom+fw.sig_off+sig_at,384);
  CHECK(!memcmp(gsp_dmem,expected+fw.data_off,sizeof(gsp_dmem)));
#else
  UINT64 last_source=((UINT64)registers[(GSP+0x110)/4]<<8)|((UINT64)registers[(GSP+0x128)/4]<<40);
  CHECK(last_source>=0xdf00);UINT8 *image=dma_pointer(last_source-0xdf00,0xe700);
  CHECK(!memcmp(gsp_dmem,image+0xdf00,sizeof(gsp_dmem)));
  CHECK(!memcmp(gsp_dmem+0x5a4,prom+0x43060+0x300,384));
  CHECK(le32(gsp_dmem+0x7c0+32)==0x27fe00);
  CHECK(le32(gsp_dmem+0x7c0+36)==0x100);
#endif
}
static VOID check_booter_publication(void) {
  UINT64 meta_address=(UINT64)registers[(SEC2+0x40)/4]|((UINT64)registers[(SEC2+0x44)/4]<<32);
  UINT8 *meta=dma_pointer(meta_address,256);fake.meta_dma=meta_address;
  CHECK(le64(meta)==GU_WPR_META_MAGIC && le64(meta+8)==1);
  CHECK(le64(meta+24)==0x5053000 && le64(meta+80)==0xfa00);
  UINT64 delta=0;
#ifdef GU_AD104_TRANSACTION
  delta=0x7fa00000ULL;
#endif
  CHECK(le64(meta+112)==0x275400000ULL+delta && le64(meta+136)==0x27ada0000ULL+delta);
  CHECK(le64(meta+144)==0x27fdf7000ULL+delta && le64(meta+152)==0x27fe00000ULL+delta);
  CHECK(le64(meta+168)==0x27ff00000ULL+delta && le64(meta+176)==0x280000000ULL+delta);
  CHECK(!memcmp(dma_pointer(le64(meta+72),64000),GuAssets[1].firmware.data,64000));
  CHECK(!memcmp(dma_pointer(le64(meta+32),36864),GuAssets[3].firmware.data,36864));
  /* Walk published DMA addresses as firmware would; expected ELF bytes are pinned. */
  for(UINT64 offset=0,page=0;offset<0x5053000;offset+=4096,page++) {
    UINT8 *l0=dma_pointer(le64(meta+16),4096);
    UINT8 *l1=dma_pointer(le64(l0+(page>>18)*8),4096);
    UINT8 *l2=dma_pointer(le64(l1+((page>>9)&511)*8),4096);
    UINT8 *leaf=dma_pointer(le64(l2+(page&511)*8),4096);
    UINTN count=(UINTN)(0x5053000-offset);if(count>4096)count=4096;
    CHECK(!memcmp(leaf,GuAssets[2].firmware.data+0x40+(size_t)offset,count));
    for(UINTN i=count;i<4096;i++)CHECK(!leaf[i]);
    fake.radix_bytes+=count;
  }
}
static VOID check_stock_signature(void) {
  UINT8 *meta=dma_pointer(fake.meta_dma,256);
  CHECK(le64(meta+80)==4096);
  struct gpu_unlock_span signature;
  CHECK(!gpu_unlock_elf_section(GuAssets[2].firmware.data,GuAssets[2].firmware.size,
                               ".fwsignature_ad10x",&signature));
  CHECK(signature.size==4096);
  CHECK(!memcmp(dma_pointer(le64(meta+72),4096),GuAssets[2].firmware.data+signature.offset,4096));
}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type,UINTN size,VOID **memory) {
  CHECK(type==EfiLoaderData && fake.pool_count<MAX_POOLS);
  *memory=calloc(1,size);if(!*memory)return EFI_OUT_OF_RESOURCES;
  pools[fake.pool_count++]=(TEST_POOL){*memory,FALSE};fake.pool_live++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pool(VOID *memory) {
  for(UINTN i=fake.pool_count;i>0;i--)if(pools[i-1].cpu==memory && !pools[i-1].freed) {
    pools[i-1].freed=TRUE;fake.pool_live--;free(memory);return EFI_SUCCESS;
  }
  CHECK(0);return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI allocate_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE type,
  EFI_MEMORY_TYPE memory_type,UINTN pages,VOID **cpu,UINT64 attributes) {
  CHECK(p==&pci && type==AllocateAnyPages && memory_type==EfiBootServicesData && !attributes);
  CHECK(pages && fake.buffer_count<MAX_BUFFERS);
  *cpu=_aligned_malloc(pages*4096,4096);if(!*cpu)return EFI_OUT_OF_RESOURCES;
  buffers[fake.buffer_count++]=(TEST_BUFFER){*cpu,pages*4096,0,FALSE,FALSE};
  fake.live_buffers++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_buffer(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu) {
  CHECK(p==&pci);TEST_BUFFER *b=cpu_buffer(cpu);
  CHECK(!b->freed && !b->mapped && pages*4096==b->bytes);
  CHECK(!Gu.started || Gu.quiesced);
  b->freed=TRUE;fake.frees++;fake.live_buffers--;_aligned_free(cpu);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI map_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION operation,
  VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *device_address,VOID **mapping) {
  CHECK(p==&pci && operation==EfiPciIoOperationBusMasterCommonBuffer);
  TEST_BUFFER *b=cpu_buffer(cpu);CHECK(*bytes==b->bytes && !b->mapped && !b->freed);
  fake.maps++;if(fake.mode==MAP_FAIL && fake.maps==1)return EFI_UNSUPPORTED;
  b->dma=fake.next_dma;fake.next_dma+=b->bytes;CHECK(fake.next_dma<=0xffffffffULL);
  CHECK(b->dma!=(UINT64)(UINTN)cpu);b->mapped=TRUE;fake.live_maps++;
  *device_address=b->dma;*mapping=b;
  if(fake.mode==MAP_PARTIAL && fake.maps==48)*bytes/=2;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmap_buffer(EFI_PCI_IO_PROTOCOL *p,VOID *mapping) {
  CHECK(p==&pci);TEST_BUFFER *b=mapping;
  CHECK(b>=buffers && b<buffers+fake.buffer_count && b->mapped && !b->freed);
  CHECK(!Gu.started || Gu.quiesced);fake.unmaps++;
  if(fake.mode==UNMAP_FAIL && !fake.unmap_failed){fake.unmap_failed=TRUE;return EFI_DEVICE_ERROR;}
  b->mapped=FALSE;fake.live_maps--;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_config(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT32 offset,UINTN count,VOID *value) {
  CHECK(p==&pci && width<=EfiPciIoWidthUint32 && count==1);quiet_check();
  UINTN bytes=(UINTN)1<<width;CHECK(offset+bytes<=sizeof(config));
  fake.config_reads++;
  if(fake.flrs && offset==0) {
    if(fake.mode==FLR_IDENTITY_TIMEOUT){*(UINT32 *)value=0xffffffff;return EFI_SUCCESS;}
    if(fake.mode==FLR_IDENTITY_WRONG){*(UINT32 *)value=0xdeaddead;return EFI_SUCCESS;}
    if(fake.mode==FLR_IDENTITY_READ_ERROR)return EFI_DEVICE_ERROR;
  }
  if(offset>=256) {
    CHECK(width==EfiPciIoWidthUint32 && offset>=0xbb0 && offset<=0xbc8 && !(offset&3));
    fake.rebar_reads[(offset-0xbb0)/4]++;
    if(!fake.flrs && offset==fake.entry_read_error){fake.config_error_injected=TRUE;return EFI_DEVICE_ERROR;}
    if(fake.flrs) {
      CHECK(fake.decode_off_verified && !(config[4]&7));
      if(offset==fake.restore_read_error){fake.config_error_injected=TRUE;return EFI_DEVICE_ERROR;}
      for(UINTN i=0;i<3;i++)if(offset==rebar_controls[i] && fake.rebar_writes[i]) {
        if(offset==fake.restore_readback_error){fake.config_error_injected=TRUE;return EFI_DEVICE_ERROR;}
        if(offset==fake.restore_readback_mismatch) {
          fake.config_error_injected=TRUE;*(UINT32 *)value=le32(config+offset)^0x100;return EFI_SUCCESS;
        }
        if(le32(config+offset)==rebar_values[i])fake.rebar_verified|=1U<<i;
      }
    }
  }
  if(fake.flrs && offset==4 && !(config[4]&7))fake.decode_off_verified=TRUE;
  if(fake.mode==BME_READBACK_FAIL && offset==4 && (config[4]&4) && !fake.bme_read_failed) {
    fake.bme_read_failed=TRUE;return EFI_DEVICE_ERROR;
  }
  memcpy(value,config+offset,bytes);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_config(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT32 offset,UINTN count,VOID *value) {
  CHECK(p==&pci && width<=EfiPciIoWidthUint32 && count==1);quiet_check();
  UINTN bytes=(UINTN)1<<width;CHECK(offset+bytes<=sizeof(config));
  CHECK(!(offset<=6 && offset+bytes>6) && offset!=0x82 && offset!=0x64);
  UINT32 trace_value=0;memcpy(&trace_value,value,bytes);trace_write(1,(UINT32)width,offset,trace_value);
  fake.pci_writes++;
  /* Every write overlapping Command's low byte must keep BME clear after FLR,
     including Command 3 -> 7 -> 0 while IO/MEM decoding remains enabled. */
  if(fake.flrs && offset<=4 && offset+bytes>4) {
    UINT8 written_command_low=((const UINT8 *)value)[4-offset];
    CHECK(!(written_command_low&4));
  }
  if(offset>=256) {
    CHECK(width==EfiPciIoWidthUint32 && (offset==0xbb8 || offset==0xbc0 || offset==0xbc8));
    UINTN i=(offset-0xbb8)/8;
    CHECK(Gu.saved && Gu.flrs==1 && fake.flrs==1 && Gu.profile_verified && Gu.owned && Gu.armed && Gu.display_ok);
    CHECK(fake.decode_off_verified && !(config[4]&7));
    CHECK(trace_value==rebar_values[i]);fake.rebar_writes[i]++;
    CHECK(fake.rebar_writes[i]==1);
    if(offset==fake.restore_write_error){fake.config_error_injected=TRUE;return EFI_DEVICE_ERROR;}
  }
  if(fake.flrs && offset>=0x10 && offset<=0x24) {
    /* This assertion exposes missing restoration before a BAR can move. */
    CHECK(fake.rebar_verified==7 && fake.decode_off_verified && !(config[4]&7));
    fake.bar_writes++;
  }
  if(fake.flrs && offset==4 && (trace_value&7) && !(config[4]&3)) {
    CHECK(fake.rebar_verified==7 && fake.bar_writes==6 && !(trace_value&4));
    fake.decode_enables++;
  }
  memcpy(config+offset,value,bytes);
  if(fake.flrs && offset==4 && !(trace_value&7) && fake.decode_stuck)config[4]|=2;
  if(offset==0x80 && width==EfiPciIoWidthUint16 && (*(UINT16 *)value&0x8000)) {
    CHECK(!(config[4]&4));
    if(!fake.isolated_rebar) {
      CHECK(fake.gsp_starts==1 && fake.sec2_starts==1);
      CHECK(!fake.unmaps && !fake.frees);check_stock_signature();
    }
    fake.flrs++;CHECK(fake.flrs==1);fake.delay_since_flr=0;
    memset(config+0x10,0,24);config[4]=3;config[0x80]=config[0x81]=0;
    /* Pinned EDK2 PciExpress21.h v1: size is six bits 13:8. Bits 15:14
       and 31:16 are immutable reserved/capability fields, never size fields. */
    for(UINTN i=0;i<3;i++) {
      UINT32 reset=rebar_values[i]&~0x3f00U;memcpy(config+rebar_controls[i],&reset,4);
    }
    if(fake.restore_drift_offset) {
      UINT32 drift=le32(config+fake.restore_drift_offset)^fake.restore_drift_mask;
      memcpy(config+fake.restore_drift_offset,&drift,4);
    }
    registers[(GSP+0x3c0)/4]=registers[(SEC2+0x3c0)/4]=0;
    registers[0x1fa824/4]=registers[0x1fa828/4]=0;
    registers[0x118128/4]=1;registers[0x118234/4]=0xff;registers[0x110100/4]=0x10;
    if(fake.mode==POST_FLR_TIMEOUT)registers[0x110100/4]=registers[0x118234/4]=0;
    if(fake.mode==SELECTOR_LOST)registers[SS1/4]=3;
    if(fake.mode==FLR_WRITE_FAIL)return EFI_DEVICE_ERROR;
  }
  if(fake.mode==RESTORE_FAIL && fake.flrs && offset==0x10 && !fake.restore_failed) {
    fake.restore_failed=TRUE;return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_memory(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT8 bar,UINT64 offset,UINTN count,VOID *value) {
  CHECK(p==&pci && !bar);quiet_check();
  CHECK(width==EfiPciIoWidthUint8 || (width==EfiPciIoWidthUint32 && count==1));
  UINTN bytes=((UINTN)1<<width)*count;CHECK(offset+bytes<=sizeof(registers));
#ifdef GU_ENTRY_LATCHED
  if(offset==0x300000 && width==EfiPciIoWidthUint8 && fake.maps && !fake.gsp_starts) {
    if(fake.mode==BEFORE_FW_SS0)registers[SS0/4]^=1;
    if(fake.mode==BEFORE_FW_SS1)registers[SS1/4]^=1;
  }
#endif
  if(offset==GSP+0x1c0 || offset==GSP+0x1c4)fake.pio_reads++;
  if(offset==GSP+0x1c4) {
    UINT32 at=registers[(GSP+0x1c0)/4];
    CHECK(!fake.gsp_starts && !(at&0xff000003U) && at+4<=sizeof(gsp_dmem));
    fake.dmem_reads++;
    if(0){*(UINT32 *)value=0xffffffffU;return EFI_SUCCESS;}
    memcpy(value,gsp_dmem+at,4);return EFI_SUCCESS;
  }
  if(offset==GSP+0x100 && fake.gsp_starts && !fake.sec2_starts) {
    fake.gsp_poll_reads++;
    if(fake.mode==FWSEC_RUNNING)
      registers[offset/4]=fake.gsp_poll_reads<=2?0:0x10;
  }
  memcpy(value,(UINT8 *)registers+(UINTN)offset,bytes);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_memory(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT8 bar,UINT64 offset,UINTN count,VOID *value) {
  CHECK(p==&pci && !bar && width==EfiPciIoWidthUint32 && count==1);quiet_check();
  CHECK(offset+4<=sizeof(registers) && Gu.started);
  UINT32 v=*(UINT32 *)value;registers[offset/4]=v;fake.mmio_writes++;
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
  CHECK(offset!=SS1);
#endif
#ifdef GU_ENTRY_LATCHED
  if(offset==SS0 && fake.mode==PRE_FLR_SS1)registers[SS1/4]^=1;
#endif
  trace_write(0,(UINT32)width,(UINT32)offset,v);
  CHECK(offset!=GSP+0x1c4); /* Diagnostics must never write firmware data. */
  if(offset==GSP+0x1c0) {
    CHECK(!fake.gsp_starts);fake.pio_writes++;
  }
  if(fake.mode==MMIO_WRITE_FAIL && !fake.write_failed){fake.write_failed=TRUE;return EFI_DEVICE_ERROR;}
  if(offset==GSP+0x118 || offset==SEC2+0x118) {
    UINT32 base=offset==GSP+0x118?GSP:SEC2;
    UINT64 source=((UINT64)registers[(base+0x110)/4]<<8)|((UINT64)registers[(base+0x128)/4]<<40);
    UINT8 *source_cpu=dma_pointer(source+registers[(base+0x11c)/4],256);
    if(base==GSP && !(v&0x10)) {
      UINT32 dest=registers[(base+0x114)/4];CHECK(dest+256<=sizeof(gsp_dmem));
      memcpy(gsp_dmem+dest,source_cpu,256);
    }
    registers[offset/4]=2;
  }
  if(offset==SEC2+0x3c0 && !(v&1))registers[(SEC2+0x100)/4]=0x10;
  if(offset==0x111668 || offset==0x841668)registers[offset/4]=1;
  if(offset==GSP+0x100 || offset==GSP+0x130) {
    CHECK(fake.pci_attributes&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    fake.gsp_starts++;CHECK(fake.gsp_starts==1 && (config[4]&4));check_fwsec_image();
    if(fake.mode!=FWSEC_NO_WPR) {
      registers[0x1fa824/4]=0x27fe000;registers[0x1fa828/4]=0x27feff0;
#ifdef GU_AD104_TRANSACTION
      registers[0x1fa824/4]=0x2ff8000;registers[0x1fa828/4]=0x2ff8ff0;
#endif
    }
    if(fake.mode==FWSEC_ERROR_CODE)registers[0x1438/4]=0x10000;
    registers[GSP/4+0x100/4]=fake.mode==FWSEC_TIMEOUT?2:0x10;
  }
  if(offset==SEC2+0x100 || offset==SEC2+0x130) {
    fake.sec2_starts++;CHECK(fake.sec2_starts==1 && (config[4]&4));
    check_booter_publication();registers[PLM/4]=fake.mode==FEAT_CLOSED?0xffffff8f:~0U;
    registers[0x1fa7cc/4]=fake.mode==WPR_CFG_CLOSED?0x4cb8f:0xfffff0ff;
    registers[0x1fa7c4/4]=fake.mode==WPR_CLOSED?0x4cb8f:0xffffffff;
    registers[(SEC2+0x40)/4]=fake.mode==MARKER_MISSING?0:0x67b00001;
    registers[(SEC2+0x100)/4]=fake.mode==HALT_MISSING?0:0x10;
#ifdef GU_ENTRY_LATCHED
    if(fake.mode==SELECTOR_ENTRY_SS0)registers[SS0/4]^=1;
    if(fake.mode==SELECTOR_ENTRY_SS1)registers[SS1/4]^=1;
#endif
  }
  if(offset==SS1) {
    registers[offset/4]=v&15;
    if(fake.mode!=READOUT_UNCHANGED)registers[0x823818/4]=0;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_PCI_IO_PROTOCOL *p) {
  CHECK(p==&pci);fake.flushes++;
  return fake.mode==FLUSH_FAIL && fake.flushes==1?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI master_attributes(EFI_PCI_IO_PROTOCOL *p,
 EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 mask,UINT64 *out) {
  CHECK(p==&pci);
  if(op==EfiPciIoAttributeOperationSupported) {
    CHECK(out && !mask);*out=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    return EFI_SUCCESS;
  }
  if(op==EfiPciIoAttributeOperationGet) {
    CHECK(out && !mask);*out=fake.pci_attributes;return EFI_SUCCESS;
  }
  CHECK(!out && mask==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
  UINT16 command=*(UINT16 *)(config+4);
  if(op==EfiPciIoAttributeOperationEnable) {
    fake.master_enables++;command|=4;fake.pci_attributes|=mask;
  } else {
    CHECK(op==EfiPciIoAttributeOperationDisable);
    fake.master_disables++;command&=(UINT16)~4U;fake.pci_attributes&=~mask;
#ifdef GU_ENTRY_LATCHED
    if(fake.mode==LATE_FLR_SS1)registers[SS1/4]^=1;
#endif
  }
  return write_config(p,EfiPciIoWidthUint16,4,1,&command);
}
static EFI_STATUS EFIAPI stall(UINTN microseconds) {
  fake.elapsed+=microseconds;if(fake.flrs)fake.delay_since_flr+=microseconds;return EFI_SUCCESS;
}
static BOOLEAN display_check(void) {return fake.mode!=DISPLAY_CHANGED || !Gu.started;}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
  CHECK(p==&pci);*s=0;*b=1;*d=0;*f=0;return EFI_SUCCESS;
}
static VOID setup_unprepared(enum scenario mode) {
  memset(&fake,0,sizeof(fake));memset(buffers,0,sizeof(buffers));memset(pools,0,sizeof(pools));
  memset(&bs,0,sizeof(bs));memset(&pci,0,sizeof(pci));memset(config,0,sizeof(config));
  memset(registers,0,sizeof(registers));memset(gsp_dmem,0,sizeof(gsp_dmem));
  fake.mode=mode;fake.next_dma=0x12000000;
  bs.AllocatePool=allocate_pool;bs.FreePool=free_pool;bs.Stall=stall;
  pci.AllocateBuffer=allocate_buffer;pci.FreeBuffer=free_buffer;pci.Map=map_buffer;pci.Unmap=unmap_buffer;
  pci.Pci.Read=read_config;pci.Pci.Write=write_config;pci.Mem.Read=read_memory;pci.Mem.Write=write_memory;pci.Flush=flush;
  pci.GetLocation=location;
  pci.Attributes=master_attributes;fake.pci_attributes=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  memcpy(config,CapturedConfig4070S,sizeof(config));
  if(mode>=REBAR_WRITE_0 && mode<=REBAR_WRITE_2)fake.restore_write_error=rebar_controls[mode-REBAR_WRITE_0];
  if(mode>=REBAR_READBACK_0 && mode<=REBAR_READBACK_2)fake.restore_readback_mismatch=rebar_controls[mode-REBAR_READBACK_0];
  if(mode>=REBAR_READBACK_ERROR_0 && mode<=REBAR_READBACK_ERROR_2)fake.restore_readback_error=rebar_controls[mode-REBAR_READBACK_ERROR_0];
  if(mode>=REBAR_DRIFT_HEADER && mode<=REBAR_DRIFT_CTL2) {
    fake.restore_drift_offset=rebar_offsets[mode-REBAR_DRIFT_HEADER];fake.restore_drift_mask=1;
  }
  if(mode>=REBAR_READ_ERROR_HEADER && mode<=REBAR_READ_ERROR_CTL2)fake.restore_read_error=rebar_offsets[mode-REBAR_READ_ERROR_HEADER];
  if(mode>=REBAR_DRIFT_CTL0_BIT14 && mode<=REBAR_DRIFT_CTL2_BIT31) {
    static const UINT32 immutable_masks[]={0x4000,0x8000,0x10000,0x80000000};
    UINTN variant=(UINTN)(mode-REBAR_DRIFT_CTL0_BIT14);
    fake.restore_drift_offset=rebar_controls[variant/4];fake.restore_drift_mask=immutable_masks[variant%4];
  }
  fake.decode_stuck=mode==REBAR_DECODE_STUCK;
  registers[0]=0x194000a1;registers[0x118128/4]=0x8b8f;registers[0x118234/4]=0x03ff;
  registers[GSP/4+0x100/4]=0x10;registers[0x1fa824/4]=0x1ffffe00;
  registers[PLM/4]=0xffffff8f;registers[SS0/4]=0x46254040;registers[SS1/4]=3;
  registers[0x1183a4/4]=0x2800;registers[0x625f04/4]=1;registers[0x1fa7c8/4]=0x45cb8f;
#ifdef GU_AD104_TRANSACTION
  registers[SS0/4]=GU_STOCK_PROFILE_SS0;registers[SS1/4]=6;registers[0x1183a4/4]=0x2ffa;
#endif
  registers[0x1fa82c/4]=0x100000;registers[0x1fa830/4]=0x2ef9ff0;
  registers[0x824148/4]=1;registers[0x8241e0/4]=1;
  registers[0x1fa7cc/4]=registers[0x1fa7c4/4]=0x4cb8f;
  registers[0x9a0148/4]=0xffffff0f;
  registers[0x823814/4]=0x233;registers[0x823818/4]=0x410080;
  registers[(SEC2+0x100)/4]=0xbadf5620;
  registers[(GSP+0xf4)/4]=registers[(SEC2+0xf4)/4]=1U<<31;
  registers[(GSP+0x118)/4]=registers[(SEC2+0x118)/4]=2;
  registers[(GSP+0x1c0)/4]=0x40;
  registers[(GSP+0x94)/4]=0x12345678;
  memcpy((UINT8 *)registers+0x300000,prom,sizeof(prom));
  GuInit(&bs,&pci);Gu.bar_base=0x84000000;Gu.bar_size=sizeof(registers);
#ifdef GU_AD104_TRANSACTION
  if(core_use_capture)memcpy(config,core_captured_config,sizeof(core_captured_config));
  Gu.bar_base=GU_BAR0_BASE;
#endif
  Gu.pcie_cap=0x78;Gu.pcie_version=2;Gu.pm_cap=0x60;Gu.msi_cap=0x68;Gu.msix_cap=0;
  Gu.owned=Gu.armed=Gu.display_ok=TRUE;Gu.display_check=display_check;
#ifdef GU_ENTRY_LATCHED
  registers[SS0/4]=test_entry_ss0;registers[SS1/4]=test_entry_ss1;
#endif
}
static VOID setup(enum scenario mode) {
  setup_unprepared(mode);
#ifdef GU_ENTRY_LATCHED
  CHECK(GuAd104Prepare()==EFI_SUCCESS);
  if(mode==CORE_ENTRY_SS0)registers[SS0/4]^=1;
  if(mode==CORE_ENTRY_SS1)registers[SS1/4]^=1;
#endif
}
static VOID dispose_host_fixture(void) {
  /* Test process disposal only, after retention assertions. This is not a recovery path. */
  for(UINTN i=0;i<fake.buffer_count;i++)if(!buffers[i].freed)_aligned_free(buffers[i].cpu);
  for(UINTN i=0;i<fake.pool_count;i++)if(!pools[i].freed)free(pools[i].cpu);
}
static UINT8 *read_file(const char *path,size_t *length) {
  FILE *file=fopen(path,"rb");CHECK(file);CHECK(!fseek(file,0,SEEK_END));
  long size=ftell(file);CHECK(size>0 && !fseek(file,0,SEEK_SET));
  UINT8 *data=malloc((size_t)size);CHECK(data);
  CHECK(fread(data,1,(size_t)size,file)==(size_t)size && fgetc(file)==EOF);fclose(file);
  *length=(size_t)size;return data;
}
static VOID load_fixture_assets(const char *prom_path,const char *assets_path) {
  static const char *names[]={"booter_load_ad102.bin","v67-ad102-610.43.03-four-plm.bin",
    "gsp_ga10x.bin","gsp_boot_ad102.bin","gsp_boot_desc_ad102.bin"};
  static const char *full_names[]={"gpu-unlock/610.43.03/booter_load_ad102.bin",
    "gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin","gpu-unlock/610.43.03/gsp_ga10x.bin",
    "gpu-unlock/610.43.03/gsp_boot_ad102.bin","gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"};
  size_t length;UINT8 *rom=read_file(prom_path,&length);CHECK(length==sizeof(prom));memcpy(prom,rom,length);free(rom);
  for(UINTN i=0;i<5;i++) {
    char path[4096];int count=snprintf(path,sizeof(path),"%s/%s",assets_path,names[i]);
    CHECK(count>0 && (size_t)count<sizeof(path));GuAssets[i].name=full_names[i];
    GuAssets[i].firmware.data=read_file(path,&GuAssets[i].firmware.size);
  }
}
int main(int argc,char **argv) {
  setvbuf(stdout,NULL,_IONBF,0);CHECK(argc==3 || argc==4);load_fixture_assets(argv[1],argv[2]);
  if(argc==4) {
    FILE *capture=fopen(argv[3],"r");CHECK(capture);char line[512];unsigned count=0,o,a,b,c,d;
    while(fgets(line,sizeof(line),capture))if(sscanf(line,"config[%x]=%x %x %x %x",&o,&a,&b,&c,&d)==5) {
      CHECK(o==count*4 && count<=60);core_captured_config[count++]=a;
      core_captured_config[count++]=b;core_captured_config[count++]=c;core_captured_config[count++]=d;
    }
    fclose(capture);CHECK(count==64);core_use_capture=TRUE;
  }
#ifdef GU_ENTRY_LATCHED
  for(unsigned permutation=0;permutation<3;permutation++) {
    test_entry_ss0=permutation==2?0x23603005U:permutation==1?0x23613005U:0x62543025U;test_entry_ss1=4;
#endif
  for(int mode=SUCCESS;mode<SCENARIO_COUNT;mode++) {
#ifdef GU_ENTRY_LATCHED
    if(permutation && mode>=REBAR_WRITE_0)continue;
#endif
    setup((enum scenario)mode);UINT64 mask=0xffffffffffffULL;
    CHECK(le32(config)==0x278310deU && le32(config+0x2c)==0x413b1458U && registers[SS1/4]==(mode==CORE_ENTRY_SS1?5U:4U));
    struct pci_dev device={.vendor=0x10de,.device=0x2783,.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},.pm_cap=0x60,.refs=1};
    struct gpu_unlock_610_shared shared={0};
    int result=gpu_unlock_610_load_shared(&device,&shared);CHECK(!result);
    result=gpu_unlock_610_unlock(&device,&shared);gpu_unlock_610_release_shared(&shared);
    BOOLEAN safe=GuSafeToReturn();
    if(mode==SUCCESS || mode==FWSEC_RUNNING) {
      CHECK(!result && safe && Gu.started && Gu.quiesced && !Gu.fault && !Gu.retained);
      CHECK(fake.gsp_starts==1 && fake.sec2_starts==1 && fake.flrs==1 && fake.radix_bytes==0x5053000);
      CHECK(!fake.live_maps && !fake.live_buffers && !Gu.allocations && !fake.pool_live);
      CHECK(fake.unmaps==fake.maps && fake.frees==fake.buffer_count);
      CHECK(registers[SS0/4]==0x88888888 && registers[SS1/4]==8);
      CHECK(registers[0x823818/4]==0 && registers[0x110100/4]&0x10);
      CHECK(!(registers[0x1fa828/4]>>4));
      CHECK(mask==0xffffffffffffULL && device.dev.coherent_dma_mask==0xfffffffffULL && device.refs==1);
      CHECK(!(config[4]&4) && fake.rebar_verified==7 && fake.bar_writes==6 && fake.decode_enables==1);
      for(UINTN i=0;i<7;i++)CHECK(le32(config+rebar_offsets[i])==rebar_words[i] && fake.rebar_reads[i]);
      CHECK(!memcmp(config+0x10,CapturedConfig4070S+0x10,24));
    } else {
      CHECK(result!=0);
      if(!Gu.started) {
        CHECK(safe && !Gu.retained && !fake.live_maps && !fake.live_buffers && !fake.pool_live);
        CHECK(!fake.gsp_starts && !fake.sec2_starts && !fake.flrs && !fake.mmio_writes);
      } else {
        CHECK(!safe && Gu.retained && Gu.allocations);
        CHECK(fake.live_maps && fake.live_buffers && fake.pool_live);
        CHECK(!fake.frees);
      }
      if(mode==FWSEC_TIMEOUT)CHECK(fake.gsp_starts==1 && !fake.sec2_starts && !fake.flrs && fake.elapsed>=5000000);
      if(mode==MAP_FAIL || mode==MAP_PARTIAL)CHECK(!Gu.started && !fake.pci_writes);
      if(mode==MARKER_MISSING || mode==HALT_MISSING || mode==WPR_CFG_CLOSED || mode==WPR_CLOSED || mode==FEAT_CLOSED) {
        CHECK(fake.sec2_starts==1 && !fake.flrs);
        CHECK(registers[SS0/4]!=0x88888888 && registers[SS1/4]==4);
      }
      if(mode==READOUT_UNCHANGED)CHECK(!fake.flrs);
      if(mode==FWSEC_NO_WPR || mode==FWSEC_ERROR_CODE)CHECK(fake.gsp_starts==1 && !fake.sec2_starts && !fake.flrs);
      if(mode>=REBAR_WRITE_0 && mode<=REBAR_DECODE_STUCK) {
        CHECK(fake.flrs==1 && fake.gsp_starts==1 && fake.sec2_starts==1);
        CHECK(!fake.bar_writes && !fake.decode_enables && !Gu.quiesced);
        if(mode!=REBAR_DECODE_STUCK)CHECK(!(config[4]&7));
        if(mode>=REBAR_DRIFT_HEADER)for(UINTN i=0;i<3;i++)CHECK(!fake.rebar_writes[i]);
        if(mode<=REBAR_READBACK_ERROR_2 || (mode>=REBAR_READ_ERROR_HEADER && mode<=REBAR_READ_ERROR_CTL2))CHECK(fake.config_error_injected);
      }
      if(mode>=FLR_IDENTITY_TIMEOUT) {
        CHECK(fake.flrs==1 && fake.gsp_starts==1 && fake.sec2_starts==1 && Gu.fault && !Gu.quiesced);
        CHECK(!fake.bar_writes && !fake.decode_enables);
        for(UINTN i=0;i<3;i++)CHECK(!fake.rebar_writes[i]);
        if(mode!=FLR_IDENTITY_WRONG)CHECK(fake.delay_since_flr>=1100000);
      }
    }
    /* App-level containment must work even when the normal I/O path is fault-latched. */
    if(mode>=FLR_IDENTITY_TIMEOUT)CHECK(EFI_ERROR(GuClearMaster()) && !(config[4]&4));
    else CHECK(!EFI_ERROR(GuClearMaster()) && !(config[4]&4));
    if(!safe)CHECK(!GuSafeToReturn());
    CHECK(fake.gsp_starts<=1 && fake.sec2_starts<=1 && fake.flrs<=1);
    CHECK(!fake.pio_reads && !fake.pio_writes && !fake.dmem_reads);
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
    for(UINTN i=0;i<fake.trace_count;i++)CHECK(fake.write_trace[i][0] || fake.write_trace[i][2]!=SS1);
#else
    if(mode==SUCCESS || mode==FWSEC_RUNNING) {
      UINTN ss1_writes=0;
      for(UINTN i=0;i<fake.trace_count;i++)if(!fake.write_trace[i][0] && fake.write_trace[i][2]==SS1) {
        CHECK(fake.write_trace[i][3]==8);ss1_writes++;
      }
      CHECK(ss1_writes==1);
    }
#endif
#ifdef GU_ENTRY_LATCHED
    if(mode==SELECTOR_ENTRY_SS0 || mode==SELECTOR_ENTRY_SS1) {
      CHECK(registers[SS0/4]!=0x88888888 && !fake.flrs);
    }
    if(mode==PRE_FLR_SS1 || mode==LATE_FLR_SS1)CHECK(!fake.flrs);
    if(mode==SELECTOR_LOST)CHECK(fake.flrs==1 && result && !Gu.quiesced);
#endif
    printf("EFI integration %s: result=%d safe=%u retained=%u live_maps=%zu starts=%zu/%zu flr=%zu PASS\n",
      case_names[mode],result,safe,Gu.retained,fake.live_maps,fake.gsp_starts,fake.sec2_starts,fake.flrs);
    dispose_host_fixture();
  }
#ifdef GU_ENTRY_LATCHED
  }
#endif
  for(UINTN i=0;i<5;i++)free((VOID *)GuAssets[i].firmware.data);
  printf("%u real core + compat + UEFI port scenarios passed; all PCI/MMIO/DMA/firmware execution simulated.\n",
#ifdef GU_ENTRY_LATCHED
    REBAR_WRITE_0*3+SCENARIO_COUNT-REBAR_WRITE_0
#else
    SCENARIO_COUNT
#endif
  );
  return 0;
}
