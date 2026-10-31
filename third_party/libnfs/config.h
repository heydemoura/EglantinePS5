/* PS5SX2 (vk-285-135, AI-assisted): libnfs's configuration for the PS5 port's NFS shares
 * (ps5/coreorbis/orbis-shims/OrbisNfs.cpp). config_ps5.h is what libnfs's own CMake checks found with the PS5 payload
 * SDK's toolchain (prospero-cmake; multithreading, tests, utils and examples off), changed by hand where its header says;
 * config_linux.h is the same build on Linux (Ubuntu 24.04), for the PC test (ps5/coreorbis/tests/nfs). */
#if defined(__PROSPERO__)
#include "config_ps5.h"
#else
#include "config_linux.h"
#endif
