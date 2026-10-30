// nn::mii of an AArch32 title. The SDK runs over the mii:u service (ipc_services.c: an empty database that can build a default character) and the empty MiiResource archive. The only function
// taken over is the validity check: the host has no Mii resources to compare a character with, and a title that finds one invalid leaves a half-built model.
#include "hle.h"

#define WHY "no Mii resources to check against; the title cannot cope with an invalid Mii"
HLE_FORCE("_ZNK2nn3mii21ParamCharInfoAccessor7IsValidEv", WHY, int, h32_mii_ParamIsValid, const void *self) { (void)self; return 1; }
HLE_FORCE("_ZNK2nn3mii16CharInfoAccessor7IsValidEv", WHY, int, h32_mii_IsValid, const void *self) { (void)self; return 1; }
