// nn::oe functions this SDK build (an older one, nn::oe without the PerformanceMode overloads) has that the shared oe hooks (hle_oe.c) do not name. The host has no applet service for
// them to talk to; each does what an application running in the foreground, handheld, with nothing pending would see.
#include "hle.h"
#include <string.h>

#define WHY "applet service call; the host has no applet service, the answer is the idle application's"
HLE_FORCE("_ZN2nn2oe16InitializeAppletEv", WHY, void, h32_oe_InitApplet, void) {}
HLE_FORCE("_ZN2nn2oe30InvalidateNnOeInitializeAppletEv", WHY, void, h32_oe_InvInit, void) {}
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", WHY, void, h32_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", WHY, void, h32_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe20NotifyRunningForTestEv", WHY, void, h32_oe_NotifyRunning, void) {}
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", WHY "; the version of the title", void, h32_oe_DisplayVersion, char *out) { memset(out, 0, 16); strcpy(out, "1.0.0"); }
HLE_FORCE("_ZN2nn2oe20GetDefaultThemeColorEv", WHY, int, h32_oe_DefTheme, void) { return 0; }
HLE_FORCE("_ZN2nn2oe21GetExpectedThemeColorEv", WHY, int, h32_oe_ExpTheme, void) { return 0; }
HLE_FORCE("_ZN2nn2oe21SetExpectedThemeColorENS0_14ThemeColorTypeE", WHY, void, h32_oe_SetTheme, int c) { (void)c; }
HLE_FORCE("_ZN2nn2oe22GetNotificationMessageEv", WHY "; no message is ever pending, and a blocking wait would never return", int, h32_oe_GetMsg, void) { return 0; }
HLE_FORCE("_ZN2nn2oe23BeginBlockingHomeButtonENS_8TimeSpanE", WHY, void, h32_oe_BlockHome, int64_t span) { (void)span; }
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", WHY, void, h32_oe_GetVol, float *a, float *b) { *a = 1.0f; *b = 1.0f; }
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", WHY, void, h32_oe_SetVol, float a, float b) { (void)a; (void)b; }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationEi", WHY, void, h32_oe_SetPerf, int cfg) { (void)cfg; }
HLE_FORCE("_ZN2nn2oe28SetFocusHandlingModeForDebugEbbb", WHY, void, h32_oe_SetFocusDbg, int a, int b, int c) { (void)a; (void)b; (void)c; }
