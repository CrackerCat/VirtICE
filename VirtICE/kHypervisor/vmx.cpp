// Copyright (c) 2016-2017, KelvinChan. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.#include <fltKernel.h>


#include <intrin.h>
#include "..\HyperPlatform\util.h"
#include "vmcs.h"
#include "vmx.h"
#include "..\HyperPlatform\vmm.h"
#include "..\HyperPlatform\log.h"
#include "..\HyperPlatform\common.h"
#include "vmx_common.h"
#include "..\HyperPlatform\ept.h"
#include "VirtIce.h"

extern "C"
{

////////////////////////////////////////////////////////////////////////////////////////////////////
//// Prototype
////

extern ULONG		 VmpGetSegmentAccessRight(USHORT segment_selector);
extern ULONG_PTR*	 VmmpSelectRegister(_In_ ULONG index, _In_ GuestContext *guest_context);
extern GpRegisters*  GetGpReg(GuestContext* guest_context);
extern FlagRegister* GetFlagReg(GuestContext* guest_context);
extern KIRQL		 GetGuestIrql(GuestContext* guest_context);
extern ULONG_PTR	 GetGuestCr8(GuestContext* guest_context);
extern VCPUVMX*	 	 GetVcpuVmx(_In_ GuestContext* guest_context);
extern VOID			 SetvCpuVmx(GuestContext* guest_context, VCPUVMX* VCPUVMX);
extern VOID			 EnterVmxMode(GuestContext* guest_context);
extern VOID			 LeaveVmxMode(GuestContext* guest_context);
extern ULONG		 GetvCpuMode(GuestContext* guest_context); 
extern ProcessorData*GetProcessorData(GuestContext* guest_context); 
extern void			 SetEptp02Data(GuestContext *guest_context, ULONG64 pEptData);
extern void			 SetEptp12Data(GuestContext *guest_context, ULONG64 pEptData);
extern EptData*		 GetEptp02Data(GuestContext* guest_context);
extern EptData*		 GetEptp12Data(GuestContext* guest_context); 
extern ULONG64		 GetEptp02(GuestContext* guest_context);
extern ULONG64		 GetEptp12(GuestContext* guest_context);

void				 VmxSaveGuestCr8(VCPUVMX* vcpu, ULONG_PTR cr8);

////////////////////////////////////////////////////////////////////////////////////////////////////
//// Marco
////
 
 
////////////////////////////////////////////////////////////////////////////////////////////////////
//// 
//// Variable
//// 

extern BOOLEAN		 IsEmulateVMExit;


////////////////////////////////////////////////////////////////////////////////////////////////////
//// 
//// Type
////
enum VMX_state
{
	VMCS_STATE_CLEAR = 0,
	VMCS_STATE_LAUNCHED
};

////////////////////////////////////////////////////////////////////////////////////////////////////
//// 
//// Implementation
////

//---------------------------------------------------------------------------------------------------------------------//
VOID	LEAVE_GUEST_MODE(VCPUVMX* vm)
{
	vm->inRoot = RootMode; 
	HYPERPLATFORM_LOG_DEBUG("VMM: %I64x Enter Root mode \r\n", vm);
}


//---------------------------------------------------------------------------------------------------------------------//
VOID	ENTER_GUEST_MODE(VCPUVMX* vm)
{
	vm->inRoot = GuestMode; 
	HYPERPLATFORM_LOG_DEBUG("VMM: %I64x Enter Guest mode \r\n", vm);
} 


//---------------------------------------------------------------------------------------------------------------------//
VMX_MODE GetVmxMode(VCPUVMX* vm)
{ 
	if (vm) 
	{
		return vm->inRoot;
	}
	else
	{
		return VMX_MODE::RootMode;
	}
}
 
//---------------------------------------------------------------------------------------------------------------------//
void DumpVcpu(GuestContext* guest_context)
{
	ULONG64 vmcs12_va = 0;
	ULONG64 vmcs_pa;
	VCPUVMX* vmx = NULL; 
	if (!guest_context)
	{
		HYPERPLATFORM_LOG_DEBUG_SAFE("GuestContex Empty");
		return;
	}

	vmx = GetVcpuVmx(guest_context);
	__vmx_vmptrst(&vmcs_pa);

	HYPERPLATFORM_LOG_DEBUG_SAFE("CurrentVmcs: %I64X vm: %I64x vmcs02: %I64X vmcs01: %I64x vmcs12: %I64x root mode: %I64x \r\n",
		vmcs_pa, vmx, vmx->vmcs02.VmcsPa, vmx->vmcs01.VmcsPa, vmx->vmcs12.VmcsPa, vmx->inRoot, GetvCpuMode(guest_context));
}
//----------------------------------------------------------------------------------------------------------------------//
/*
Descritpion:

1. Call before emulate a VMExit, Read All VMExit related-Information
From VMCS0-2, And backup it into VMCS1-2, the purpose is for
emulate VMExit,

2. Actually the Emulation of VMExit is that we RESUME the L0 to L1,
so when L1 make any VMREAD/WRITE,  will trap by us, we return a
VMCS1-2 to its.

Parameters:

1. VMExit Reason
2. Physical Address for VMCS1-2

*/
NTSTATUS VmxSaveExceptionInformationFromVmcs02(VCPUVMX* vcpu)
{
	ULONG_PTR vmcs12_va = 0;
	//all nested vm-exit should record 
	if (!vcpu)
	{
		return STATUS_UNSUCCESSFUL;
	}

	if (!vcpu->vmcs12.VmcsPa)
	{
		return STATUS_UNSUCCESSFUL;
	}

	vmcs12_va = (ULONG_PTR)UtilVaFromPa(vcpu->vmcs12.VmcsPa);

	if (!vmcs12_va)
	{
		return STATUS_UNSUCCESSFUL;
	}
	const VmExitInformation exit_reason = {
		UtilVmRead(VmcsField::kVmExitReason)
	};
	
	const VmExitInterruptionInformationField exception = {
		UtilVmRead(VmcsField::kVmExitIntrInfo)
	};

	ULONG_PTR vmexit_qualification = UtilVmRead(VmcsField::kExitQualification);

	VmWrite32(VmcsField::kVmExitIntrInfo, vmcs12_va, exception.all);
	VmWrite32(VmcsField::kVmExitReason, vmcs12_va, exit_reason.all);
	VmWrite32(VmcsField::kExitQualification, vmcs12_va, vmexit_qualification);
	VmWrite32(VmcsField::kVmExitInstructionLen, vmcs12_va, UtilVmRead(VmcsField::kVmExitInstructionLen));
	VmWrite32(VmcsField::kVmInstructionError, vmcs12_va, UtilVmRead(VmcsField::kVmInstructionError));
	VmWrite32(VmcsField::kVmExitIntrErrorCode, vmcs12_va, UtilVmRead(VmcsField::kVmExitIntrErrorCode));
	VmWrite32(VmcsField::kIdtVectoringInfoField, vmcs12_va, UtilVmRead(VmcsField::kIdtVectoringInfoField));
	VmWrite32(VmcsField::kIdtVectoringErrorCode, vmcs12_va, UtilVmRead(VmcsField::kIdtVectoringErrorCode));
	VmWrite32(VmcsField::kVmxInstructionInfo, vmcs12_va, UtilVmRead(VmcsField::kVmxInstructionInfo));
	VmWrite64(VmcsField::kGuestPhysicalAddress,vmcs12_va, UtilVmRead64(VmcsField::kGuestPhysicalAddress));
	VmWrite64(VmcsField::kGuestLinearAddress, vmcs12_va, UtilVmRead64(VmcsField::kGuestLinearAddress));
}
//---------------------------------------------------------------------------------------------------------------------//

/*
Descritpion:
1.  Call before emulate a VMExit, Read All Guest Field From VMCS0-2,
And backup into VMCS1-2, the purpose is for emulated VMExit, but
actually we RESUME the VM to L1, when L1 make any VMREAD/WRITE,
we return a VMCS1-2 to its.

Parameters:
1.	Physical Address for VMCS1-2

*/
NTSTATUS VmxSaveGuestFieldFromVmcs02(VCPUVMX* vcpu)
{
	ULONG_PTR vmcs12_va = 0;
	//all nested vm-exit should record 
	if (!vcpu)
	{
		return STATUS_UNSUCCESSFUL;
	}

	if (!vcpu->vmcs12.VmcsPa)
	{
		return STATUS_UNSUCCESSFUL;
	}

	vmcs12_va = (ULONG_PTR)UtilVaFromPa(vcpu->vmcs12.VmcsPa);

	if (!vmcs12_va)
	{
		return STATUS_UNSUCCESSFUL;
	}

 
	VmWrite64(VmcsField::kGuestRip, vmcs12_va, UtilVmRead(VmcsField::kGuestRip));
	VmWrite64(VmcsField::kGuestRsp, vmcs12_va, UtilVmRead(VmcsField::kGuestRsp));
	VmWrite64(VmcsField::kGuestCr3, vmcs12_va, UtilVmRead(VmcsField::kGuestCr3));
	VmWrite64(VmcsField::kGuestCr0, vmcs12_va, UtilVmRead(VmcsField::kGuestCr0));
	VmWrite64(VmcsField::kGuestCr4, vmcs12_va, UtilVmRead(VmcsField::kGuestCr4));
	VmWrite64(VmcsField::kGuestDr7, vmcs12_va, UtilVmRead(VmcsField::kGuestDr7));
	VmWrite64(VmcsField::kGuestRflags, vmcs12_va, UtilVmRead(VmcsField::kGuestRflags));

	VmWrite16(VmcsField::kGuestEsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestEsSelector));
	VmWrite16(VmcsField::kGuestCsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestCsSelector));
	VmWrite16(VmcsField::kGuestSsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestSsSelector));
	VmWrite16(VmcsField::kGuestDsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestDsSelector));
	VmWrite16(VmcsField::kGuestFsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestFsSelector));
	VmWrite16(VmcsField::kGuestGsSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestGsSelector));
	VmWrite16(VmcsField::kGuestLdtrSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestLdtrSelector));
	VmWrite16(VmcsField::kGuestTrSelector, vmcs12_va, UtilVmRead(VmcsField::kGuestTrSelector));

	VmWrite32(VmcsField::kGuestEsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestEsLimit));
	VmWrite32(VmcsField::kGuestCsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestCsLimit));
	VmWrite32(VmcsField::kGuestSsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestSsLimit));
	VmWrite32(VmcsField::kGuestDsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestDsLimit));
	VmWrite32(VmcsField::kGuestFsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestFsLimit));
	VmWrite32(VmcsField::kGuestGsLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestGsLimit));
	VmWrite32(VmcsField::kGuestLdtrLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestLdtrLimit));
	VmWrite32(VmcsField::kGuestTrLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestTrLimit));
	VmWrite32(VmcsField::kGuestGdtrLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestGdtrLimit));
	VmWrite32(VmcsField::kGuestIdtrLimit, vmcs12_va, UtilVmRead(VmcsField::kGuestIdtrLimit));

	VmWrite32(VmcsField::kGuestEsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestEsArBytes));
	VmWrite32(VmcsField::kGuestCsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestCsArBytes));
	VmWrite32(VmcsField::kGuestSsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestSsArBytes));
	VmWrite32(VmcsField::kGuestDsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestDsArBytes));
	VmWrite32(VmcsField::kGuestFsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestFsArBytes));
	VmWrite32(VmcsField::kGuestGsArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestGsArBytes));
	VmWrite32(VmcsField::kGuestLdtrArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestLdtrArBytes));

	VmWrite32(VmcsField::kGuestTrArBytes, vmcs12_va, UtilVmRead(VmcsField::kGuestTrArBytes));

	VmWrite32(VmcsField::kGuestInterruptibilityInfo, vmcs12_va, UtilVmRead(VmcsField::kGuestInterruptibilityInfo));
	VmWrite32(VmcsField::kGuestActivityState, vmcs12_va, UtilVmRead(VmcsField::kGuestActivityState));
	VmWrite32(VmcsField::kGuestSysenterCs, vmcs12_va, UtilVmRead(VmcsField::kGuestSysenterCs));

	VmWrite64(VmcsField::kGuestSysenterEsp, vmcs12_va, UtilVmRead(VmcsField::kGuestSysenterEsp));
	VmWrite64(VmcsField::kGuestSysenterEip, vmcs12_va, UtilVmRead(VmcsField::kGuestSysenterEip));
	VmWrite64(VmcsField::kGuestPendingDbgExceptions, vmcs12_va, UtilVmRead(VmcsField::kGuestPendingDbgExceptions));
	VmWrite64(VmcsField::kGuestEsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestEsBase));
	VmWrite64(VmcsField::kGuestCsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestCsBase));
	VmWrite64(VmcsField::kGuestSsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestSsBase));
	VmWrite64(VmcsField::kGuestDsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestDsBase));
	VmWrite64(VmcsField::kGuestFsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestFsBase));
	VmWrite64(VmcsField::kGuestGsBase, vmcs12_va, UtilVmRead(VmcsField::kGuestGsBase));
	VmWrite64(VmcsField::kGuestLdtrBase, vmcs12_va, UtilVmRead(VmcsField::kGuestLdtrBase));
	VmWrite64(VmcsField::kGuestTrBase, vmcs12_va, UtilVmRead(VmcsField::kGuestTrBase));
	VmWrite64(VmcsField::kGuestGdtrBase, vmcs12_va, UtilVmRead(VmcsField::kGuestGdtrBase));
	VmWrite64(VmcsField::kGuestIdtrBase, vmcs12_va, UtilVmRead(VmcsField::kGuestIdtrBase));
	
	VmWrite64(VmcsField::kGuestIa32Efer, vmcs12_va, UtilVmRead(VmcsField::kGuestIa32Efer));

	/*
	VmWrite64(VmcsField::kGuestPdptr0, vmcs12_va, UtilVmRead(VmcsField::kGuestPdptr0));
	VmWrite64(VmcsField::kGuestPdptr1, vmcs12_va, UtilVmRead(VmcsField::kGuestPdptr1));
	VmWrite64(VmcsField::kGuestPdptr2, vmcs12_va, UtilVmRead(VmcsField::kGuestPdptr2));
	VmWrite64(VmcsField::kGuestPdptr3, vmcs12_va, UtilVmRead(VmcsField::kGuestPdptr3));
	*/
}

