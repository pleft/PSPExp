	.set noreorder

#include "pspimport.s"

/* the custom firmware's own kernel library (PRO, ARK), imported weakly (0x0009): the module loads
 * without it (PPSSPP), and pspexpUsbMute calls it only once linked (see there). The NID PRO exports
 * (procfw SystemControl/exports.exp), as psp/capture/systemctrl.S */
	IMPORT_START "SystemCtrlForKernel",0x00090000
	IMPORT_FUNC  "SystemCtrlForKernel",0x159AF5CC,sctrlHENFindFunction
