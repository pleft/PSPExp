	.set noreorder

#include "pspimport.s"

/* the custom firmware's own kernel library (PRO, ARK): two of its functions, by the NIDs PRO exports
 * (procfw SystemControl/exports.exp) */
	IMPORT_START "SystemCtrlForKernel",0x00090000
	IMPORT_FUNC  "SystemCtrlForKernel",0x159AF5CC,sctrlHENFindFunction
	IMPORT_FUNC  "SystemCtrlForKernel",0x826668E9,sctrlHENPatchSyscall
