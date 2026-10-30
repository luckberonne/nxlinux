// nn::account functions of an AArch32 title that the shared account hooks (hle_account.c) do not take over. The host has no Nintendo Account: a function that would read or refresh one
// answers with a failure, which a title without a linked account handles on its own; running the SDK's version instead dereferences service objects the host never made.
#include "hle.h"

#define WHY "no Nintendo Account on the host; the SDK's version uses account service objects that do not exist"
HLE_FORCE("_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvj", WHY, Result, h32_acc_LoadCachedNA,
          void *info, const void *user, void *work, uint32_t size) { (void)info; (void)user; (void)work; (void)size; return RES_FAIL; }
