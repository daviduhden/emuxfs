/* sync.c */

/*
 * Implementation of sync.  The first directory is the destination and the
 * remaining directories are sources; the destination may be empty (it is then
 * formatted) or an existing device rebuilt from a designated source.  Sync is
 * the explicit recovery path and therefore opens the destination with force,
 * clears interrupted-operation markers, and removes leftover rename
 * temporaries.  It does not clear the degraded flag.
 */

// clang-format off
#include <stdio.h>
#include <stdlib.h>

#include "emuxfs.h"
#include "sandbox.h"
// clang-format on

static void
emuxfs_sync_usage(void)
{
	EMUXFS_TRACE("enter");
	dprintf(2, "usage: emuxfs sync destination source ...\n");
}

/*
 * Recover a destination device from the first usable source.  The destination
 * is formatted only when it is empty; otherwise it is opened with force so a
 * dirty state.db can be repaired.  Every source must be usable before the
 * rebuild starts, otherwise a later source could be skipped and the result
 * would silently be incomplete.  A restore failure is propagated instead of
 * writing a clean state record over a possibly inconsistent device.
 */
EMUXFS int
emuxfs_sync_main(int argc, char *argv[])
{
	EMUXFS_TRACE("enter");
	bool			 empty, exists;
	struct emuxfs_dev	*ddev, *sdev;
	const char		*ddev_path;
	enum emuxfs_chk_alg_type alg;
	size_t			 chksz, metasz;
	time_t			 seq_zero_time;
	const uint8_t		*array_uuid;

	static const dind ddev_index = 0, sdev_index = 1;

	if (emuxfs_parse_args(argc, argv, 1)) {
		emuxfs_sync_usage();
		exit(1);
	}

	if (emuxfs_sandbox_unveil_mirrors(&emuxfs_cmdline))
		exit(1);
	if (emuxfs_sandbox_unveil_lock())
		exit(1);
	if (emuxfs_sandbox_pledge(EMUXFS_PLEDGE_SYNC))
		exit(1);

	if (emuxfs_init(1, 1))
		exit(-1);

	if (emuxfs_dev_count() < 2) {
		dprintf(2,
		    "Error: There are less than 2 directories in the "
		    "array.\n");
		exit(1);
	}
	if (emuxfs_dev_get(&ddev, ddev_index, 1))
		exit(-1);
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		exit(-1);

	/*
	 * Every source must be usable.  A degraded later source would
	 * otherwise be skipped silently by the restore source scan, producing
	 * a destination that is complete only for the paths the remaining
	 * sources happened to have.
	 */
	{
		dind		   si;
		struct emuxfs_dev *tmp;

		for (si = 1; si < emuxfs_dev_count(); ++si) {
			if (emuxfs_dev_get(&tmp, si, 0)) {
				dprintf(2,
				    "Error: source device %lu is not "
				    "usable.\n",
				    (unsigned long)si);
				emuxfs_final();
				exit(1);
			}
		}
	}

	ddev_path = ddev->root_path;
	alg = sdev->conf.chk_alg_type;
	chksz = emuxfs_chk_size(alg);
	if (emuxfs_meta_size_raw(&metasz, alg))
		exit(-1);
	seq_zero_time = sdev->conf.seq_zero_time;
	array_uuid = sdev->conf.array_uuid;

	if (emuxfs_dir_is_empty(&empty, ddev_path))
		exit(-1);
	if (empty) {
		if (emuxfs_dev_format(ddev_path, alg, chksz, metasz,
			seq_zero_time, array_uuid))
			exit(-1);
	}

	if (emuxfs_dev_mount(ddev_index, 1))
		exit(-1);

	if (emuxfs_state_restore_only_set(ddev_index))
		exit(-1);
	if (emuxfs_state_restore_push_back(ddev_index, "."))
		exit(-1);
	/*
	 * A zero exit status must mean that the destination was actually
	 * rebuilt.  Propagate a restore failure instead of writing a clean
	 * state record over a device that may still be inconsistent.
	 */
	if (emuxfs_restore_now()) {
		emuxfs_final();
		exit(1);
	}

	if (emuxfs_dev_get(&ddev, ddev_index, 0))
		exit(-1);
	if (emuxfs_dev_get(&sdev, sdev_index, 0))
		exit(-1);

	/*
	 * 'sync' is the explicit recovery path (see RECOVERY.md): the
	 * destination has just been rebuilt from the source, so the
	 * interrupted-operation markers must be cleared before committing.
	 * Without this a device left with 'working' set by a crash would be
	 * refused by every later mount and could never be recovered.
	 * 'degraded' is left untouched:
	 * it records a hardware, integrity or sequence-exhaustion fault and is
	 * not a transient operation marker.
	 */
	ddev->state.working = 0;
	ddev->state.restoring = 0;
	ddev->state.seq = sdev->state.seq;
	if (emuxfs_dev_state_write_fd(ddev->state_fd, &ddev->state))
		exit(-1);

	/*
	 * A rename uses two private temporaries: one for the source and, when
	 * it replaces an existing name, one for the old destination.  Either
	 * can survive a crash; remove both while rebuilding.
	 */
	if (emuxfs_existsat(&exists, ddev->muxfs_fd, "rename.tmp"))
		exit(-1);
	if (exists) {
		if (emuxfs_removeat(ddev->muxfs_fd, "rename.tmp"))
			exit(-1);
	}
	if (emuxfs_existsat(&exists, ddev->muxfs_fd, "rename.dest.tmp"))
		exit(-1);
	if (exists) {
		if (emuxfs_removeat(ddev->muxfs_fd, "rename.dest.tmp"))
			exit(-1);
	}

	if (emuxfs_final())
		exit(-1);

	return 0;
}
