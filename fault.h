/* fault.h */

/*
 * This file is part of emuxfs, The Enhanced Multiplexed File System (see
 * NOTICE.md).
 *
 * Optional, compile-time-gated fault injection for crash-consistency tests.
 * It is inert unless EMUXFS_FAULT_INJECTION is defined, and is never enabled
 * by the normal build.  Call sites are unconditional; the disabled form is an
 * empty function so no #ifdef clutter is needed at each boundary.
 */

#ifndef _FAULT_H_
#define _FAULT_H_

#define EMUXFS_FAULT_ENV_POINT "EMUXFS_FAULT_POINT"
#define EMUXFS_FAULT_ENV_ACTION "EMUXFS_FAULT_ACTION"
constexpr int EMUXFS_FAULT_EXIT_STATUS = 70;

/*
 * If the environment names 'name' as the active fault point, terminate the
 * process at that point (simulating a crash) or abort.  Otherwise return.
 */
void emuxfs_fault_point(const char *);

#endif /* _FAULT_H_ */