//---------------------------------------------------------------------------------------------------------------------//
NTSTATUS VmxLoadHostStateForLevel1(
	_In_ VCPUVMX* vcpu
)
{ 
	ULONG_PTR Vmcs01_pa = 0;
	ULONG_PTR Vmcs12_va = 0;

	if (!vcpu || !vcpu->vmcs01.VmcsPa || !vcpu->vmcs12.VmcsPa)
	{
		HYPERPLATFORM_COMMON_DBG_BREAK();
		return STATUS_UNSUCCESSFUL;
	}

	Vmcs01_pa = vcpu->vmcs01.VmcsPa;
	Vmcs12_va = (ULONG_PTR)UtilVaFromPa(vcpu->vmcs12.VmcsPa);

	if (!Vmcs01_pa || !Vmcs12_va)
	{
		HYPERPLATFORM_COMMON_DBG_BREAK();
		return STATUS_UNSUCCESSFUL;
	}


	VmxStatus status;

	// Host Data Field  
	ULONG64   VMCS12_HOST_RIP = 0;
	ULONG64   VMCS12_HOST_STACK = 0;
	ULONG_PTR VMCS12_HOST_RFLAGs = 0;
	ULONG64   VMCS12_HOST_CR4 = 0;
	ULONG64   VMCS12_HOST_CR3 = 0;
	ULONG64   VMCS12_HOST_CR0 = 0;
	
	ULONG64   VMCS12_HOST_CS = 0;
	ULONG64   VMCS12_HOST_SS = 0;
	ULONG64   VMCS12_HOST_DS = 0;
	ULONG64   VMCS12_HOST_ES = 0;
	ULONG64   VMCS12_HOST_FS = 0;
	ULONG64   VMCS12_HOST_GS = 0;
	ULONG64   VMCS12_HOST_TR = 0;

	ULONG32   VMCS12_HOST_SYSENTER_CS = 0;
	ULONG64   VMCS12_HOST_SYSENTER_RIP = 0;
	ULONG64   VMCS12_HOST_SYSENTER_RSP = 0;

	ULONG64   VMCS12_HOST_FS_BASE = 0;
	ULONG64   VMCS12_HOST_GS_BASE = 0;
	ULONG64   VMCS12_HOST_TR_BASE = 0;

	if (VmxStatus::kOk != (status = static_cast<VmxStatus>(__vmx_vmptrld(&Vmcs01_pa))))
	{
		VmxInstructionError error = static_cast<VmxInstructionError>(UtilVmRead(VmcsField::kVmInstructionError));
		HYPERPLATFORM_LOG_DEBUG_SAFE("Error vmptrld error code :%x , %x", status, error);
	}
 
	VmRead64(VmcsField::kHostRip, Vmcs12_va, &VMCS12_HOST_RIP);
	VmRead64(VmcsField::kHostRsp, Vmcs12_va, &VMCS12_HOST_STACK);
	VmRead64(VmcsField::kHostCr0, Vmcs12_va, &VMCS12_HOST_CR0);
	VmRead64(VmcsField::kHostCr3, Vmcs12_va, &VMCS12_HOST_CR3);
	VmRead64(VmcsField::kHostCr4, Vmcs12_va, &VMCS12_HOST_CR4);
	 
	VmRead64(VmcsField::kHostCsSelector, Vmcs12_va, &VMCS12_HOST_CS);
	VmRead64(VmcsField::kHostSsSelector, Vmcs12_va, &VMCS12_HOST_SS);
	VmRead64(VmcsField::kHostDsSelector, Vmcs12_va, &VMCS12_HOST_DS);
	VmRead64(VmcsField::kHostEsSelector, Vmcs12_va, &VMCS12_HOST_ES);
	VmRead64(VmcsField::kHostFsSelector, Vmcs12_va, &VMCS12_HOST_FS);
	VmRead64(VmcsField::kHostGsSelector, Vmcs12_va, &VMCS12_HOST_GS);
	VmRead64(VmcsField::kHostTrSelector, Vmcs12_va, &VMCS12_HOST_TR);
	 
	VmRead32(VmcsField::kHostIa32SysenterCs, Vmcs12_va, &VMCS12_HOST_SYSENTER_CS);
	VmRead64(VmcsField::kHostIa32SysenterEip, Vmcs12_va, &VMCS12_HOST_SYSENTER_RSP);
	VmRead64(VmcsField::kHostIa32SysenterEsp, Vmcs12_va, &VMCS12_HOST_SYSENTER_RIP); 

	VmRead64(VmcsField::kHostFsBase, Vmcs12_va, &VMCS12_HOST_FS_BASE);
	VmRead64(VmcsField::kHostGsBase, Vmcs12_va, &VMCS12_HOST_GS_BASE);
	VmRead64(VmcsField::kHostTrBase, Vmcs12_va, &VMCS12_HOST_TR_BASE);

	//Disable Interrupt Flags
	FlagRegister rflags = { VMCS12_HOST_RFLAGs };
	rflags.fields.reserved1 = 1;
	UtilVmWrite(VmcsField::kGuestRflags, rflags.all);

	UtilVmWrite(VmcsField::kGuestRip, VMCS12_HOST_RIP);
	UtilVmWrite(VmcsField::kGuestRsp, VMCS12_HOST_STACK);
	UtilVmWrite(VmcsField::kGuestCr0, VMCS12_HOST_CR0);
	UtilVmWrite(VmcsField::kGuestCr3, VMCS12_HOST_CR3);
	UtilVmWrite(VmcsField::kGuestCr4, VMCS12_HOST_CR4);
	UtilVmWrite(VmcsField::kGuestDr7, 0x400);

	UtilVmWrite(VmcsField::kGuestCsSelector, VMCS12_HOST_CS);
	UtilVmWrite(VmcsField::kGuestSsSelector, VMCS12_HOST_SS);
	UtilVmWrite(VmcsField::kGuestDsSelector, VMCS12_HOST_DS);
	UtilVmWrite(VmcsField::kGuestEsSelector, VMCS12_HOST_ES);
	UtilVmWrite(VmcsField::kGuestFsSelector, VMCS12_HOST_FS);
	UtilVmWrite(VmcsField::kGuestGsSelector, VMCS12_HOST_GS);
	UtilVmWrite(VmcsField::kGuestTrSelector, VMCS12_HOST_TR);

	UtilVmWrite(VmcsField::kGuestSysenterCs,  VMCS12_HOST_SYSENTER_CS);
	UtilVmWrite(VmcsField::kGuestSysenterEsp, VMCS12_HOST_SYSENTER_RSP);
	UtilVmWrite(VmcsField::kGuestSysenterEip, VMCS12_HOST_SYSENTER_RIP);

	// Sync L1's Host segment base with L0 VMM Host Host segment base
	UtilVmWrite(VmcsField::kGuestCsBase, 0);
	UtilVmWrite(VmcsField::kGuestSsBase, 0);
	UtilVmWrite(VmcsField::kGuestDsBase, 0);
	UtilVmWrite(VmcsField::kGuestEsBase, 0);
	UtilVmWrite(VmcsField::kGuestFsBase, VMCS12_HOST_FS_BASE);
	UtilVmWrite(VmcsField::kGuestGsBase, VMCS12_HOST_GS_BASE);
	UtilVmWrite(VmcsField::kGuestTrBase, VMCS12_HOST_TR_BASE);
	  
	// Sync L1's Host Host segment Limit with L0 Host Host segment Limit
	UtilVmWrite(VmcsField::kGuestEsLimit, GetSegmentLimit(AsmReadES()));
	UtilVmWrite(VmcsField::kGuestCsLimit, GetSegmentLimit(AsmReadCS()));
	UtilVmWrite(VmcsField::kGuestSsLimit, GetSegmentLimit(AsmReadSS()));
	UtilVmWrite(VmcsField::kGuestDsLimit, GetSegmentLimit(AsmReadDS()));
	UtilVmWrite(VmcsField::kGuestFsLimit, GetSegmentLimit(AsmReadFS()));
	UtilVmWrite(VmcsField::kGuestGsLimit, GetSegmentLimit(AsmReadGS()));
	UtilVmWrite(VmcsField::kGuestLdtrLimit, GetSegmentLimit(AsmReadLDTR()));
	UtilVmWrite(VmcsField::kGuestTrLimit, GetSegmentLimit(AsmReadTR())); 

	// Sync L1's Host segment ArBytes with L0  Host segment ArBytes 
	UtilVmWrite(VmcsField::kGuestEsArBytes,	  VmpGetSegmentAccessRight(AsmReadES()));
	UtilVmWrite(VmcsField::kGuestCsArBytes,	  VmpGetSegmentAccessRight(AsmReadCS()));
	UtilVmWrite(VmcsField::kGuestSsArBytes,	  VmpGetSegmentAccessRight(AsmReadSS()));
	UtilVmWrite(VmcsField::kGuestDsArBytes,	  VmpGetSegmentAccessRight(AsmReadDS()));
	UtilVmWrite(VmcsField::kGuestFsArBytes,	  VmpGetSegmentAccessRight(AsmReadFS()));
	UtilVmWrite(VmcsField::kGuestGsArBytes,	  VmpGetSegmentAccessRight(AsmReadGS()));
	UtilVmWrite(VmcsField::kGuestLdtrArBytes, VmpGetSegmentAccessRight(AsmReadLDTR()));
	UtilVmWrite(VmcsField::kGuestTrArBytes,	  VmpGetSegmentAccessRight(AsmReadTR()) | 0xB); 

	UtilVmWrite(VmcsField::kGuestIa32Debugctl, 0);
	  
	//Clean VMCS1-2 Injecting event since it shouldn't be injected 
	VmWrite32(VmcsField::kVmEntryIntrInfoField, Vmcs12_va, 0);
	VmWrite32(VmcsField::kVmEntryExceptionErrorCode, Vmcs12_va, 0); 
	UtilVmWrite(VmcsField::kVmEntryIntrInfoField, 0);
	UtilVmWrite(VmcsField::kVmEntryExceptionErrorCode, 0);

	return STATUS_SUCCESS;
}


 
//-----------------------------------------------------------------------------------------------------------
/*
	We need to emulate the exception if and only if the vCPU mode is Guest Mode ,
	and only the exception is somethings we want to redirect to L1 for handle it.
	GetVmxMode:
	{
	Root Mode:
	- if the Guest's vCPU is root mode , that means he dun expected the action will be trap.
	so that action should not give its VMExit handler, otherwise.
	Guest Mode:
	- If the Guest's vCPU is in guest mode, that means he expected the action will be trapped
	And handle by its VMExit handler
	}

	We desginated the L1 wants to handle any breakpoint exception but the others.
	So that we only nested it for testing purpose.
*/
NTSTATUS VmxVMExitEmulate(VCPUVMX* vCPU , GuestContext* guest_context)
{ 
	if (!vCPU)
	{
		return STATUS_UNSUCCESSFUL;
	}

	// Since VMXON, but VMPTRLD 
	if (!vCPU->vmcs02.VmcsPa || !vCPU->vmcs12.VmcsPa || vCPU->vmcs12.VmcsPa == ~0x0 || vCPU->vmcs02.VmcsPa == ~0x0)
	{
		//HYPERPLATFORM_LOG_DEBUG_SAFE("cannot find vmcs \r\n");
		return STATUS_UNSUCCESSFUL; 
	}

	VMDbgCfg* cfg;
	IceGetVmmDbgConfig(&cfg);
	
 
	LEAVE_GUEST_MODE(vCPU); 
	SaveGuestKernelGsBase(GetProcessorData(guest_context)); 
	LoadHostKernelGsBase(GetProcessorData(guest_context));

	if (cfg->CallbackBitmap & DBG_VMEXIT_PRE_VMCS_SAVE_GUEST_STATE)
	{	 
		const VmExitInformation exit_reason = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		VMDbgInfo Info = { 0 };
		Info.CallbackType = DBG_VMEXIT_PRE_VMCS_SAVE_GUEST_STATE;
		Info.ExitReason = exit_reason.fields.reason;
		Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(vCPU->vmcs12.VmcsPa); 
		cfg->OnPreVmExitCallback[0](&Info);
	}
	
	VmxSaveGuestFieldFromVmcs02(vCPU);
	 

	if (cfg->CallbackBitmap & DBG_VMEXIT_POST_VMCS_SAVE_GUEST_STATE)
	{
		const VmExitInformation exit_reason = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		VMDbgInfo Info = { 0 };
		Info.CallbackType = DBG_VMEXIT_POST_VMCS_SAVE_GUEST_STATE;
		Info.ExitReason = exit_reason.fields.reason;
		Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(vCPU->vmcs12.VmcsPa);
		cfg->OnPostVmExitCallback[0](&Info);
	}

	 
	VmxSaveExceptionInformationFromVmcs02(vCPU);

	 

	VmxSaveGuestCr8(vCPU, GetGuestCr8(guest_context));

	if (cfg->CallbackBitmap & DBG_VMEXIT_PRE_VMCS_LOAD_HOST_STATE)
	{
		const VmExitInformation exit_reason = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		VMDbgInfo Info = { 0 };
		Info.CallbackType = DBG_VMEXIT_PRE_VMCS_LOAD_HOST_STATE;
		Info.ExitReason = exit_reason.fields.reason;
		Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(vCPU->vmcs12.VmcsPa);
		cfg->OnPreVmExitCallback[0](&Info);
	}

	VmxLoadHostStateForLevel1(vCPU);

	if (cfg->CallbackBitmap & DBG_VMEXIT_POST_VMCS_LOAD_HOST_STATE)
	{
		const VmExitInformation exit_reason = {
			static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
		VMDbgInfo Info = { 0 };
		Info.CallbackType = DBG_VMEXIT_POST_VMCS_LOAD_HOST_STATE;
		Info.ExitReason = exit_reason.fields.reason;
		Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(vCPU->vmcs12.VmcsPa);
		cfg->OnPostVmExitCallback[0](&Info);
	}


	return STATUS_SUCCESS;
} 

//---------------------------------------------------------------------------------------------------------------------//
void VmxSaveGuestCr8(VCPUVMX* vcpu, ULONG_PTR cr8)
{
	vcpu->guest_cr8 = cr8;
	
	//HYPERPLATFORM_LOG_DEBUG_SAFE("DEBUG###Save cr8 : %I64X \r\n ", vcpu->guest_cr8);
}
//---------------------------------------------------------------------------------------------------------------------//
void VmxRestoreGuestCr8(VCPUVMX* vcpu)
{
	__writecr8(vcpu->guest_cr8);
	
	//HYPERPLATFORM_LOG_DEBUG_SAFE("DEBUG###Restore cr8 : %I64X \r\n ", __readcr8());
}
 
//---------------------------------------------------------------------------------------------------------------------//
void VmxAllocateEpt02(GuestContext* guest_context, ULONG64 Eptp12)
{
	PVOID Eptr02 = NULL;

	if (!GetEptp12Data(guest_context))
	{
		SetEptp12Data(guest_context, (ULONG64)RawEptPointerToStruct(Eptp12));
		HYPERPLATFORM_LOG_DEBUG("Allocated Ept0-2: %I64x \r\n ", GetEptp12Data(guest_context));
	}

	if (!GetEptp02(guest_context))
	{
		Eptr02 = AllocEmptyEptp(GetEptp12Data(guest_context));
		HYPERPLATFORM_LOG_DEBUG("Allocated Ept0-2: %I64x \r\n ", Eptr02 );
	}

	if (Eptr02)
	{
		SetEptp02Data(guest_context, (ULONG64)Eptr02);
		HYPERPLATFORM_LOG_DEBUG("Set Ept0-2 : %I64x \r\n ", GetEptp02(guest_context));
	}
	
	HYPERPLATFORM_LOG_DEBUG("Ept0-2 : %I64x \r\n ", GetEptp02(guest_context));
	UtilVmWrite(VmcsField::kEptPointer, (ULONG64)GetEptp02(guest_context));
}

//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmxonEmulate(GuestContext* guest_context)
{
	do
	{
		VCPUVMX*			  nested_vmx		    = NULL;
		ULONG64				  InstructionPointer	= 0;	 
		ULONG64				  StackPointer			= 0;	 
		ULONG64				  vmxon_region_pa		= 0;	 
		ULONG64				  guest_address			= NULL;
		VmControlStructure*   vmxon_region_struct	= NULL;  
		PROCESSOR_NUMBER      number = { 0 };

		InstructionPointer  =  { UtilVmRead64(VmcsField::kGuestRip) };
		StackPointer		=  { UtilVmRead64(VmcsField::kGuestRsp) };   
		guest_address		= DecodeVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
		   
		 
		if (GetvCpuMode(guest_context) == VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode !"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		} 

		if (!guest_address)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXON: guest_address Parameter is NULL !"));
			//#UD
			ThrowInvalidCodeException();
			break;
		}

		vmxon_region_pa = *(PULONG64)guest_address;
		// VMXON_REGION IS NULL 
		if (!vmxon_region_pa)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXON: vmxon_region_pa Parameter is NULL !"));
			//#UD
			ThrowInvalidCodeException();
			break;
		}

		//if is it not page aglined
		if (!CheckPageAlgined(vmxon_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXON: not page aligned physical address %I64X !"), vmxon_region_pa);
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
		if (!CheckPhysicalAddress(vmxon_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXON: invalid physical address %I64X !"), vmxon_region_pa);
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		 
		// todo: check vcpu context ...'

		nested_vmx = (VCPUVMX*)ExAllocatePool(NonPagedPoolNx, sizeof(VCPUVMX));

		nested_vmx->inRoot = RootMode;
		nested_vmx->blockINITsignal = TRUE;
		nested_vmx->blockAndDisableA20M = TRUE;
		nested_vmx->vmcs02.VmcsPa = 0xFFFFFFFFFFFFFFFF;
		nested_vmx->vmcs12.VmcsPa = 0xFFFFFFFFFFFFFFFF;
		__vmx_vmptrst(&nested_vmx->vmcs01.VmcsPa);
		nested_vmx->vmxon_region = vmxon_region_pa;
		nested_vmx->InitialCpuNumber = KeGetCurrentProcessorNumberEx(&number);

		// vcpu etner vmx-root mode now
		EnterVmxMode(guest_context);  
		SetvCpuVmx(guest_context, nested_vmx); 
				  


		HYPERPLATFORM_LOG_DEBUG("VMXON: Guest Instruction Pointer %I64X Guest Stack Pointer: %I64X  Guest VMXON_Region: %I64X stored at %I64x physical address\r\n",
			InstructionPointer, StackPointer, vmxon_region_pa, guest_address);

		HYPERPLATFORM_LOG_DEBUG("VMXON: Run Successfully with VMXON_Region:  %I64X Total Vitrualized Core: %x  Current Cpu: %x in Cpu Group : %x  Number: %x \r\n",
			vmxon_region_pa, nested_vmx->InitialCpuNumber, number.Group, number.Number);
		  
		
		HYPERPLATFORM_COMMON_DBG_BREAK();

		BuildGernericVMCSMap();

		VMSucceed(GetFlagReg(guest_context));

	} while (FALSE);


}
//---------------------------------------------------------------------------------------------------------------//
VOID VmxVmxoffEmulate(
	_In_ GuestContext* guest_context
)
{
	do
	{
		VCPUVMX* vcpu_vmx = NULL; 
		ULONG_PTR InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
		ULONG_PTR StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) }; 

		HYPERPLATFORM_LOG_DEBUG("VMXOFF: InstructionPointer : %I64x \r\n", InstructionPointer);
		HYPERPLATFORM_LOG_DEBUG("VMXOFF: StackPointer : %I64x \r\n", StackPointer);
		HYPERPLATFORM_COMMON_DBG_BREAK();
		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		vcpu_vmx = GetVcpuVmx(guest_context);
		if (!vcpu_vmx)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Don't have Nested vCPU ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Vmxoff: Unimplemented third level virualization \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		ULONG GuestRip = UtilVmRead(VmcsField::kGuestRip);
		ULONG InstLen  = UtilVmRead(VmcsField::kVmExitInstructionLen);
		//load back vmcs01
		__vmx_vmptrld(&vcpu_vmx->vmcs01.VmcsPa);
	 
		UtilVmWrite(VmcsField::kGuestRip, GuestRip + InstLen);
	
		SetvCpuVmx(guest_context, NULL);

		LeaveVmxMode(guest_context);


		ExFreePool(vcpu_vmx);
		vcpu_vmx = NULL;  

		VMSucceed(GetFlagReg(guest_context));
	} while (0);
}
//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmclearEmulate(
	_In_ GuestContext* guest_context)
{
	do
	{ 
		//*(PULONG64)(StackPointer + offset);				
		//May need to be fixed later 
		VCPUVMX*				nested_vmx = NULL;
		ULONG64			InstructionPointer = 0;
		ULONG64				  StackPointer = 0;
		ULONG64				vmcs_region_pa = 0;
		ULONG64				 guest_address = NULL;  
		PROCESSOR_NUMBER		procnumber = { 0 };

		InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
		StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };

		HYPERPLATFORM_LOG_DEBUG("VMCLEAR: InstructionPointer: %I64x \r\n", InstructionPointer);
		HYPERPLATFORM_LOG_DEBUG("VMCLEAR: StackPointer: %I64x \r\n", StackPointer);
		HYPERPLATFORM_COMMON_DBG_BREAK();

		guest_address = DecodeVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);

		if (!guest_address)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR: guest_address NULL ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		 
		vmcs_region_pa = *(PULONG64)guest_address; 	 
		if (!vmcs_region_pa)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR: vmcs_region_pa NULL ! \r\n"));
			ThrowInvalidCodeException();
			break;
		} 
		 
		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR: Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR : Unimplemented third level virualization \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		nested_vmx = GetVcpuVmx(guest_context);
		if (!nested_vmx)
		{
			DumpVcpu(guest_context);
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}
 
		//if is it not page aglined
		if (!CheckPageAlgined(vmcs_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXCLEAR: not page aligned physical address %I64X ! \r\n"),
				vmcs_region_pa);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
		if (!CheckPhysicalAddress(vmcs_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXCLEAR: invalid physical address %I64X ! \r\n"),
				vmcs_region_pa);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		//if vmcs != vmregion 
		if (nested_vmx && (vmcs_region_pa == nested_vmx->vmxon_region))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMXCLEAR: VMCS region %I64X same as VMXON region %I64X ! \r\n"),
				vmcs_region_pa, nested_vmx->vmxon_region);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
	
		if (vmcs_region_pa == nested_vmx->vmcs12.VmcsPa)
		{
			nested_vmx->vmcs12.VmcsPa = 0xFFFFFFFFFFFFFFFF;
			nested_vmx->vmcs12.IsLaunch = FALSE;
		}

		__vmx_vmclear(&nested_vmx->vmcs02.VmcsPa);

		nested_vmx->vmcs02.VmcsPa = 0xFFFFFFFFFFFFFFFF;
		nested_vmx->vmcs02.IsLaunch = FALSE;

		HYPERPLATFORM_LOG_DEBUG_SAFE("VMCLEAR: Guest Instruction Pointer %I64X Guest Stack Pointer: %I64X  Guest vmcs region: %I64X stored at %I64x on stack\r\n",
			InstructionPointer, StackPointer, vmcs_region_pa, guest_address);

		HYPERPLATFORM_LOG_DEBUG_SAFE("VMCLEAR: Run Successfully Current Cpu: %x in Cpu Group : %x  Number: %x \r\n",
			nested_vmx->InitialCpuNumber, procnumber.Group, procnumber.Number);

		HYPERPLATFORM_LOG_DEBUG_SAFE("VMCLEAR: VCPU No.: %i Current VMCS : %I64X VMXON Region : %I64X  ",
			nested_vmx->InitialCpuNumber, nested_vmx->vmcs02.VmcsPa, nested_vmx->vmxon_region);

		VMSucceed(GetFlagReg(guest_context));

	} while (FALSE);
}

