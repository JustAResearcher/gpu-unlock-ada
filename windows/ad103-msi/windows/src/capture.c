/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded diagnostic snapshots. Nothing here authorizes driver release. */
#include "capture.h"
#include "capture_pe.h"
#include "app_io.h"
#include "runtime.h"
#include "format.h"
#include "sha256.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

#if GU_ENABLE_UNLOCK
#error Firmware metadata capture must not be compiled into an unlock image
#endif
#define MAX_HANDLES 4096
#define MAX_EDGES 4096
#define MAX_ENDPOINTS 128
#define MAX_DRIVERS 32
#define MAP_BYTES (256U*1024U)
typedef struct {
  EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;
} EDGE;
typedef struct {UINTN count,handles,protocols,opens;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_STATUS io,metadata;
  UINT8 *map;UINTN map_bytes,descriptor_bytes;
  EFI_HANDLE endpoints[MAX_ENDPOINTS],drivers[MAX_DRIVERS];UINTN endpoints_count,drivers_count;
} CAPTURE;
static CAPTURE C;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,PciGuid=EFI_PCI_IO_PROTOCOL_GUID,
  BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static VOID Emit(const char *Format,...) {
  if(EFI_ERROR(C.io))return;
  char text[1024];va_list args;va_start(args,Format);
  size_t n=GuFormatV(text,sizeof(text),Format,args);va_end(args);
  C.io=n>=sizeof(text)?EFI_BAD_BUFFER_SIZE:GuCaptureWrite(text);
}
static VOID Fail(EFI_STATUS Status) {if(EFI_ERROR(Status)&&!EFI_ERROR(C.metadata))C.metadata=Status;}
static VOID Free(VOID *Buffer) {if(Buffer)Fail(Gu.bs->FreePool(Buffer));}
static VOID Hex(const UINT8 *Data,UINTN Bytes,char *Text) {
  static const char digits[]="0123456789abcdef";
  for(UINTN i=0;i<Bytes;i++){Text[2*i]=digits[Data[i]>>4];Text[2*i+1]=digits[Data[i]&15];}
  Text[2*Bytes]=0;
}
static VOID Hash(const char *Kind,EFI_HANDLE Handle,const VOID *Data,UINTN Bytes) {
  UINT8 digest[32];char text[65];GuSha256(Data,Bytes,digest);Hex(digest,32,text);
  Emit("HASH kind=%s handle=%#llx bytes=%zu sha256=%s\n",Kind,(unsigned long long)(UINTN)Handle,(size_t)Bytes,text);
}
static VOID GuidLine(const char *Kind,UINTN Index,const EFI_GUID *G) {
  Emit("GUID kind=%s index=%zu value=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",Kind,(size_t)Index,
    G->Data1,(unsigned)G->Data2,(unsigned)G->Data3,(unsigned)G->Data4[0],(unsigned)G->Data4[1],
    (unsigned)G->Data4[2],(unsigned)G->Data4[3],(unsigned)G->Data4[4],(unsigned)G->Data4[5],(unsigned)G->Data4[6],(unsigned)G->Data4[7]);
}
static BOOLEAN Passive(UINT32 Attributes){return Attributes==1||Attributes==2||Attributes==4;}
static EFI_STATUS Snapshot(GRAPH *Graph) {
  EFI_HANDLE *handles=NULL;UINTN count=0;memset(Graph,0,sizeof(*Graph));
  EFI_STATUS status=Gu.bs->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(!EFI_ERROR(status)&&(!handles||!count||count>MAX_HANDLES))status=EFI_BAD_BUFFER_SIZE;
  Graph->handles=count;
  for(UINTN i=0;!EFI_ERROR(status)&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    if(!handles[i]){status=EFI_COMPROMISED_DATA;break;}
    for(UINTN j=0;j<i;j++)if(handles[j]==handles[i])status=EFI_COMPROMISED_DATA;
    if(EFI_ERROR(status))break;
    status=Gu.bs->ProtocolsPerHandle(handles[i],&protocols,&pc);
    if(!EFI_ERROR(status)&&(!protocols||!pc||pc>64||Graph->protocols+pc>16384))status=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;!EFI_ERROR(status)&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){status=EFI_COMPROMISED_DATA;break;}
      for(UINTN k=0;k<j;k++)if(!memcmp(protocols[k],protocols[j],sizeof(EFI_GUID)))status=EFI_COMPROMISED_DATA;
      if(EFI_ERROR(status))break;
      status=Gu.bs->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      if(!EFI_ERROR(status)&&(ec>128||(ec&&!entries)||Graph->opens+ec>131072))status=EFI_BAD_BUFFER_SIZE;
      if(!EFI_ERROR(status))for(UINTN k=0;k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(!e->OpenCount){status=EFI_COMPROMISED_DATA;break;}
        if(Passive(e->Attributes))continue;
        if(Graph->count==MAX_EDGES){status=EFI_BAD_BUFFER_SIZE;break;}
        EDGE *edge=&Graph->edges[Graph->count++];
        edge->handle=handles[i];edge->agent=e->AgentHandle;edge->controller=e->ControllerHandle;
        edge->protocol=*protocols[j];edge->attributes=e->Attributes;edge->count=e->OpenCount;
      }
      Graph->opens+=ec;Free(entries);
    }
    Graph->protocols+=pc;Free(protocols);
  }
  Free(handles);return status;
}
static BOOLEAN SameEdge(const EDGE *A,const EDGE *B) {
  return A->handle==B->handle&&A->agent==B->agent&&A->controller==B->controller&&
    A->attributes==B->attributes&&A->count==B->count&&!memcmp(&A->protocol,&B->protocol,sizeof(EFI_GUID));
}
static BOOLEAN SameGraph(const GRAPH *A,const GRAPH *B) {
  if(A->count!=B->count)return FALSE;
  for(UINTN i=0;i<A->count;i++) {
    UINTN matches=0;for(UINTN j=0;j<B->count;j++)if(SameEdge(&A->edges[i],&B->edges[j]))matches++;
    if(matches!=1)return FALSE;
  }
  return TRUE;
}
static VOID LogGraph(const GRAPH *Graph,UINTN Phase,EFI_STATUS Status) {
  Emit("GRAPH phase=%zu status=%#llx handles=%zu protocols=%zu opens=%zu active_edges=%zu\n",(size_t)Phase,
    (unsigned long long)Status,(size_t)Graph->handles,(size_t)Graph->protocols,(size_t)Graph->opens,(size_t)Graph->count);
  for(UINTN i=0;i<Graph->count&&!EFI_ERROR(C.io);i++) {
    const EDGE *e=&Graph->edges[i];
    Emit("EDGE phase=%zu index=%zu handle=%#llx agent=%#llx controller=%#llx attributes=%#x count=%u\n",
      (size_t)Phase,(size_t)i,(unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,
      (unsigned long long)(UINTN)e->controller,e->attributes,e->count);
    GuidLine("edge",i,&e->protocol);
  }
}
static BOOLEAN Contains(EFI_HANDLE *Handles,UINTN Count,EFI_HANDLE Handle) {
  for(UINTN i=0;i<Count;i++)if(Handles[i]==Handle)return TRUE;return FALSE;
}
static EFI_STATUS Add(EFI_HANDLE *Handles,UINTN *Count,UINTN Max,EFI_HANDLE Handle) {
  if(!Handle||Contains(Handles,*Count,Handle))return EFI_SUCCESS;
  if(*Count==Max)return EFI_BAD_BUFFER_SIZE;Handles[(*Count)++]=Handle;return EFI_SUCCESS;
}
static EFI_STATUS Related(const GRAPH *Graph) {
  EFI_HANDLE roots[]={Gu.handle,C.table->ConsoleOutHandle,C.table->ConsoleInHandle,C.table->StandardErrorHandle};
  for(UINTN i=0;i<4;i++)Fail(Add(C.endpoints,&C.endpoints_count,MAX_ENDPOINTS,roots[i]));
  UINTN old;
  do {
    old=C.endpoints_count;
    for(UINTN i=0;i<Graph->count;i++) {
      const EDGE *e=&Graph->edges[i];if(!Contains(C.endpoints,C.endpoints_count,e->handle))continue;
      EFI_STATUS status=Add(C.drivers,&C.drivers_count,MAX_DRIVERS,e->agent);if(EFI_ERROR(status))return status;
      status=Add(C.endpoints,&C.endpoints_count,MAX_ENDPOINTS,e->controller);if(EFI_ERROR(status))return status;
    }
  }while(old!=C.endpoints_count);
  return C.metadata;
}
static EFI_STATUS MemoryMap(VOID) {
  if(!Gu.bs->GetMemoryMap)return EFI_UNSUPPORTED;
  C.map=GuAlloc(MAP_BYTES);if(!C.map)return EFI_OUT_OF_RESOURCES;
  C.map_bytes=MAP_BYTES;UINTN key=0;UINT32 version=0;
  EFI_STATUS status=Gu.bs->GetMemoryMap(&C.map_bytes,(VOID *)C.map,&key,&C.descriptor_bytes,&version);
  if(!EFI_ERROR(status)&&(C.map_bytes>MAP_BYTES||C.descriptor_bytes<sizeof(EFI_MEMORY_DESCRIPTOR)||
     C.descriptor_bytes>256||C.map_bytes%C.descriptor_bytes||C.map_bytes/C.descriptor_bytes>4096||version!=1))status=EFI_COMPROMISED_DATA;
  Emit("MEMORY_MAP status=%#llx bytes=%zu stride=%zu version=%u\n",(unsigned long long)status,(size_t)C.map_bytes,(size_t)C.descriptor_bytes,version);
  if(EFI_ERROR(status)){C.map_bytes=0;C.descriptor_bytes=0;}return status;
}
static BOOLEAN Readable(const VOID *Pointer,UINTN Bytes) {
  UINT64 cursor=(UINTN)Pointer,end;
  if(!cursor||!Bytes||Bytes>~(UINT64)0-cursor||!C.map_bytes||!C.descriptor_bytes)return FALSE;
  end=cursor+Bytes;
  while(cursor<end) {
    UINT64 next=0;UINTN matches=0;
    for(UINTN i=0;i<C.map_bytes;i+=C.descriptor_bytes) {
      EFI_MEMORY_DESCRIPTOR d;memcpy(&d,C.map+i,sizeof(d));
      if(d.NumberOfPages>(~(UINT64)0-d.PhysicalStart)/4096)return FALSE;
      UINT64 limit=d.PhysicalStart+d.NumberOfPages*4096;
      if(cursor<d.PhysicalStart||cursor>=limit)continue;
      if(++matches!=1||d.Attribute&EFI_MEMORY_RP||
        (d.Type!=EfiLoaderCode&&d.Type!=EfiLoaderData&&d.Type!=EfiBootServicesCode&&
         d.Type!=EfiBootServicesData&&d.Type!=EfiRuntimeServicesCode&&d.Type!=EfiRuntimeServicesData))return FALSE;
      next=limit;
    }
    if(matches!=1||next<=cursor)return FALSE;cursor=next;
  }
  return TRUE;
}
static EFI_STATUS Path(const char *Kind,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *PathValue) {
  UINTN bytes=0;BOOLEAN end=FALSE;
  for(UINTN i=0;i<128;i++) {
    if(bytes>4092||!Readable((UINT8 *)PathValue+bytes,4))return EFI_COMPROMISED_DATA;
    EFI_DEVICE_PATH_PROTOCOL n;memcpy(&n,(UINT8 *)PathValue+bytes,4);
    UINTN length=n.Length[0]+((UINTN)n.Length[1]<<8);
    if(length<4||length>4096-bytes||!Readable((UINT8 *)PathValue+bytes,length))return EFI_COMPROMISED_DATA;
    bytes+=length;if(n.Type==END_DEVICE_PATH_TYPE){end=n.SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&length==4;break;}
  }
  if(!end)return EFI_COMPROMISED_DATA;
  Hash(Kind,Handle,PathValue,bytes);
  for(UINTN offset=0;offset<bytes;offset+=32) {
    UINTN size=bytes-offset;if(size>32)size=32;char text[65];Hex((UINT8 *)PathValue+offset,size,text);
    Emit("PATH_BYTES kind=%s handle=%#llx offset=%zu hex=%s\n",Kind,(unsigned long long)(UINTN)Handle,(size_t)offset,text);
  }
  return C.io;
}
static VOID Endpoint(EFI_HANDLE Handle) {
  EFI_GUID **protocols=NULL;UINTN count=0;
  EFI_STATUS status=Gu.bs->ProtocolsPerHandle(Handle,&protocols,&count);
  if(!EFI_ERROR(status)&&(!protocols||!count||count>64))status=EFI_BAD_BUFFER_SIZE;
  Emit("ENDPOINT handle=%#llx status=%#llx protocols=%zu\n",(unsigned long long)(UINTN)Handle,(unsigned long long)status,(size_t)count);
  if(!EFI_ERROR(status))for(UINTN i=0;i<count;i++) {
    if(!protocols[i]){status=EFI_COMPROMISED_DATA;break;}GuidLine("endpoint",i,protocols[i]);
  }
  Fail(status);Free(protocols);
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  status=Gu.bs->HandleProtocol(Handle,&PathGuid,(VOID **)&path);
  Emit("ENDPOINT_PATH handle=%#llx status=%#llx present=%u\n",(unsigned long long)(UINTN)Handle,(unsigned long long)status,(unsigned)(path!=NULL));
  if(status==EFI_UNSUPPORTED||status==EFI_NOT_FOUND)return;
  if(!EFI_ERROR(status))status=path?Path("endpoint",Handle,path):EFI_COMPROMISED_DATA;
  if(!EFI_ERROR(status)) {
    EFI_DEVICE_PATH_PROTOCOL *remaining=path;EFI_HANDLE parent=NULL;
    EFI_STATUS found=Gu.bs->LocateDevicePath(&PciGuid,&remaining,&parent);
    Emit("ENDPOINT_PCI handle=%#llx status=%#llx parent=%#llx target=%#llx\n",(unsigned long long)(UINTN)Handle,
      (unsigned long long)found,(unsigned long long)(UINTN)parent,(unsigned long long)(UINTN)Gu.handle);
  }
  Fail(status);
}
static VOID Console(VOID) {
  EFI_SYSTEM_TABLE *t=C.table;
  Emit("CONSOLE out=%#llx in=%#llx error=%#llx text=%#llx keys=%#llx stderr=%#llx\n",
    (unsigned long long)(UINTN)t->ConsoleOutHandle,(unsigned long long)(UINTN)t->ConsoleInHandle,
    (unsigned long long)(UINTN)t->StandardErrorHandle,(unsigned long long)(UINTN)t->ConOut,
    (unsigned long long)(UINTN)t->ConIn,(unsigned long long)(UINTN)t->StdErr);
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *outputs[]={t->ConOut,t->StdErr};
  for(UINTN i=0;i<2;i++) {
    if(!Readable(outputs[i],sizeof(*outputs[i]))){Fail(EFI_COMPROMISED_DATA);continue;}
    Emit("CONSOLE_CALLBACK kind=%zu reset=%#llx output=%#llx set_mode=%#llx\n",(size_t)i,
      (unsigned long long)(UINTN)outputs[i]->Reset,(unsigned long long)(UINTN)outputs[i]->OutputString,
      (unsigned long long)(UINTN)outputs[i]->SetMode);
  }
  if(!Readable(t->ConIn,sizeof(*t->ConIn))){Fail(EFI_COMPROMISED_DATA);return;}
  Emit("CONSOLE_CALLBACK kind=2 reset=%#llx read_key=%#llx wait=%#llx\n",
    (unsigned long long)(UINTN)t->ConIn->Reset,(unsigned long long)(UINTN)t->ConIn->ReadKeyStroke,
    (unsigned long long)(UINTN)t->ConIn->WaitForKey);
}
static VOID Driver(EFI_HANDLE Handle) {
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
  EFI_STATUS status=Gu.bs->HandleProtocol(Handle,&BindingGuid,(VOID **)&binding);
  Emit("DRIVER agent=%#llx binding_status=%#llx binding=%#llx\n",(unsigned long long)(UINTN)Handle,
    (unsigned long long)status,(unsigned long long)(UINTN)binding);
  if(EFI_ERROR(status)){Fail(status);return;}
  if(!Readable(binding,sizeof(*binding))){Fail(EFI_COMPROMISED_DATA);return;}
  EFI_DRIVER_BINDING_PROTOCOL d=*binding;
  Emit("BINDING agent=%#llx handle=%#llx image=%#llx version=%#x supported=%#llx start=%#llx stop=%#llx\n",
    (unsigned long long)(UINTN)Handle,(unsigned long long)(UINTN)d.DriverBindingHandle,
    (unsigned long long)(UINTN)d.ImageHandle,d.Version,(unsigned long long)(UINTN)d.Supported,
    (unsigned long long)(UINTN)d.Start,(unsigned long long)(UINTN)d.Stop);
  if(d.DriverBindingHandle!=Handle||!d.ImageHandle||!d.Stop){Fail(EFI_COMPROMISED_DATA);return;}
  status=Gu.bs->HandleProtocol(d.ImageHandle,&LoadedGuid,(VOID **)&image);
  Emit("DRIVER_IMAGE agent=%#llx status=%#llx interface=%#llx\n",(unsigned long long)(UINTN)Handle,
    (unsigned long long)status,(unsigned long long)(UINTN)image);
  if(EFI_ERROR(status)){Fail(status);return;}
  if(!Readable(image,sizeof(*image))){Fail(EFI_COMPROMISED_DATA);return;}
  EFI_LOADED_IMAGE_PROTOCOL im=*image;
  Emit("IMAGE agent=%#llx image=%#llx device=%#llx parent=%#llx base=%#llx bytes=%llu code_type=%u data_type=%u\n",
    (unsigned long long)(UINTN)Handle,(unsigned long long)(UINTN)d.ImageHandle,
    (unsigned long long)(UINTN)im.DeviceHandle,(unsigned long long)(UINTN)im.ParentHandle,
    (unsigned long long)(UINTN)im.ImageBase,(unsigned long long)im.ImageSize,(unsigned)im.ImageCodeType,(unsigned)im.ImageDataType);
  if(im.FilePath)Fail(Path("driver-file",Handle,im.FilePath));
  else Emit("DRIVER_FILE agent=%#llx absent=1\n",(unsigned long long)(UINTN)Handle);
  if(!im.ImageSize||im.ImageSize>GU_PE_MAX_IMAGE||!Readable(im.ImageBase,(UINTN)im.ImageSize)) {
    Emit("IMAGE_READ agent=%#llx readable=0\n",(unsigned long long)(UINTN)Handle);Fail(EFI_COMPROMISED_DATA);return;
  }
  Hash("loaded-image",Handle,im.ImageBase,(UINTN)im.ImageSize);
  UINTN base=(UINTN)im.ImageBase,stop=(UINTN)d.Stop;
  UINTN stop_offset=stop>=base&&stop-base<im.ImageSize?stop-base:~(UINTN)0;
  UINTN scratch_size=GuPeScratchBytes((UINTN)im.ImageSize);VOID *scratch=GuAlloc(scratch_size);
  GU_PE_IDENTITY *identity=GuAlloc(sizeof(*identity));
  status=scratch&&identity?GuPeIdentity(im.ImageBase,(UINTN)im.ImageSize,base,stop_offset,scratch,scratch_size,identity):EFI_OUT_OF_RESOURCES;
  Emit("PE agent=%#llx status=%#llx stop_offset=%#llx canonical_valid=%u\n",(unsigned long long)(UINTN)Handle,
    (unsigned long long)status,(unsigned long long)stop_offset,(unsigned)(!EFI_ERROR(status)&&identity->canonical_valid));
  if(!EFI_ERROR(status)) {
    char headers[65],relocations[65];Hex(identity->headers_sha256,32,headers);Hex(identity->relocation_sha256,32,relocations);
    Emit("PE_METADATA_HASH agent=%#llx headers=%s relocations=%s\n",(unsigned long long)(UINTN)Handle,headers,relocations);
    Emit("PE_LAYOUT agent=%#llx preferred_base=%#llx image_bytes=%u headers=%u entry=%#x reloc_rva=%#x reloc_bytes=%u relocations=%u executable_relocations=%u sections=%u stop_section=%u\n",
      (unsigned long long)(UINTN)Handle,(unsigned long long)identity->preferred_base,identity->image_bytes,
      identity->headers_bytes,identity->entry_rva,identity->relocation_rva,identity->relocation_bytes,
      identity->relocation_count,identity->executable_relocations,identity->section_count,identity->stop_section);
    for(UINTN i=0;i<identity->section_count;i++) {
      GU_PE_SECTION *s=&identity->sections[i];char raw[65],canonical[65];
      Hex(s->raw_sha256,32,raw);Hex(s->canonical_sha256,32,canonical);
      Emit("SECTION agent=%#llx index=%zu name=%s rva=%#x bytes=%u flags=%#x executable=%u relocations=%u\n",
        (unsigned long long)(UINTN)Handle,(size_t)i,s->name,s->rva,s->bytes,s->characteristics,(unsigned)s->executable,s->relocations);
      if(s->executable)Emit("SECTION_HASH agent=%#llx index=%zu raw=%s canonical=%s\n",
        (unsigned long long)(UINTN)Handle,(size_t)i,raw,canonical);
    }
  }
  Fail(status);Free(identity);Free(scratch);
}
EFI_STATUS GuCaptureFirmware(EFI_SYSTEM_TABLE *Table) {
  memset(&C,0,sizeof(C));C.table=Table;
  if(!Table||!Gu.bs||Table->BootServices!=Gu.bs||!Gu.handle||Gu.owned||Gu.armed||Gu.started||
     !Gu.bs->LocateHandleBuffer||!Gu.bs->ProtocolsPerHandle||!Gu.bs->OpenProtocolInformation||
     !Gu.bs->HandleProtocol||!Gu.bs->LocateDevicePath)return EFI_UNSUPPORTED;
  EFI_STATUS opened=GuCaptureOpen();if(EFI_ERROR(opened))return opened;
  Emit("CAPTURE_FORMAT version=1 diagnostic=0.4.2 read_only=1 unlock_authorized=0\n");
  Emit("TARGET handle=%#llx application=%#llx\n",(unsigned long long)(UINTN)Gu.handle,(unsigned long long)(UINTN)Gu.image);
  GRAPH *before=GuAlloc(sizeof(GRAPH)),*after=GuAlloc(sizeof(GRAPH));
  BOOLEAN stable=FALSE;EFI_STATUS first=EFI_OUT_OF_RESOURCES,second=EFI_OUT_OF_RESOURCES;
  if(!before||!after){Fail(EFI_OUT_OF_RESOURCES);goto done;}
  first=Snapshot(before);LogGraph(before,0,first);Fail(first);
  if(!EFI_ERROR(first))Fail(Related(before));
  Fail(MemoryMap());Console();
  for(UINTN i=0;i<C.endpoints_count&&!EFI_ERROR(C.io);i++)Endpoint(C.endpoints[i]);
  for(UINTN i=0;i<C.drivers_count&&!EFI_ERROR(C.io);i++)Driver(C.drivers[i]);
  if(!EFI_ERROR(C.io)) {
    second=Snapshot(after);LogGraph(after,1,second);Fail(second);
    stable=!EFI_ERROR(first)&&!EFI_ERROR(second)&&SameGraph(before,after);
  }
done:
  Free(before);Free(after);Free(C.map);C.map=NULL;
  Emit("CAPTURE_END metadata_status=%#llx graph_first=%#llx graph_second=%#llx stable_active_graph=%u endpoints=%zu drivers=%zu gpu_writes=0 unlock_authorized=0\n",
    (unsigned long long)C.metadata,(unsigned long long)first,(unsigned long long)second,(unsigned)stable,
    (size_t)C.endpoints_count,(size_t)C.drivers_count);
  EFI_STATUS closed=GuCaptureClose();
  return EFI_ERROR(C.io)?C.io:EFI_ERROR(closed)?closed:C.metadata;
}
