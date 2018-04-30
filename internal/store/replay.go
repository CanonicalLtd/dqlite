package store

import (
	"github.com/CanonicalLtd/dqlite/internal/registry"
	"github.com/CanonicalLtd/dqlite/internal/replication"
	"github.com/hashicorp/raft"
	"github.com/pkg/errors"
)

// Replay the commands in the given logs and snapshot stores using the given
// dir as database directory.
func Replay(logs raft.LogStore, snaps raft.SnapshotStore, dir string) error {
	// Create a registry and a FSM.
	registry := registry.New(dir)
	fsm := replication.NewFSM(registry)

	// We'll apply all logs unless there is a snapshot, see below.
	r, err := DefaultRange(logs)
	if err != nil {
		return errors.Wrap(err, "failed to get index range")
	}

	// Figure out if we have a snapshot to restore.
	metas, err := snaps.List()
	if err != nil {
		return errors.Wrap(err, "failed to get snapshots list")
	}

	if len(metas) > 0 {
		meta := metas[0] // The most recent.
		_, reader, err := snaps.Open(meta.ID)
		if err != nil {
			return errors.Wrapf(err, "failed to open snapshot %s", meta.ID)
		}
		if err := fsm.Restore(reader); err != nil {
			return errors.Wrapf(err, "failed to restore snapshot %s", meta.ID)
		}

		// Update the range
		r.First = meta.Index + 1
	}

	// Replay the logs.
	err = Iterate(logs, r, func(index uint64, log *raft.Log) error {
		fsm.Apply(log)
		return nil
	})
	if err != nil {
		errors.Wrap(err, "failed to iterate through the logs")
	}
	return nil
}