//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmptrldEmulate(GuestContext* guest_context)
{
	do
	{
		VCPUVMX*				nested_vmx = NULL;
		ULONG64			InstructionPointer = 0;
		ULONG64				  StackPointer = 0;
		PUCHAR			   vmcs02_region_va = NULL;
		ULONG64			   vmcs02_region_pa = NULL; 
		ULONG64				vmcs12_region_pa = 0;
		ULONG64				 guest_address = NULL;
		PROCESSOR_NUMBER		procnumber = { 0 };

		InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
		StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };

		HYPERPLATFORM_LOG_DEBUG("VMPTRLD: InstructionPointer: %I64x \r\n", InstructionPointer);
		HYPERPLATFORM_LOG_DEBUG("VMPTRLD: StackPointer: %I64x \r\n", StackPointer);
		HYPERPLATFORM_COMMON_DBG_BREAK();

		guest_address = DecodeVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
		if (!guest_address)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR: guest_address NULL ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		vmcs12_region_pa = *(PULONG64)guest_address;
		if (!vmcs12_region_pa)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMCLEAR: vmcs_region_pa NULL ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
 
		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG_SAFE("VMPTRLD Unimplemented third level virualization %I64x \r\n", GetVcpuVmx(guest_context));
			VMfailInvalid(GetFlagReg(guest_context)); 
			break;
		}
		
		//if is it not page aglined
		if (!CheckPageAlgined(vmcs12_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMPTRLD: not page aligned physical address %I64X ! \r\n"),
				vmcs12_region_pa);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		//if IA32_VMX_BASIC[48] == 1 it is not support 64bit addressing, so address[32] to address[63] supposed = 0
		if (!CheckPhysicalAddress(vmcs12_region_pa))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMPTRLD: invalid physical address %I64X ! \r\n"),
				vmcs12_region_pa);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		nested_vmx = GetVcpuVmx(guest_context);
		if (nested_vmx && (vmcs12_region_pa == nested_vmx->vmxon_region))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMPTRLD: VMCS region %I64X same as VMXON region %I64X ! \r\n"),
				vmcs12_region_pa, nested_vmx->vmxon_region);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
 
		vmcs02_region_va = (PUCHAR)ExAllocatePool(NonPagedPoolNx, PAGE_SIZE); 
		if (!vmcs02_region_va)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMPTRLD: vmcs02_region_va NULL ! \r\n"),
				vmcs12_region_pa, nested_vmx->vmxon_region);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		RtlZeroMemory(vmcs02_region_va, PAGE_SIZE); 

		vmcs02_region_pa = UtilPaFromVa(vmcs02_region_va); 
		nested_vmx->vmcs02.VmcsPa = vmcs02_region_pa;		    //vmcs02' physical address - DIRECT VMREAD/WRITE
		nested_vmx->vmcs12.VmcsPa = vmcs12_region_pa;		    //vmcs12' physical address - we will control its structure in Vmread/Vmwrite
		nested_vmx->kVirtualProcessorId = (USHORT)KeGetCurrentProcessorNumberEx(nullptr) + 1;

		HYPERPLATFORM_LOG_DEBUG_SAFE("[VMPTRLD] Run Successfully \r\n");
		HYPERPLATFORM_LOG_DEBUG_SAFE("[VMPTRLD] VMCS02 PA: %I64X VA: %I64X  \r\n", vmcs02_region_pa, vmcs02_region_va);
		HYPERPLATFORM_LOG_DEBUG_SAFE("[VMPTRLD] VMCS12 PA: %I64X \r\n", vmcs12_region_pa);
		HYPERPLATFORM_LOG_DEBUG_SAFE("[VMPTRLD] VMCS01 PA: %I64X VA: %I64X \r\n", nested_vmx->vmcs01.VmcsPa, UtilVaFromPa(nested_vmx->vmcs01.VmcsPa));
		HYPERPLATFORM_LOG_DEBUG_SAFE("[VMPTRLD] Current Cpu: %x in Cpu Group : %x  Number: %x \r\n", nested_vmx->InitialCpuNumber, procnumber.Group, procnumber.Number);
		  
		VMSucceed(GetFlagReg(guest_context));

	} while (FALSE);
}

