/**
 * Dqlite Raft FSM
 */

#ifndef DQLITE_FSM_H_
#define DQLITE_FSM_H_

#include <raft.h>

#include "registry.h"
#include "config.h"

/**
 * Initialize the given SQLite replication interface with dqlite's raft based
 * implementation.
 */
int fsmInit(struct raft_fsm *fsm,
	    struct config *config,
	    struct registry *registry);

void fsmClose(struct raft_fsm *fsm);

#endif /* DQLITE_REPLICATION_METHODS_H_ */
