/* Minimal stand-ins for the DOSBox-X headers used by src/hardware/pvr_pcx.cpp, so that
 * the PowerVR pipeline can be exercised on the host without the emulator. */
#ifndef PVR_TEST_SHIM_H
#define PVR_TEST_SHIM_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

typedef uintptr_t Bitu;
typedef intptr_t Bits;
typedef uint32_t PhysPt;
typedef uint8_t *HostPt;
typedef uint32_t PageNum;

#define LOG(a, b) shim_log
#define LOG_MSG shim_log
enum { LOG_MISC, LOG_PCI };
enum { LOG_DEBUG, LOG_NORMAL, LOG_WARN, LOG_ERROR };
void shim_log(const char *fmt, ...);
[[noreturn]] void E_Exit(const char *fmt, ...);

/* guest physical memory */
extern std::vector<uint8_t> shim_ram;
static inline uint8_t phys_readb(PhysPt a) { return a < shim_ram.size() ? shim_ram[a] : 0xFF; }
static inline uint16_t phys_readw(PhysPt a) { return (uint16_t)(phys_readb(a) | (phys_readb(a + 1) << 8)); }
static inline uint32_t phys_readd(PhysPt a) { return (uint32_t)phys_readw(a) | ((uint32_t)phys_readw(a + 2) << 16); }
static inline void phys_writeb(PhysPt a, uint8_t v) { if (a < shim_ram.size()) shim_ram[a] = v; }
static inline void phys_writew(PhysPt a, uint16_t v) { phys_writeb(a, (uint8_t)v); phys_writeb(a + 1, (uint8_t)(v >> 8)); }
static inline void phys_writed(PhysPt a, uint32_t v) { phys_writew(a, (uint16_t)v); phys_writew(a + 2, (uint16_t)(v >> 16)); }
static inline void host_writew(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void host_writed(uint8_t *p, uint32_t v) { host_writew(p, (uint16_t)v); host_writew(p + 2, (uint16_t)(v >> 16)); }
static inline uint32_t host_readd(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
Bitu MEM_TotalPages(void);
uint32_t MEM_HardwareAllocate(const char *name, uint32_t sz);
static inline PhysPt PAGING_GetPhysicalAddress(PhysPt a) { return a; }
void PAGING_ClearTLB(void);

#define PFLAG_READABLE 1u
#define PFLAG_WRITEABLE 2u
#define PFLAG_NOCODE 0x10u
class PageHandler {
public:
	PageHandler(Bitu f) : flags(f) {}
	virtual ~PageHandler() {}
	virtual uint8_t readb(PhysPt) { return 0xFF; }
	virtual uint16_t readw(PhysPt) { return 0xFFFF; }
	virtual uint32_t readd(PhysPt) { return 0xFFFFFFFF; }
	virtual void writeb(PhysPt, uint8_t) {}
	virtual void writew(PhysPt, uint16_t) {}
	virtual void writed(PhysPt, uint32_t) {}
	virtual HostPt GetHostReadPt(PageNum) { return NULL; }
	virtual HostPt GetHostWritePt(PageNum) { return NULL; }
	Bitu flags;
};
class MEM_CalloutObject;
typedef PageHandler *(MEM_CalloutHandler)(MEM_CalloutObject &co, Bitu phys_page);
class MEM_CalloutObject {
public:
	void Install(Bitu page, Bitu mask, MEM_CalloutHandler *h) { base = page; pmask = mask; handler = h; }
	void Uninstall() { handler = NULL; }
	Bitu base = 0, pmask = 0;
	MEM_CalloutHandler *handler = NULL;
};
typedef int MEM_Callout_t;
static const MEM_Callout_t MEM_Callout_t_none = -1;
enum { MEM_TYPE_PCI };
MEM_Callout_t MEM_AllocateCallout(int);
void MEM_FreeCallout(MEM_Callout_t);
MEM_CalloutObject *MEM_GetCallout(MEM_Callout_t);
void MEM_PutCallout(MEM_CalloutObject *);
static inline Bitu MEMMASK_Range(Bitu x) { return ~(x - 1); }
static inline Bitu MEMMASK_Combine(Bitu a, Bitu b) { return a & b; }
static const Bitu MEMMASK_FULL = 0x000FFFFFu;

void PIC_ActivateIRQ(Bitu irq);
void PIC_DeActivateIRQ(Bitu irq);

class Section {};
class Section_prop : public Section {
public:
	std::string Get_string(const char *) { return "false"; }
	int Get_int(const char *) { return 0; }
	bool Get_bool(const char *) { return false; }
};
struct ShimControl { Section *GetSection(const char *) { return NULL; } };
extern ShimControl *control;
typedef void (*SectionFunc)(Section *);
#define AddExitFunctionFuncPair(x) x
#define AddVMEventFunctionFuncPair(x) x
enum { VM_EVENT_POWERON };
void AddExitFunction(SectionFunc, bool);
void AddVMEventFunction(int, SectionFunc);

class PCI_Device {
public:
	unsigned char config[256];
	unsigned char config_writemask[256];
	PCI_Device(uint16_t vendor, uint16_t device) {
		for (int i = 0; i < 256; i++) config[i] = config_writemask[i] = 0;
		config[0] = (uint8_t)vendor; config[1] = (uint8_t)(vendor >> 8);
		config[2] = (uint8_t)device; config[3] = (uint8_t)(device >> 8);
	}
	virtual ~PCI_Device() {}
	virtual void config_write(uint8_t, Bitu, uint32_t) {}
};
extern bool pcibus_enable;
Bits RegisterPCIDevice(PCI_Device *device, Bits bus = -1, Bits slot = -1);
bool UnregisterPCIDevice(PCI_Device *device);
#endif