//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmreadEmulate(GuestContext* guest_context)
{

	do
	{

		VmcsField		  field;
		ULONG_PTR		  offset;
		ULONG_PTR		  value;
		BOOLEAN			  RorM;
		ULONG_PTR		  regIndex;
		ULONG_PTR		  memAddress;
		PROCESSOR_NUMBER  procnumber = { 0 };
		VCPUVMX*		  NestedvCPU = GetVcpuVmx(guest_context);
		ULONG64			  vmcs12_pa = NestedvCPU->vmcs12.VmcsPa;
		ULONG64			  vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);
		
		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		if (!NestedvCPU)
		{
			DumpVcpu(guest_context);
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG(" Vmread: Unimplemented third level virualization VMX: %I64x  VMCS12: %I64x \r\n", GetVcpuVmx(guest_context), vmcs12_pa);
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		field = DecodeVmwriteOrVmRead(GetGpReg(guest_context), &offset, &value, &RorM, &regIndex, &memAddress);

		if (!is_vmcs_field_supported(field))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE("VMREAD: Virtual VT-x is not supported this feature [field: %I64X] \r\n", field); 	  //#gp
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		if ((ULONG64)vmcs12_va == 0xFFFFFFFFFFFFFFFF)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMREAD: 0xFFFFFFFFFFFFFFFF		 ! \r\n")); 	  //#gp
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		auto operand_size = VMCS_FIELD_WIDTH((int)field);

		if (RorM)
		{
			auto reg = VmmpSelectRegister((ULONG)regIndex, guest_context);
			if (operand_size == VMCS_FIELD_WIDTH_16BIT)
			{
				VmRead16(field, vmcs12_va, (PUSHORT)reg);
				HYPERPLATFORM_LOG_DEBUG("VMREAD16: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, *(PUSHORT)reg);

			}
			if (operand_size == VMCS_FIELD_WIDTH_32BIT)
			{
				VmRead32(field, vmcs12_va, (PULONG32)reg);
				HYPERPLATFORM_LOG_DEBUG("VMREAD32: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field, vmcs12_va, offset, *(PULONG32)reg);
			}
			if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
			{
				VmRead64(field, vmcs12_va, (PULONG64)reg);
				HYPERPLATFORM_LOG_DEBUG("VMREAD64: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, *(PULONG64)reg);
			}

		}
		else
		{
			if (operand_size == VMCS_FIELD_WIDTH_16BIT)
			{
				VmRead16(field, vmcs12_va, (PUSHORT)memAddress);
				HYPERPLATFORM_LOG_DEBUG("VMREAD16: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, *(PUSHORT)memAddress);
			}
			if (operand_size == VMCS_FIELD_WIDTH_32BIT)
			{
				VmRead32(field, vmcs12_va, (PULONG32)memAddress);
				HYPERPLATFORM_LOG_DEBUG("VMREAD32: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, *(PULONG32)memAddress);
			}
			if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
			{
				VmRead64(field, vmcs12_va, (PULONG64)memAddress);
				HYPERPLATFORM_LOG_DEBUG("VMREAD64: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, *(PULONG64)memAddress);
			}
		}

		VMSucceed(GetFlagReg(guest_context));

	} while (FALSE);
}

//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmwriteEmulate(GuestContext* guest_context)
{

	do
	{
		VmcsField			field;
		ULONG_PTR			offset;
		ULONG_PTR			Value;
		BOOLEAN				RorM;
		PROCESSOR_NUMBER    procnumber = { 0 };
		VCPUVMX*			NestedvCPU = GetVcpuVmx(guest_context);
		ULONG64				vmcs12_pa = (ULONG64)NestedvCPU->vmcs12.VmcsPa;
		ULONG64				vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);

		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE((" Vmwrite: Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG(" Vmwrite: Unimplemented third level virualization VMX: %I64x  VMCS12: %I64x \r\n", GetVcpuVmx(guest_context), vmcs12_pa);
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
		
		if (!NestedvCPU)
		{
			DumpVcpu(guest_context);
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}
		 
	
		///TODO: If in VMX non-root operation, should be VM Exit

		field = DecodeVmwriteOrVmRead(GetGpReg(guest_context), &offset, &Value, &RorM);

		if (!is_vmcs_field_supported(field))
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE("VMWRITE: IS NOT SUPPORT %X ! \r\n", field); 	  //#gp
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		/*if (!g_vcpus[vcpu_index]->inRoot)
		{
		///TODO: Should INJECT vmexit to L1
		///	   And Handle it well
		break;
		}*/
		auto operand_size = VMCS_FIELD_WIDTH((int)field);
		if (operand_size == VMCS_FIELD_WIDTH_16BIT)
		{
			VmWrite16(field, vmcs12_va, Value);
			HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X  \r\n",GetVmcsFieldNameByIndex(field), field,  vmcs12_va, offset, (USHORT)Value);
		}

		if (operand_size == VMCS_FIELD_WIDTH_32BIT)
		{
			VmWrite32(field, vmcs12_va, Value);
			HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n", GetVmcsFieldNameByIndex(field), field, vmcs12_va,  offset, (ULONG32)Value);
		}
		if (operand_size == VMCS_FIELD_WIDTH_64BIT || operand_size == VMCS_FIELD_WIDTH_NATURAL_WIDTH)
		{
			VmWrite64(field, vmcs12_va, Value);
			HYPERPLATFORM_LOG_DEBUG("VMWRITE: field: %s (%I64X) base: %I64X Offset: %I64X Value: %I64X\r\n",GetVmcsFieldNameByIndex(field), field,   vmcs12_va,   offset, (ULONG64)Value);
		}
		  

		VMSucceed(GetFlagReg(guest_context));
	} while (FALSE);
}

/*
64 bit Control field is not used

// below not used
// kVmExitMsrStoreAddr = 0x00002006,
// kVmExitMsrStoreAddrHigh = 0x00002007,
// kVmExitMsrLoadAddr = 0x00002008,
// kVmExitMsrLoadAddrHigh = 0x00002009,
// kVmEntryMsrLoadAddr = 0x0000200a,
// kVmEntryMsrLoadAddrHigh = 0x0000200b,
// kExecutiveVmcsPointer = 0x0000200c,
// kExecutiveVmcsPointerHigh = 0x0000200d,


// below not used
kTscOffset = 0x00002010,
kTscOffsetHigh = 0x00002011,
kVirtualApicPageAddr = 0x00002012,
kVirtualApicPageAddrHigh = 0x00002013,
kApicAccessAddrHigh = 0x00002015,
kPostedInterruptDescAddr  =  0x00002016,
kPostedInterruptDescAddrHigh = 0x00002017,

// below not used
kVmreadBitmapAddress = 0x00002026,
kVmreadBitmapAddressHigh = 0x00002027,
kVmwriteBitmapAddress = 0x00002028,
kVmwriteBitmapAddressHigh = 0x00002029,
kVirtualizationExceptionInfoAddress = 0x0000202a,
kVirtualizationExceptionInfoAddressHigh = 0x0000202b,
kXssExitingBitmap = 0x0000202c,
kXssExitingBitmapHigh = 0x0000202d,
*/

/*----------------------------------------------------------------------------------------------------

VMCS02 Structure
--------------------------------------
16/32/64/Natrual Guest state field :  VMCS12
16/32/64/Natrual Host  state field :  VMCS01
16/32/64/Natrual Control field	   :  VMCS01+VMCS12

----------------------------------------------------------------------------------------------------*/


//---------------------------------------------------------------------------------------------------------------------//
VOID VmxVmlaunchEmulate(GuestContext* guest_context)
{

	PROCESSOR_NUMBER  procnumber = { 0 };
	VCPUVMX*		  NestedvCPU = GetVcpuVmx(guest_context);
	VmxStatus		  status;
	do {
		HYPERPLATFORM_COMMON_DBG_BREAK();
		HYPERPLATFORM_LOG_DEBUG_SAFE("-----start vmlaunch---- \r\n");

		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		if (!NestedvCPU)
		{
			DumpVcpu(guest_context);
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG(" Vmlaunch: Unimplemented third level virualization VMX: %I64x  VMCS12: %I64x \r\n", 
				GetVcpuVmx(guest_context), NestedvCPU->vmcs12.VmcsPa);

			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		ENTER_GUEST_MODE(NestedvCPU);

		/*
		if (!g_vcpus[vcpu_index]->inRoot)
		{
		///TODO: Should INJECT vmexit to L1
		///	   And Handle it well
		break;
		}
		*/
		//Get vmcs02 / vmcs12
		PVOID	Eptr02 = NULL;
		ULONG64 Eptp12 = 0;
		ULONG32 SecondaryCtrl = 0;
		VmxSecondaryProcessorBasedControls SecondCtrl;


		auto    vmcs02_pa = NestedvCPU->vmcs02.VmcsPa;
		auto	vmcs12_pa = NestedvCPU->vmcs12.VmcsPa;

		if (!vmcs02_pa || !vmcs12_pa)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMLAUNCH: VMCS still not loaded ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		auto    vmcs02_va = (ULONG64)UtilVaFromPa(vmcs02_pa);
		auto    vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);
		 

		const Ia32VmxBasicMsr vmx_basic_msr = { UtilReadMsr64(Msr::kIa32VmxBasic) };
		RtlFillMemory((PVOID)vmcs02_va, 0, PAGE_SIZE);
		VmControlStructure* ptr = (VmControlStructure*)vmcs02_va;
		ptr->revision_identifier = vmx_basic_msr.fields.revision_identifier;

		///1. Check Setting of VMX Controls and Host State area;
		///2. Attempt to load guest state and PDPTRs as appropriate
		///3. Attempt to load MSRs from VM-Entry MSR load area;
		///4. Set VMCS to "launched"
		///5. VM Entry success

		//Guest passed it to us, and read/write it  VMCS 1-2
		// Write a VMCS revision identifier
 		VmRead32(VmcsField::kSecondaryVmExecControl, vmcs12_va, &SecondaryCtrl);
	
		SecondCtrl = { SecondaryCtrl };

		/*
		1. Mix vmcs control field
		*/
		PrepareHostAndControlField(vmcs12_va, vmcs02_pa, TRUE);
		 
		if (SecondCtrl.fields.enable_ept)
		{ 
			VmRead64(VmcsField::kEptPointer, vmcs12_va, &Eptp12); 	 
			EptPointer pt = { Eptp12 };
			HYPERPLATFORM_LOG_DEBUG("L1 VMM EptPointer PA: %I64x \r\n", pt.fields.pml4_address);
			VmxAllocateEpt02(guest_context, Eptp12);
		}

		/*
		2. Read VMCS12 Guest's field to VMCS02
		*/
		PrepareGuestStateField(vmcs12_va);
		  
		SaveHostKernelGsBase(GetProcessorData(guest_context));

		NestedvCPU->vmcs02.IsLaunch = TRUE;
		
		if (GetGuestIrql(guest_context) < DISPATCH_LEVEL)
		{
			KeLowerIrql(GetGuestIrql(guest_context));
		}

		if (VmxStatus::kOk != (status = static_cast<VmxStatus>(__vmx_vmlaunch())))
		{
			VmxInstructionError error2 = static_cast<VmxInstructionError>(UtilVmRead(VmcsField::kVmInstructionError));
			HYPERPLATFORM_LOG_DEBUG("Error VMLAUNCH error code :%x , %x ", status, error2);
			HYPERPLATFORM_COMMON_DBG_BREAK();
		}
		return;

	} while (FALSE);

}
//----------------------------------------------------------------------------------------------------------------//
VOID VmxVmresumeEmulate(GuestContext* guest_context)
{
	do
	{
		ULONG32 SecondaryCtrl = 0;
		VmxSecondaryProcessorBasedControls SecondCtrl;
		ULONG64 Eptp12 = 0;
		VmxStatus			  status = VmxStatus::kErrorWithoutStatus ;
		PROCESSOR_NUMBER  procnumber = { 0 };
		VCPUVMX*		  NestedvCPU	 = GetVcpuVmx(guest_context);
		VMDbgCfg* cfg =	NULL;
		IceGetVmmDbgConfig(&cfg);
		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		if (!NestedvCPU)
		{
			DumpVcpu(guest_context);
			HYPERPLATFORM_COMMON_DBG_BREAK();
			break;
		}

		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG(" Vmresume: Unimplemented third level virualization VMX: %I64x  VMCS12: %I64x \r\n", GetVcpuVmx(guest_context), NestedvCPU->vmcs12.VmcsPa);
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		ENTER_GUEST_MODE(NestedvCPU);

		auto      vmcs02_pa = NestedvCPU->vmcs02.VmcsPa;
		auto	  vmcs12_pa = NestedvCPU->vmcs12.VmcsPa;

		if (!vmcs02_pa || !vmcs12_pa)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("VMRESUME: VMCS still not loaded ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}
	
		auto    vmcs02_va = (ULONG64)UtilVaFromPa(vmcs02_pa);
		auto    vmcs12_va = (ULONG64)UtilVaFromPa(vmcs12_pa);

		// Write a VMCS revision identifier
		const Ia32VmxBasicMsr vmx_basic_msr = { UtilReadMsr64(Msr::kIa32VmxBasic) };
		VmControlStructure* ptr = (VmControlStructure*)vmcs02_va;

		VmRead32(VmcsField::kSecondaryVmExecControl, vmcs12_va, &SecondaryCtrl);
		SecondCtrl = { SecondaryCtrl };


		ptr->revision_identifier = vmx_basic_msr.fields.revision_identifier;

		//Restore some MSR & cr8 we may need to ensure the consistency  
		  
		VmxRestoreGuestCr8(NestedvCPU);

		if (cfg && (cfg->CallbackBitmap & DBG_VMENTRY_PRE_VMCS_SAVE_HOST_STATE))
		{
			const VmExitInformation exit_reason = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
			VMDbgInfo Info = { 0 };
			Info.CallbackType = DBG_VMENTRY_PRE_VMCS_SAVE_HOST_STATE;
			Info.ExitReason = exit_reason.fields.reason;
			Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(GetVcpuVmx(guest_context)->vmcs12.VmcsPa);
			cfg->OnPreVmEntryCallback[0](&Info);
		}

		//Prepare VMCS01 Host / Control Field
		PrepareHostAndControlField(vmcs12_va, vmcs02_pa, FALSE);

		if (SecondCtrl.fields.enable_ept)
		{ 
			UtilVmWrite(VmcsField::kEptPointer, GetEptp02(guest_context)); 
		}

		if (cfg && (cfg->CallbackBitmap & DBG_VMENTRY_POST_VMCS_SAVE_HOST_STATE))
		{
			const VmExitInformation exit_reason = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
			VMDbgInfo Info = { 0 };
			Info.CallbackType = DBG_VMENTRY_POST_VMCS_SAVE_HOST_STATE;
			Info.ExitReason = exit_reason.fields.reason;
			Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(GetVcpuVmx(guest_context)->vmcs12.VmcsPa);
			cfg->OnPostVmEntryCallback[0](&Info);
		}
		
		
		if (cfg->CallbackBitmap & DBG_VMENTRY_PRE_VMCS_LOAD_GUEST_STATE)
		{
			const VmExitInformation exit_reason = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
			VMDbgInfo Info = { 0 };
			Info.CallbackType = DBG_VMENTRY_PRE_VMCS_LOAD_GUEST_STATE;
			Info.ExitReason = exit_reason.fields.reason;
			Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(GetVcpuVmx(guest_context)->vmcs12.VmcsPa);
			cfg->OnPreVmEntryCallback[0](&Info);
		}
		/*
		VM Guest state field Start
		*/
		PrepareGuestStateField(vmcs12_va); 
		/*
		VM Guest state field End
		*/ 

		if (cfg->CallbackBitmap & DBG_VMENTRY_POST_VMCS_LOAD_GUEST_STATE)
		{
			const VmExitInformation exit_reason = {
				static_cast<ULONG32>(UtilVmRead(VmcsField::kVmExitReason)) };
			VMDbgInfo Info = { 0 };
			Info.CallbackType = DBG_VMENTRY_POST_VMCS_LOAD_GUEST_STATE;
			Info.ExitReason = exit_reason.fields.reason;
			Info.GuestVmcs = (ULONG_PTR)UtilVaFromPa(GetVcpuVmx(guest_context)->vmcs12.VmcsPa);
			cfg->OnPostVmEntryCallback[0](&Info);
		}
	
		PrintVMCS();   
		
		SaveHostKernelGsBase(GetProcessorData(guest_context)); 
		LoadGuestKernelGsBase(GetProcessorData(guest_context));
		 
		//--------------------------------------------------------------------------------------//

		/*
		*		After L1 handles any VM Exit and should be executes VMRESUME for back L2
		*		But this time trapped by VMCS01 and We can't get any VM-Exit information
		*       from it. So we need to read from VMCS12 and return from here immediately.
		*		We saved the vmcs02 GuestRip into VMCS12 our VMExit Handler because when
		*		L1 was executing VMRESUME(We injected VMExit to it), and it is running on
		*		VMCS01, we can't and shouldn't change it.
		*		See: VmmVmExitHandler
		*/

		//--------------------------------------------------------------------------------------//

	
		HYPERPLATFORM_COMMON_DBG_BREAK();

	} while (FALSE);
}

//----------------------------------------------------------------------------------------------------------------//
VOID VmxVmptrstEmulate(GuestContext* guest_context)
{
	do
	{
		PROCESSOR_NUMBER	procnumber = {};
		ULONG64				InstructionPointer = { UtilVmRead64(VmcsField::kGuestRip) };
		ULONG64				StackPointer = { UtilVmRead64(VmcsField::kGuestRsp) };
		ULONG64				vmcs12_region_pa = *(PULONG64)DecodeVmclearOrVmptrldOrVmptrstOrVmxon(guest_context);
		ULONG64				vmcs12_region_va = (ULONG64)UtilVaFromPa(vmcs12_region_pa);
		ULONG				vcpu_index = KeGetCurrentProcessorNumberEx(&procnumber);


		HYPERPLATFORM_LOG_DEBUG("VMPTRST: InstructionPointer: %I64x \r\n", InstructionPointer);
		HYPERPLATFORM_LOG_DEBUG("VMPTRST: StackPointer: %I64x \r\n", StackPointer);
		HYPERPLATFORM_COMMON_DBG_BREAK();

		if (GetvCpuMode(guest_context) != VmxMode)
		{
			HYPERPLATFORM_LOG_DEBUG_SAFE(("Current vCPU already in VMX mode ! \r\n"));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		// if VCPU not run in VMX mode 
		if (GetVmxMode(GetVcpuVmx(guest_context)) != RootMode)
		{
			// Inject ...'
			HYPERPLATFORM_LOG_DEBUG_SAFE("Vmptrst: Unimplemented third level virualization  %I64x \r\n", GetVcpuVmx(guest_context));
			VMfailInvalid(GetFlagReg(guest_context));
			break;
		}

		*(PULONG64)vmcs12_region_va = GetVcpuVmx(guest_context)->vmcs12.VmcsPa;

		VMSucceed(GetFlagReg(guest_context));
	} while (FALSE);
}
}