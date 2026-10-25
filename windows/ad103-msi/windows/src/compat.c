#include "compat/compat.h"
GU_ASSET GuAssets[5];
void *devres_open_group(struct device *D,void *Id,int Flags) {
  (void)Flags;if(Id || D->scope)return NULL;D->scope=true;return &D->scope;
}
int devres_release_group(struct device *D,void *Group) {
  if(Group!=&D->scope || !D->scope)return -EINVAL;D->scope=false;return 0;
}
void usleep_range(unsigned long A,unsigned long B) {(void)B;GuStall(A);}
ktime_t ktime_get(void) {return Gu.ticks;}
u32 readl(const void *P) {
  UINT64 Address=(UINTN)P;
  if(Address<Gu.bar_base || Address-Gu.bar_base>0xffffffffULL){Gu.fault=TRUE;return ~0U;}
  return GuRead32((UINT32)(Address-Gu.bar_base));
}
void writel(u32 Value,void *P) {
  UINT64 Address=(UINTN)P;
  if(Address<Gu.bar_base || Address-Gu.bar_base>0xffffffffULL){Gu.fault=TRUE;return;}
  GuWrite32((UINT32)(Address-Gu.bar_base),Value);
}
VOID GuCopyFromIo(VOID *D,const VOID *S,size_t N) {
  UINT64 Address=(UINTN)S,Offset;UINTN Done=0;
  memset(D,0,N);
  /* Both callers read PROM. Byte accesses are unreliable on some GPUs;
     use aligned 32-bit transactions for both validation and FWSEC loading. */
  if(Gu.fault || !Gu.pci || !Gu.pci->Mem.Read || ((Address|(UINTN)D|N)&3) || Address<Gu.bar_base || Address-Gu.bar_base>Gu.bar_size ||
     N>Gu.bar_size-(Address-Gu.bar_base)){Gu.fault=TRUE;return;}
  Offset=Address-Gu.bar_base;
  while(Done<N) {
    UINTN Count=N-Done<1024?N-Done:1024;
    if(Gu.pci->Mem.Read(Gu.pci,EfiPciIoWidthUint32,0,Offset+Done,Count/4,(UINT8 *)D+Done)!=EFI_SUCCESS) {
      Gu.fault=TRUE;return;
    }
    Done+=Count;
  }
}
void *dma_alloc_coherent(struct device *D,size_t N,dma_addr_t *Address,int Flags) {
  VOID *Cpu=NULL;(void)Flags;
  if(!D->dma_mask || *D->dma_mask!=0xffffffffULL || D->coherent_dma_mask!=0xffffffffULL)return NULL;
  if(GuDmaAllocate(N,&Cpu,Address)!=EFI_SUCCESS)return NULL;return Cpu;
}
void dma_free_coherent(struct device *D,size_t N,void *P,dma_addr_t A) {
  (void)D;(void)N;if(GuDmaFree(P,A)!=EFI_SUCCESS){Gu.fault=TRUE;Gu.retained=TRUE;}
}
int dma_set_mask(struct device *D,u64 Mask) {if(!D->dma_mask)return -EINVAL;*D->dma_mask=Mask;return 0;}
int dma_set_coherent_mask(struct device *D,u64 Mask) {D->coherent_dma_mask=Mask;return 0;}
void dma_wmb(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if(!Gu.pci || (Gu.pci->Flush && Gu.pci->Flush(Gu.pci)!=EFI_SUCCESS))Gu.fault=TRUE;
}
void *kzalloc(size_t N,int Flags) {(void)Flags;return GuAlloc(N);}
void *kvmalloc(size_t N,int Flags) {(void)Flags;return GuAlloc(N);}
void *kvcalloc(size_t N,size_t S,int Flags) {
  (void)Flags;if(!S || N>(size_t)-1/S)return NULL;return GuAlloc(N*S);
}
int request_firmware(const struct firmware **Out,const char *Name,struct device *D) {
  UINTN I;(void)D;*Out=NULL;
  for(I=0;I<5;I++)if(GuAssets[I].name && !strcmp(Name,GuAssets[I].name) &&
      GuAssets[I].firmware.data && GuAssets[I].firmware.size) {
    *Out=&GuAssets[I].firmware;return 0;
  }
  return -ENOENT;
}
void release_firmware(const struct firmware *F) {(void)F; /* Preloaded, app-owned immutable assets. */}
void __module_get(void *M) {(void)M;Gu.retained=TRUE;}
int pci_read_config_word(struct pci_dev *D,int O,u16 *V) {
  (void)D;return GuConfigRead(EfiPciIoWidthUint16,(UINT32)O,V)!=EFI_SUCCESS?-EIO:0;
}
int pci_read_config_dword(struct pci_dev *D,int O,u32 *V) {
  (void)D;return GuConfigRead(EfiPciIoWidthUint32,(UINT32)O,V)!=EFI_SUCCESS?-EIO:0;
}
int pci_write_config_word(struct pci_dev *D,int O,u16 V) {
  (void)D;return GuConfigWrite(EfiPciIoWidthUint16,(UINT32)O,&V)!=EFI_SUCCESS?-EIO:0;
}
int pcie_capability_read_word(struct pci_dev *D,int O,u16 *V) {
  return Gu.pcie_cap?pci_read_config_word(D,Gu.pcie_cap+O,V):-EIO;
}
int pcie_capability_read_dword(struct pci_dev *D,int O,u32 *V) {
  return Gu.pcie_cap?pci_read_config_dword(D,Gu.pcie_cap+O,V):-EIO;
}
int pci_is_pcie(struct pci_dev *D) {(void)D;return Gu.pcie_cap!=0;}
int pci_select_bars(struct pci_dev *D,int R) {(void)D;(void)R;return Gu.bar_size?1:0;}
int pci_request_region(struct pci_dev *D,int Bar,const char *Name) {
  (void)D;(void)Name;return !Bar && Gu.owned && Gu.display_ok && !Gu.fault?0:-EBUSY;
}
void pci_release_region(struct pci_dev *D,int Bar) {(void)D;(void)Bar; /* App releases PCI protocol after all cleanup. */}
int aperture_remove_conflicting_pci_devices(struct pci_dev *D,const char *Name) {
  (void)D;(void)Name;
  if(Gu.display_check)Gu.display_ok=Gu.display_check();
  return Gu.display_ok?0:-EBUSY;
}
int pci_enable_device_mem(struct pci_dev *D) {(void)D;return -EOPNOTSUPP;}
void pci_disable_device(struct pci_dev *D) {(void)D;}
resource_size_t pci_resource_len(struct pci_dev *D,int Bar) {(void)D;return Bar?0:Gu.bar_size;}
void *pci_iomap(struct pci_dev *D,int Bar,unsigned long N) {
  (void)D;(void)N;return !Bar && Gu.owned && !Gu.fault?(VOID *)(UINTN)Gu.bar_base:NULL;
}
void pci_iounmap(struct pci_dev *D,void *P) {(void)D;(void)P;}
int pci_save_state(struct pci_dev *D) {(void)D;return GuSavePci()!=EFI_SUCCESS?-EIO:0;}
void pci_restore_state(struct pci_dev *D) {(void)D;if(GuRestorePci()!=EFI_SUCCESS)Gu.fault=TRUE;}
void pci_set_master(struct pci_dev *D) {
  (void)D;(void)GuEnableMaster();
}
void pci_clear_master(struct pci_dev *D) {(void)D;(void)GuClearMaster();}
int pcie_flr(struct pci_dev *D) {(void)D;return GuFlr()!=EFI_SUCCESS?-EIO:0;}
struct pci_dev *pci_dev_get(struct pci_dev *D) {D->refs++;return D;}
void pci_dev_put(struct pci_dev *D) {D->refs--;}

/* Mirror the shared transaction's firmware lifetime, never infer it from logs. */
bool gpu_unlock_platform_fault(void) { return Gu.fault; }
void gpu_unlock_platform_started(void) { GuStarted(); }
void gpu_unlock_platform_quiesced(void) { GuQuiesced(); }
