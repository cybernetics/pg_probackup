/*-------------------------------------------------------------------------
 *
 * catalog.c: backup catalog operation
 *
 * Portions Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2019, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"
#include "access/timeline.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/file.h"
#include "utils/configuration.h"

static pgBackup* get_closest_backup(timelineInfo *tlinfo);
static pgBackup* get_oldest_backup(timelineInfo *tlinfo);
static const char *backupModes[] = {"", "PAGE", "PTRACK", "DELTA", "FULL"};
static pgBackup *readBackupControlFile(const char *path);

static bool exit_hook_registered = false;
static parray *lock_files = NULL;

static timelineInfo *
timelineInfoNew(TimeLineID tli)
{
	timelineInfo *tlinfo = (timelineInfo *) pgut_malloc(sizeof(timelineInfo));
	MemSet(tlinfo, 0, sizeof(timelineInfo));
	tlinfo->tli = tli;
	tlinfo->switchpoint = InvalidXLogRecPtr;
	tlinfo->parent_link = NULL;
	tlinfo->xlog_filelist = parray_new();
	tlinfo->anchor_lsn = InvalidXLogRecPtr;
	tlinfo->anchor_tli = 0;
	return tlinfo;
}

/* Iterate over locked backups and delete locks files */
static void
unlink_lock_atexit(void)
{
	int			i;

	if (lock_files == NULL)
		return;

	for (i = 0; i < parray_num(lock_files); i++)
	{
		char	   *lock_file = (char *) parray_get(lock_files, i);
		int			res;

		res = fio_unlink(lock_file, FIO_BACKUP_HOST);
		if (res != 0 && errno != ENOENT)
			elog(WARNING, "%s: %s", lock_file, strerror(errno));
	}

	parray_walk(lock_files, pfree);
	parray_free(lock_files);
	lock_files = NULL;
}

/*
 * Read backup meta information from BACKUP_CONTROL_FILE.
 * If no backup matches, return NULL.
 */
pgBackup *
read_backup(const char *instance_name, time_t timestamp)
{
	pgBackup	tmp;
	char		conf_path[MAXPGPATH];

	tmp.start_time = timestamp;
	pgBackupGetPathInInstance(instance_name, &tmp, conf_path,
					 lengthof(conf_path), BACKUP_CONTROL_FILE, NULL);

	return readBackupControlFile(conf_path);
}

/*
 * Save the backup status into BACKUP_CONTROL_FILE.
 *
 * We need to reread the backup using its ID and save it changing only its
 * status.
 */
void
write_backup_status(pgBackup *backup, BackupStatus status,
					const char *instance_name)
{
	pgBackup   *tmp;

	tmp = read_backup(instance_name, backup->start_time);
	if (!tmp)
	{
		/*
		 * Silently exit the function, since read_backup already logged the
		 * warning message.
		 */
		return;
	}

	backup->status = status;
	tmp->status = backup->status;
	write_backup(tmp);

	pgBackupFree(tmp);
}

/*
 * Create exclusive lockfile in the backup's directory.
 */
bool
lock_backup(pgBackup *backup)
{
	char		lock_file[MAXPGPATH];
	int			fd;
	char		buffer[MAXPGPATH * 2 + 256];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		my_pid,
				my_p_pid;

	pgBackupGetPath(backup, lock_file, lengthof(lock_file), BACKUP_CATALOG_PID);

	/*
	 * If the PID in the lockfile is our own PID or our parent's or
	 * grandparent's PID, then the file must be stale (probably left over from
	 * a previous system boot cycle).  We need to check this because of the
	 * likelihood that a reboot will assign exactly the same PID as we had in
	 * the previous reboot, or one that's only one or two counts larger and
	 * hence the lockfile's PID now refers to an ancestor shell process.  We
	 * allow pg_ctl to pass down its parent shell PID (our grandparent PID)
	 * via the environment variable PG_GRANDPARENT_PID; this is so that
	 * launching the postmaster via pg_ctl can be just as reliable as
	 * launching it directly.  There is no provision for detecting
	 * further-removed ancestor processes, but if the init script is written
	 * carefully then all but the immediate parent shell will be root-owned
	 * processes and so the kill test will fail with EPERM.  Note that we
	 * cannot get a false negative this way, because an existing postmaster
	 * would surely never launch a competing postmaster or pg_ctl process
	 * directly.
	 */
	my_pid = getpid();
#ifndef WIN32
	my_p_pid = getppid();
#else

	/*
	 * Windows hasn't got getppid(), but doesn't need it since it's not using
	 * real kill() either...
	 */
	my_p_pid = 0;
#endif

	/*
	 * We need a loop here because of race conditions.  But don't loop forever
	 * (for example, a non-writable $backup_instance_path directory might cause a failure
	 * that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600.  See
		 * comments below.
		 */
		fd = fio_open(lock_file, O_RDWR | O_CREAT | O_EXCL, FIO_BACKUP_HOST);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			elog(ERROR, "Could not create lock file \"%s\": %s",
				 lock_file, strerror(errno));

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */
		fd = fio_open(lock_file, O_RDONLY, FIO_BACKUP_HOST);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			elog(ERROR, "Could not open lock file \"%s\": %s",
				 lock_file, strerror(errno));
		}
		if ((len = fio_read(fd, buffer, sizeof(buffer) - 1)) < 0)
			elog(ERROR, "Could not read lock file \"%s\": %s",
				 lock_file, strerror(errno));
		fio_close(fd);

		if (len == 0)
			elog(ERROR, "Lock file \"%s\" is empty", lock_file);

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		if (encoded_pid <= 0)
			elog(ERROR, "Bogus data in lock file \"%s\": \"%s\"",
				 lock_file, buffer);

		/*
		 * Check to see if the other process still exists
		 *
		 * Per discussion above, my_pid, my_p_pid can be
		 * ignored as false matches.
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist.
		 */
		if (encoded_pid != my_pid && encoded_pid != my_p_pid)
		{
			if (kill(encoded_pid, 0) == 0)
			{
				elog(WARNING, "Process %d is using backup %s and still is running",
					 encoded_pid, base36enc(backup->start_time));
				return false;
			}
			else
			{
				if (errno == ESRCH)
					elog(WARNING, "Process %d which used backup %s no longer exists",
						 encoded_pid, base36enc(backup->start_time));
				else
					elog(ERROR, "Failed to send signal 0 to a process %d: %s",
						encoded_pid, strerror(errno));
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.  Need a loop because of possible race condition against other
		 * would-be creators.
		 */
		if (fio_unlink(lock_file, FIO_BACKUP_HOST) < 0)
			elog(ERROR, "Could not remove old lock file \"%s\": %s",
				 lock_file, strerror(errno));
	}

	/*
	 * Successfully created the file, now fill it.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n", my_pid);

	errno = 0;
	if (fio_write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);
		/* if write didn't set errno, assume problem is no disk space */
		errno = save_errno ? save_errno : ENOSPC;
		elog(ERROR, "Could not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}
	if (fio_flush(fd) != 0)
	{
		int			save_errno = errno;

		fio_close(fd);
		fio_unlink(lock_file, FIO_BACKUP_HOST);
		errno = save_errno;
		elog(ERROR, "Could not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}
	if (fio_close(fd) != 0)
	{
		int			save_errno = errno;

		fio_unlink(lock_file, FIO_BACKUP_HOST);
		errno = save_errno;
		elog(ERROR, "Could not write lock file \"%s\": %s",
			 lock_file, strerror(errno));
	}

	/*
	 * Arrange to unlink the lock file(s) at proc_exit.
	 */
	if (!exit_hook_registered)
	{
		atexit(unlink_lock_atexit);
		exit_hook_registered = true;
	}

	/* Use parray so that the lock files are unlinked in a loop */
	if (lock_files == NULL)
		lock_files = parray_new();
	parray_append(lock_files, pgut_strdup(lock_file));

	return true;
}

/*
 * Get backup_mode in string representation.
 */
const char *
pgBackupGetBackupMode(pgBackup *backup)
{
	return backupModes[backup->backup_mode];
}

static bool
IsDir(const char *dirpath, const char *entry, fio_location location)
{
	char		path[MAXPGPATH];
	struct stat	st;

	snprintf(path, MAXPGPATH, "%s/%s", dirpath, entry);

	return fio_stat(path, &st, false, location) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Create list of instances in given backup catalog.
 *
 * Returns parray of "InstanceConfig" structures, filled with
 * actual config of each instance.
 */
parray *
catalog_get_instance_list(void)
{
	char		path[MAXPGPATH];
	DIR		   *dir;
	struct dirent *dent;
	parray		*instances;

	instances = parray_new();

	/* open directory and list contents */
	join_path_components(path, backup_path, BACKUPS_DIR);
	dir = opendir(path);
	if (dir == NULL)
		elog(ERROR, "Cannot open directory \"%s\": %s",
			 path, strerror(errno));

	while (errno = 0, (dent = readdir(dir)) != NULL)
	{
		char		child[MAXPGPATH];
		struct stat	st;
		InstanceConfig *instance;

		/* skip entries point current dir or parent dir */
		if (strcmp(dent->d_name, ".") == 0 ||
			strcmp(dent->d_name, "..") == 0)
			continue;

		join_path_components(child, path, dent->d_name);

		if (lstat(child, &st) == -1)
			elog(ERROR, "Cannot stat file \"%s\": %s",
					child, strerror(errno));

		if (!S_ISDIR(st.st_mode))
			continue;

		instance = readInstanceConfigFile(dent->d_name);

		parray_append(instances, instance);
	}

	/* TODO 3.0: switch to ERROR */
	if (parray_num(instances) == 0)
		elog(WARNING, "This backup catalog contains no backup instances. Backup instance can be added via 'add-instance' command.");

	if (errno)
		elog(ERROR, "Cannot read directory \"%s\": %s",
				path, strerror(errno));

	if (closedir(dir))
		elog(ERROR, "Cannot close directory \"%s\": %s",
				path, strerror(errno));

	return instances;
}

/*
 * Create list of backups.
 * If 'requested_backup_id' is INVALID_BACKUP_ID, return list of all backups.
 * The list is sorted in order of descending start time.
 * If valid backup id is passed only matching backup will be added to the list.
 */
parray *
catalog_get_backup_list(const char *instance_name, time_t requested_backup_id)
{
	DIR		   *data_dir = NULL;
	struct dirent *data_ent = NULL;
	parray	   *backups = NULL;
	int			i;
	char backup_instance_path[MAXPGPATH];

	sprintf(backup_instance_path, "%s/%s/%s",
			backup_path, BACKUPS_DIR, instance_name);

	/* open backup instance backups directory */
	data_dir = fio_opendir(backup_instance_path, FIO_BACKUP_HOST);
	if (data_dir == NULL)
	{
		elog(WARNING, "cannot open directory \"%s\": %s", backup_instance_path,
			strerror(errno));
		goto err_proc;
	}

	/* scan the directory and list backups */
	backups = parray_new();
	for (; (data_ent = fio_readdir(data_dir)) != NULL; errno = 0)
	{
		char		backup_conf_path[MAXPGPATH];
		char		data_path[MAXPGPATH];
		pgBackup   *backup = NULL;

		/* skip not-directory entries and hidden entries */
		if (!IsDir(backup_instance_path, data_ent->d_name, FIO_BACKUP_HOST)
			|| data_ent->d_name[0] == '.')
			continue;

		/* open subdirectory of specific backup */
		join_path_components(data_path, backup_instance_path, data_ent->d_name);

		/* read backup information from BACKUP_CONTROL_FILE */
		snprintf(backup_conf_path, MAXPGPATH, "%s/%s", data_path, BACKUP_CONTROL_FILE);
		backup = readBackupControlFile(backup_conf_path);

		if (!backup)
		{
			backup = pgut_new(pgBackup);
			pgBackupInit(backup);
			backup->start_time = base36dec(data_ent->d_name);
		}
		else if (strcmp(base36enc(backup->start_time), data_ent->d_name) != 0)
		{
			elog(WARNING, "backup ID in control file \"%s\" doesn't match name of the backup folder \"%s\"",
				 base36enc(backup->start_time), backup_conf_path);
		}

		backup->backup_id = backup->start_time;
		if (requested_backup_id != INVALID_BACKUP_ID
			&& requested_backup_id != backup->start_time)
		{
			pgBackupFree(backup);
			continue;
		}
		parray_append(backups, backup);

		if (errno && errno != ENOENT)
		{
			elog(WARNING, "cannot read data directory \"%s\": %s",
				 data_ent->d_name, strerror(errno));
			goto err_proc;
		}
	}
	if (errno)
	{
		elog(WARNING, "cannot read backup root directory \"%s\": %s",
			backup_instance_path, strerror(errno));
		goto err_proc;
	}

	fio_closedir(data_dir);
	data_dir = NULL;

	parray_qsort(backups, pgBackupCompareIdDesc);

	/* Link incremental backups with their ancestors.*/
	for (i = 0; i < parray_num(backups); i++)
	{
		pgBackup   *curr = parray_get(backups, i);
		pgBackup  **ancestor;
		pgBackup	key;

		if (curr->backup_mode == BACKUP_MODE_FULL)
			continue;

		key.start_time = curr->parent_backup;
		ancestor = (pgBackup **) parray_bsearch(backups, &key,
												pgBackupCompareIdDesc);
		if (ancestor)
			curr->parent_backup_link = *ancestor;
	}

	return backups;

err_proc:
	if (data_dir)
		fio_closedir(data_dir);
	if (backups)
		parray_walk(backups, pgBackupFree);
	parray_free(backups);

	elog(ERROR, "Failed to get backup list");

	return NULL;
}

/*
 * Create list of backup datafiles.
 * If 'requested_backup_id' is INVALID_BACKUP_ID, exit with error.
 * If valid backup id is passed only matching backup will be added to the list.
 * TODO this function only used once. Is it really needed?
 */
parray *
get_backup_filelist(pgBackup *backup)
{
	parray		*files = NULL;
	char		backup_filelist_path[MAXPGPATH];

	pgBackupGetPath(backup, backup_filelist_path, lengthof(backup_filelist_path), DATABASE_FILE_LIST);
	files = dir_read_file_list(NULL, NULL, backup_filelist_path, FIO_BACKUP_HOST);

	/* redundant sanity? */
	if (!files)
		elog(ERROR, "Failed to get filelist for backup %s", base36enc(backup->start_time));

	return files;
}

/*
 * Lock list of backups. Function goes in backward direction.
 */
void
catalog_lock_backup_list(parray *backup_list, int from_idx, int to_idx)
{
	int			start_idx,
				end_idx;
	int			i;

	if (parray_num(backup_list) == 0)
		return;

	start_idx = Max(from_idx, to_idx);
	end_idx = Min(from_idx, to_idx);

	for (i = start_idx; i >= end_idx; i--)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backup_list, i);
		if (!lock_backup(backup))
			elog(ERROR, "Cannot lock backup %s directory",
				 base36enc(backup->start_time));
	}
}

/*
 * Find the latest valid child of latest valid FULL backup on given timeline
 */
pgBackup *
catalog_get_last_data_backup(parray *backup_list, TimeLineID tli, time_t current_start_time)
{
	int			i;
	pgBackup   *full_backup = NULL;
	pgBackup   *tmp_backup = NULL;
	char 	   *invalid_backup_id;

	/* backup_list is sorted in order of descending ID */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *) parray_get(backup_list, i);

		if ((backup->backup_mode == BACKUP_MODE_FULL &&
			(backup->status == BACKUP_STATUS_OK ||
			 backup->status == BACKUP_STATUS_DONE)) && backup->tli == tli)
		{
			full_backup = backup;
			break;
		}
	}

	/* Failed to find valid FULL backup to fulfill ancestor role */
	if (!full_backup)
		return NULL;

	elog(LOG, "Latest valid FULL backup: %s",
		base36enc(full_backup->start_time));

	/* FULL backup is found, lets find his latest child */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup *backup = (pgBackup *) parray_get(backup_list, i);

		/* only valid descendants are acceptable for evaluation */
		if ((backup->status == BACKUP_STATUS_OK ||
			backup->status == BACKUP_STATUS_DONE))
		{
			switch (scan_parent_chain(backup, &tmp_backup))
			{
				/* broken chain */
				case 0:
					invalid_backup_id = base36enc_dup(tmp_backup->parent_backup);

					elog(WARNING, "Backup %s has missing parent: %s. Cannot be a parent",
						base36enc(backup->start_time), invalid_backup_id);
					pg_free(invalid_backup_id);
					continue;

				/* chain is intact, but at least one parent is invalid */
				case 1:
					invalid_backup_id = base36enc_dup(tmp_backup->start_time);

					elog(WARNING, "Backup %s has invalid parent: %s. Cannot be a parent",
						base36enc(backup->start_time), invalid_backup_id);
					pg_free(invalid_backup_id);
					continue;

				/* chain is ok */
				case 2:
					/* Yes, we could call is_parent() earlier - after choosing the ancestor,
					 * but this way we have an opportunity to detect and report all possible
					 * anomalies.
					 */
					if (is_parent(full_backup->start_time, backup, true))
					{
						elog(INFO, "Parent backup: %s",
							base36enc(backup->start_time));
						return backup;
					}
			}
		}
		/* skip yourself */
		else if (backup->start_time == current_start_time)
			continue;
		else
		{
			elog(WARNING, "Backup %s has status: %s. Cannot be a parent.",
				base36enc(backup->start_time), status2str(backup->status));
		}
	}

	return NULL;
}

/* create backup directory in $BACKUP_PATH */
int
pgBackupCreateDir(pgBackup *backup)
{
	int		i;
	char	path[MAXPGPATH];
	parray *subdirs = parray_new();

	parray_append(subdirs, pg_strdup(DATABASE_DIR));

	/* Add external dirs containers */
	if (backup->external_dir_str)
	{
		parray *external_list;

		external_list = make_external_directory_list(backup->external_dir_str,
													 false);
		for (i = 0; i < parray_num(external_list); i++)
		{
			char		temp[MAXPGPATH];
			/* Numeration of externaldirs starts with 1 */
			makeExternalDirPathByNum(temp, EXTERNAL_DIR, i+1);
			parray_append(subdirs, pg_strdup(temp));
		}
		free_dir_list(external_list);
	}

	pgBackupGetPath(backup, path, lengthof(path), NULL);

	if (!dir_is_empty(path, FIO_BACKUP_HOST))
		elog(ERROR, "backup destination is not empty \"%s\"", path);

	fio_mkdir(path, DIR_PERMISSION, FIO_BACKUP_HOST);

	/* create directories for actual backup files */
	for (i = 0; i < parray_num(subdirs); i++)
	{
		pgBackupGetPath(backup, path, lengthof(path), parray_get(subdirs, i));
		fio_mkdir(path, DIR_PERMISSION, FIO_BACKUP_HOST);
	}

	free_dir_list(subdirs);
	return 0;
}

/*
 * Create list of timelines
 */
parray *
catalog_get_timelines(InstanceConfig *instance)
{
	parray *xlog_files_list = parray_new();
	parray *timelineinfos;
	parray *backups;
	timelineInfo *tlinfo;
	char		arclog_path[MAXPGPATH];

	/* read all xlog files that belong to this archive */
	sprintf(arclog_path, "%s/%s/%s", backup_path, "wal", instance->name);
	dir_list_file(xlog_files_list, arclog_path, false, false, false, 0, FIO_BACKUP_HOST);
	parray_qsort(xlog_files_list, pgFileComparePath);

	timelineinfos = parray_new();
	tlinfo = NULL;

	/* walk through files and collect info about timelines */
	for (int i = 0; i < parray_num(xlog_files_list); i++)
	{
		pgFile *file = (pgFile *) parray_get(xlog_files_list, i);
		TimeLineID tli;
		parray *timelines;
		xlogFile *wal_file = NULL;

		/* regular WAL file */
		if (strspn(file->name, "0123456789ABCDEF") == XLOG_FNAME_LEN)
		{
			int result = 0;
			uint32 log, seg;
			XLogSegNo segno;
			char suffix[MAXPGPATH];

			result = sscanf(file->name, "%08X%08X%08X.%s",
						&tli, &log, &seg, (char *) &suffix);

			if (result < 3)
			{
				elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
				continue;
			}

			segno = log * instance->xlog_seg_size + seg;

			/* regular WAL file with suffix */
			if (result == 4)
			{
				/* backup history file. Currently we don't use them */
				if (IsBackupHistoryFileName(file->name))
				{
					elog(VERBOSE, "backup history file \"%s\"", file->name);

					if (!tlinfo || tlinfo->tli != tli)
					{
						tlinfo = timelineInfoNew(tli);
						parray_append(timelineinfos, tlinfo);
					}

					/* append file to xlog file list */
					wal_file = palloc(sizeof(xlogFile));
					wal_file->file = *file;
					wal_file->segno = segno;
					wal_file->type = BACKUP_HISTORY_FILE;
					wal_file->keep = false;
					parray_append(tlinfo->xlog_filelist, wal_file);
					continue;
				}
				/* partial WAL segment */
				else if (IsPartialXLogFileName(file->name))
				{
					elog(VERBOSE, "partial WAL file \"%s\"", file->name);

					if (!tlinfo || tlinfo->tli != tli)
					{
						tlinfo = timelineInfoNew(tli);
						parray_append(timelineinfos, tlinfo);
					}

					/* append file to xlog file list */
					wal_file = palloc(sizeof(xlogFile));
					wal_file->file = *file;
					wal_file->segno = segno;
					wal_file->type = PARTIAL_SEGMENT;
					wal_file->keep = false;
					parray_append(tlinfo->xlog_filelist, wal_file);
					continue;
				}
				/* we only expect compressed wal files with .gz suffix */
				else if (strcmp(suffix, "gz") != 0)
				{
					elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
					continue;
				}
			}

			/* new file belongs to new timeline */
			if (!tlinfo || tlinfo->tli != tli)
			{
				tlinfo = timelineInfoNew(tli);
				parray_append(timelineinfos, tlinfo);
			}
			/*
			 * As it is impossible to detect if segments before segno are lost,
			 * or just do not exist, do not report them as lost.
			 */
			else if (tlinfo->n_xlog_files != 0)
			{
				/* check, if segments are consequent */
				XLogSegNo expected_segno = tlinfo->end_segno + 1;

				/*
				 * Some segments are missing. remember them in lost_segments to report.
				 * Normally we expect that segment numbers form an increasing sequence,
				 * though it's legal to find two files with equal segno in case there
				 * are both compressed and non-compessed versions. For example
				 * 000000010000000000000002 and 000000010000000000000002.gz
				 *
				 */
				if (segno != expected_segno && segno != tlinfo->end_segno)
				{
					xlogInterval *interval = palloc(sizeof(xlogInterval));;
					interval->begin_segno = expected_segno;
					interval->end_segno = segno - 1;

					if (tlinfo->lost_segments == NULL)
						tlinfo->lost_segments = parray_new();

					parray_append(tlinfo->lost_segments, interval);
				}
			}

			if (tlinfo->begin_segno == 0)
				tlinfo->begin_segno = segno;

			/* this file is the last for this timeline so far */
			tlinfo->end_segno = segno;
			/* update counters */
			tlinfo->n_xlog_files++;
			tlinfo->size += file->size;

			/* append file to xlog file list */
			wal_file = palloc(sizeof(xlogFile));
			wal_file->file = *file;
			wal_file->segno = segno;
			wal_file->type = SEGMENT;
			wal_file->keep = false;
			parray_append(tlinfo->xlog_filelist, wal_file);
		}
		/* timeline history file */
		else if (IsTLHistoryFileName(file->name))
		{
			TimeLineHistoryEntry *tln;

			sscanf(file->name, "%08X.history", &tli);
			timelines = read_timeline_history(arclog_path, tli);

			if (!tlinfo || tlinfo->tli != tli)
			{
				tlinfo = timelineInfoNew(tli);
				parray_append(timelineinfos, tlinfo);
				/*
				 * 1 is the latest timeline in the timelines list.
				 * 0 - is our timeline, which is of no interest here
				 */
				tln = (TimeLineHistoryEntry *) parray_get(timelines, 1);
				tlinfo->switchpoint = tln->end;
				tlinfo->parent_tli = tln->tli;

				/* find parent timeline to link it with this one */
				for (int i = 0; i < parray_num(timelineinfos); i++)
				{
					timelineInfo *cur = (timelineInfo *) parray_get(timelineinfos, i);
					if (cur->tli == tlinfo->parent_tli)
					{
						tlinfo->parent_link = cur;
						break;
					}
				}
			}

			parray_walk(timelines, pfree);
			parray_free(timelines);
		}
		else
			elog(WARNING, "unexpected WAL file name \"%s\"", file->name);
	}

	/* save information about backups belonging to each timeline */
	backups = catalog_get_backup_list(instance->name, INVALID_BACKUP_ID);

	for (int i = 0; i < parray_num(timelineinfos); i++)
	{
		timelineInfo *tlinfo = parray_get(timelineinfos, i);
		for (int j = 0; j < parray_num(backups); j++)
		{
			pgBackup *backup = parray_get(backups, j);
			if (tlinfo->tli == backup->tli)
			{
				if (tlinfo->backups == NULL)
					tlinfo->backups = parray_new();

				parray_append(tlinfo->backups, backup);
			}
		}
	}

	/* determine oldest backup and closest backup for every timeline */
	for (int i = 0; i < parray_num(timelineinfos); i++)
	{
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		tlinfo->oldest_backup = get_oldest_backup(tlinfo);
		tlinfo->closest_backup = get_closest_backup(tlinfo);
	}

	/* determine which WAL segments must be kept because of wal retention */
	if (instance->wal_depth <= 0)
		return timelineinfos;

	/*
	 * WAL retention for now is fairly simple.
	 * User can set only one parameter - 'wal-depth'.
	 * It determines the starting segment of WAL (anchor_segno)
	 * that must be kept by providing the serial number of backup
	 * (anchor_backup) which start_lsn is to use for anchor_segno calculation.
	 *
	 * From user POV 'wal-depth' determine how many valid(!) backups in timeline
	 * should have an ability to perform PITR:
	 * Consider the example:
	 *
	 * ---B-------B1-------B2-------B3--------> WAL timeline1
	 *
	 * If 'wal-depth' is set to 2, then WAL purge should produce the following result:
	 *
	 *    B       B1       B2-------B3--------> WAL timeline1
	 *
	 * Only valid backup can satisfy 'wal-depth' condition, so if B3 is not OK or DONE,
	 * then WAL purge should produce the following result:
	 *    B       B1-------B2-------B3--------> WAL timeline1
	 *
	 * Complicated cases, such as brached timelines are taken into account.
	 * wal-depth is applied to each timeline independently:
	 *
	 *        |--------->                       WAL timeline2
	 * ---B---|---B1-------B2-------B3--------> WAL timeline1
	 *
	 * after WAL purge with wal-depth=2:
	 * TODO implement this in the code
	 *
	 *        |--------->                       WAL timeline2
	 *    B---|   B1       B2-------B3--------> WAL timeline1
	 *
	 * In this example timeline2 prevents purge of WAL reachable from B backup.
	 *
	 * To determine the segno that should be protected by WAL retention
	 * we set anchor_backup, which START LSN is used to calculate this segno.
 	 * With wal-depth=2 anchor_backup is B2.
	 *
 	 * Second part is ARCHIVE backups handling.
 	 * If B and B1 have ARCHIVE wal-mode, then we must preserve WAL intervals
 	 * between start_lsn and stop_lsn for each of them.
	 *
	 * For this we store this intervals in keep_segments array and
	 * must consult them during retention purge.
	 */

	/* determine anchor_lsn and keep_segments for every timeline */
	for (int i = 0; i < parray_num(timelineinfos); i++)
	{
		int count = 0;
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		/*
		 * Iterate backward on backups belonging to this timeline to find
		 * anchor_backup. NOTE Here we rely on the fact that backups list
		 * is ordered by start_lsn DESC.
		 */
		if (tlinfo->backups)
		{
			for (int j = 0; j < parray_num(tlinfo->backups); j++)
			{
				pgBackup *backup = parray_get(tlinfo->backups, j);

				/* skip invalid backups */
				if (backup->status != BACKUP_STATUS_OK &&
					backup->status != BACKUP_STATUS_DONE)
					continue;

				/* sanity */
				if (XLogRecPtrIsInvalid(backup->start_lsn) ||
					backup->tli <= 0)
					continue;

				elog(VERBOSE, "Timeline %i: backup %s",
						tlinfo->tli, base36enc(backup->start_time));

				count++;

				if (count == instance->wal_depth)
				{
					elog(VERBOSE, "Timeline %i: ANCHOR %s, TLI %i",
						 tlinfo->tli, base36enc(backup->start_time), backup->tli);
					tlinfo->anchor_lsn = backup->start_lsn;
					tlinfo->anchor_tli = backup->tli;
					break;
				}
			}
		}

		/*
		 * Failed to find anchor backup for this timeline.
		 * We cannot just thrown it to the wolves, because by
		 * doing that we will violate our own guarantees.
		 * So check the existence of closest_backup for
		 * this timeline. If there is one, then
		 * set the anchor_backup to closest_backup.
		 *                      |-------------B4----------> WAL timeline3
		 *                |-----|-------------------------> WAL timeline2
		 *      B    B1---|        B2     B3-------B5-----> WAL timeline1
		 *
		 * wal-depth=2
		 *
		 * If number of valid backups on timelines is less than 'wal-depth'
		 * then timeline must(!) stay reachable via parent timelines if possible.
		 * It means, that we must for timeline2 sake protect backup B1 im timeline1
		 * from purge. To do so, we must set anchor_backup for timeline1 to B1,
		 * even though wal-depth setting point to B3.
		 */
		if (XLogRecPtrIsInvalid(tlinfo->anchor_lsn))
		{
			/*
			 * Failed to find anchor_lsn in our own timeline.
			 * Consider the case:
			 * -------------------------------------> tli3
			 *                     S3`--------------> tli2
			 *      S1`------------S3------B3-------> tli2
			 * B1---S1-------------B2---------------> tli1
			 *
			 * B* - backups
			 * S* - switchpoints
			 * wal-depth=1
			 *
			 * Expected result:
			 *                     S2`--------------> tli2
			 *      S1`------------S2      B3-------> tli2
			 * B1---S1             B2---------------> tli1
			 */
			pgBackup *closest_backup = NULL;
			xlogInterval *interval = NULL;
			/* check if tli has closest_backup */
			if (!tlinfo->closest_backup)
				/* timeline has no closest_backup, wal retention cannot be
				 * applied to this timeline.
				 * Timeline will be purged up to oldest_backup if any or
				 * purge entirely if there is none.
				 * In example above: tli3.
				 */
				continue;

			/* sanity for closest_backup */
			if (XLogRecPtrIsInvalid(tlinfo->closest_backup->start_lsn) ||
				tlinfo->closest_backup->tli <= 0)
				continue;

			/* Set anchor_lsn and anchor_tli to protect current timeline from purge
			 * In the example above tli2 will be protected.
			 */
			tlinfo->anchor_lsn = tlinfo->closest_backup->start_lsn;
			tlinfo->anchor_tli = tlinfo->closest_backup->tli;

			/* closest backup may be located not in parent timeline */
			closest_backup = tlinfo->closest_backup;

			/*
			 * Iterate over parent timeline chain and
			 * look for timeline where closest_backup belong
			 */
			while (tlinfo->parent_link)
			{
				/* save branch_tli for savepoint */
				XLogSegNo switch_segno = 0;
				XLogRecPtr switchpoint = tlinfo->switchpoint;

				tlinfo = tlinfo->parent_link;

				if (tlinfo->keep_segments == NULL)
					tlinfo->keep_segments = parray_new();

				interval = palloc(sizeof(xlogInterval));
				GetXLogSegNo(switchpoint, switch_segno, instance->xlog_seg_size);
				interval->end_segno = switch_segno;

				/* TODO: check, maybe this interval is already here */

				/* Save [S1`, S2] to keep_segments */
				if (tlinfo->tli != closest_backup->tli)
				{
					interval->begin_segno = tlinfo->begin_segno;
					parray_append(tlinfo->keep_segments, interval);
					continue;
				}
				/* Save [B1, S1] to keep_segments */
				else
				{
					XLogSegNo begin_segno = 0;
					GetXLogSegNo(closest_backup->start_lsn, begin_segno, instance->xlog_seg_size);
					interval->begin_segno = begin_segno;
					parray_append(tlinfo->keep_segments, interval);
					break;
				}
			}
			/* This timeline wholly saved. */
			continue;
		}

		/* Iterate over backups left */
		for (int j = count; j < parray_num(tlinfo->backups); j++)
		{
			XLogSegNo   segno = 0;
			xlogInterval *interval = NULL;
			pgBackup *backup = parray_get(tlinfo->backups, j);

			/* anchor backup is set, now we must calculate
			 * keep_segments intervals for ARCHIVE backups
			 * older than anchor backup.
			 */

			/* STREAM backups cannot contribute to keep_segments */
			if (backup->stream)
				continue;

			/* sanity */
			if (XLogRecPtrIsInvalid(backup->start_lsn) ||
				backup->tli <= 0)
				continue;

			/* no point in clogging keep_segments by backups protected by anchor_lsn */
			if (backup->start_lsn >= tlinfo->anchor_lsn)
				continue;

			/* append interval to keep_segments */
			interval = palloc(sizeof(xlogInterval));
			GetXLogSegNo(backup->start_lsn, segno, instance->xlog_seg_size);
			interval->begin_segno = segno;
			GetXLogSegNo(backup->stop_lsn, segno, instance->xlog_seg_size);

			/*
			 * On replica it is possible to get STOP_LSN pointing to contrecord,
			 * so set end_segno to the next segment after STOP_LSN just to be safe.
			 */
			if (backup->from_replica)
				interval->end_segno = segno + 1;
			else
				interval->end_segno = segno;

			if (tlinfo->keep_segments == NULL)
				tlinfo->keep_segments = parray_new();

			parray_append(tlinfo->keep_segments, interval);
		}
	}

	/*
	 * Protect WAL segments from deletion by setting 'keep' flag.
	 * We must keep all WAL segments after anchor_lsn (including), and also segments
	 * required by ARCHIVE backups for consistency - WAL between [start_lsn, stop_lsn].
	 */
	for (int i = 0; i < parray_num(timelineinfos); i++)
	{
		XLogSegNo   anchor_segno = 0;
		timelineInfo *tlinfo = parray_get(timelineinfos, i);

		/* At this point invalid anchor_lsn can be only in one case:
		 * timeline is going to be purged by regular WAL purge rules.
		 */
		if (XLogRecPtrIsInvalid(tlinfo->anchor_lsn))
			continue;

		/* anchor_lsn located in another timeline, it means that the timeline
		 * will be protected from purge entirely.
		 */
		if (tlinfo->anchor_tli > 0 && tlinfo->anchor_tli != tlinfo->tli)
			continue;

		GetXLogSegNo(tlinfo->anchor_lsn, anchor_segno, instance->xlog_seg_size);

		for (int i = 0; i < parray_num(tlinfo->xlog_filelist); i++)
		{
			xlogFile *wal_file = (xlogFile *) parray_get(tlinfo->xlog_filelist, i);

			if (wal_file->segno >= anchor_segno)
			{
				wal_file->keep = true;
				continue;
			}

			/* no keep segments */
			if (!tlinfo->keep_segments)
				continue;

			/* protect segments belonging to one of the keep invervals */
			for (int j = 0; j < parray_num(tlinfo->keep_segments); j++)
			{
				xlogInterval *keep_segments = (xlogInterval *) parray_get(tlinfo->keep_segments, j);

				if ((wal_file->segno >= keep_segments->begin_segno) &&
					wal_file->segno <= keep_segments->end_segno)
				{
					wal_file->keep = true;
					break;
				}
			}
		}
	}

	return timelineinfos;
}

/*
 * Iterate over parent timelines and look for valid backup
 * closest to given timeline switchpoint.
 *
 * If such backup doesn't exist, it means that
 * timeline is unreachable. Return NULL.
 */
pgBackup*
get_closest_backup(timelineInfo *tlinfo)
{
	pgBackup *closest_backup = NULL;
	int i;

	/*
	 * Iterate over backups belonging to parent timelines
	 * and look for candidates.
	 */
	while (tlinfo->parent_link && !closest_backup)
	{
		parray *backup_list = tlinfo->parent_link->backups;
		if (backup_list != NULL)
		{
			for (i = 0; i < parray_num(backup_list); i++)
			{
				pgBackup   *backup = parray_get(backup_list, i);

				/*
				 * Only valid backups made before switchpoint
				 * should be considered.
				 */
				if (!XLogRecPtrIsInvalid(backup->stop_lsn) &&
					XRecOffIsValid(backup->stop_lsn) &&
					backup->stop_lsn <= tlinfo->switchpoint &&
					(backup->status == BACKUP_STATUS_OK ||
					backup->status == BACKUP_STATUS_DONE))
				{
					/* Check if backup is closer to switchpoint than current candidate */
					if (!closest_backup || backup->stop_lsn > closest_backup->stop_lsn)
						closest_backup = backup;
				}
			}
		}

		/* Continue with parent */
		tlinfo = tlinfo->parent_link;
	}

	return closest_backup;
}

/*
 * Find oldest backup in given timeline
 * to determine what WAL segments of this timeline
 * are reachable from backups belonging to it.
 *
 * If such backup doesn't exist, it means that
 * there is no backups on this timeline. Return NULL.
 */
pgBackup*
get_oldest_backup(timelineInfo *tlinfo)
{
	pgBackup *oldest_backup = NULL;
	int i;
	parray *backup_list = tlinfo->backups;

	if (backup_list != NULL)
	{
		for (i = 0; i < parray_num(backup_list); i++)
		{
			pgBackup   *backup = parray_get(backup_list, i);

			/* Backups with invalid START LSN can be safely skipped */
			if (XLogRecPtrIsInvalid(backup->start_lsn) ||
				!XRecOffIsValid(backup->start_lsn))
				continue;

			/*
			 * Check if backup is older than current candidate.
			 * Here we use start_lsn for comparison, because backup that
			 * started earlier needs more WAL.
			 */
			if (!oldest_backup || backup->start_lsn < oldest_backup->start_lsn)
				oldest_backup = backup;
		}
	}

	return oldest_backup;
}

/*
 * Write information about backup.in to stream "out".
 */
void
pgBackupWriteControl(FILE *out, pgBackup *backup)
{
	char		timestamp[100];

	fio_fprintf(out, "#Configuration\n");
	fio_fprintf(out, "backup-mode = %s\n", pgBackupGetBackupMode(backup));
	fio_fprintf(out, "stream = %s\n", backup->stream ? "true" : "false");
	fio_fprintf(out, "compress-alg = %s\n",
			deparse_compress_alg(backup->compress_alg));
	fio_fprintf(out, "compress-level = %d\n", backup->compress_level);
	fio_fprintf(out, "from-replica = %s\n", backup->from_replica ? "true" : "false");

	fio_fprintf(out, "\n#Compatibility\n");
	fio_fprintf(out, "block-size = %u\n", backup->block_size);
	fio_fprintf(out, "xlog-block-size = %u\n", backup->wal_block_size);
	fio_fprintf(out, "checksum-version = %u\n", backup->checksum_version);
	if (backup->program_version[0] != '\0')
		fio_fprintf(out, "program-version = %s\n", backup->program_version);
	if (backup->server_version[0] != '\0')
		fio_fprintf(out, "server-version = %s\n", backup->server_version);

	fio_fprintf(out, "\n#Result backup info\n");
	fio_fprintf(out, "timelineid = %d\n", backup->tli);
	/* LSN returned by pg_start_backup */
	fio_fprintf(out, "start-lsn = %X/%X\n",
			(uint32) (backup->start_lsn >> 32),
			(uint32) backup->start_lsn);
	/* LSN returned by pg_stop_backup */
	fio_fprintf(out, "stop-lsn = %X/%X\n",
			(uint32) (backup->stop_lsn >> 32),
			(uint32) backup->stop_lsn);

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	fio_fprintf(out, "start-time = '%s'\n", timestamp);
	if (backup->merge_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->merge_time);
		fio_fprintf(out, "merge-time = '%s'\n", timestamp);
	}
	if (backup->end_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->end_time);
		fio_fprintf(out, "end-time = '%s'\n", timestamp);
	}
	fio_fprintf(out, "recovery-xid = " XID_FMT "\n", backup->recovery_xid);
	if (backup->recovery_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time);
		fio_fprintf(out, "recovery-time = '%s'\n", timestamp);
	}

	/*
	 * Size of PGDATA directory. The size does not include size of related
	 * WAL segments in archive 'wal' directory.
	 */
	if (backup->data_bytes != BYTES_INVALID)
		fio_fprintf(out, "data-bytes = " INT64_FORMAT "\n", backup->data_bytes);

	if (backup->wal_bytes != BYTES_INVALID)
		fio_fprintf(out, "wal-bytes = " INT64_FORMAT "\n", backup->wal_bytes);

	if (backup->uncompressed_bytes >= 0)
		fio_fprintf(out, "uncompressed-bytes = " INT64_FORMAT "\n", backup->uncompressed_bytes);

	if (backup->pgdata_bytes >= 0)
		fio_fprintf(out, "pgdata-bytes = " INT64_FORMAT "\n", backup->pgdata_bytes);

	fio_fprintf(out, "status = %s\n", status2str(backup->status));

	/* 'parent_backup' is set if it is incremental backup */
	if (backup->parent_backup != 0)
		fio_fprintf(out, "parent-backup-id = '%s'\n", base36enc(backup->parent_backup));

	/* print connection info except password */
	if (backup->primary_conninfo)
		fio_fprintf(out, "primary_conninfo = '%s'\n", backup->primary_conninfo);

	/* print external directories list */
	if (backup->external_dir_str)
		fio_fprintf(out, "external-dirs = '%s'\n", backup->external_dir_str);
}

/*
 * Save the backup content into BACKUP_CONTROL_FILE.
 */
void
write_backup(pgBackup *backup)
{
	FILE	   *fp = NULL;
	char		path[MAXPGPATH];
	char		path_temp[MAXPGPATH];
	int			errno_temp;

	pgBackupGetPath(backup, path, lengthof(path), BACKUP_CONTROL_FILE);
	snprintf(path_temp, sizeof(path_temp), "%s.tmp", path);

	fp = fio_fopen(path_temp, PG_BINARY_W, FIO_BACKUP_HOST);
	if (fp == NULL)
		elog(ERROR, "Cannot open configuration file \"%s\": %s",
			 path_temp, strerror(errno));

	pgBackupWriteControl(fp, backup);

	if (fio_fflush(fp) || fio_fclose(fp))
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot write configuration file \"%s\": %s",
			 path_temp, strerror(errno_temp));
	}

	if (fio_rename(path_temp, path, FIO_BACKUP_HOST) < 0)
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot rename configuration file \"%s\" to \"%s\": %s",
			 path_temp, path, strerror(errno_temp));
	}
}

/*
 * Output the list of files to backup catalog DATABASE_FILE_LIST
 */
void
write_backup_filelist(pgBackup *backup, parray *files, const char *root,
					  parray *external_list)
{
	FILE	   *out;
	char		path[MAXPGPATH];
	char		path_temp[MAXPGPATH];
	int			errno_temp;
	size_t		i = 0;
	#define BUFFERSZ BLCKSZ*500
	char		buf[BUFFERSZ];
	size_t		write_len = 0;
	int64 		backup_size_on_disk = 0;
	int64 		uncompressed_size_on_disk = 0;
	int64 		wal_size_on_disk = 0;

	pgBackupGetPath(backup, path, lengthof(path), DATABASE_FILE_LIST);
	snprintf(path_temp, sizeof(path_temp), "%s.tmp", path);

	out = fio_fopen(path_temp, PG_BINARY_W, FIO_BACKUP_HOST);
	if (out == NULL)
		elog(ERROR, "Cannot open file list \"%s\": %s", path_temp,
			 strerror(errno));

	/* print each file in the list */
	while(i < parray_num(files))
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);
		char	   *path = file->path; /* for streamed WAL files */
		char	line[BLCKSZ];
		int 	len = 0;

		i++;

		if (S_ISDIR(file->mode))
		{
			backup_size_on_disk += 4096;
			uncompressed_size_on_disk += 4096;
		}

		/* Count the amount of the data actually copied */
		if (S_ISREG(file->mode) && file->write_size > 0)
		{
			/*
			 * Size of WAL files in 'pg_wal' is counted separately
			 * TODO: in 3.0 add attribute is_walfile
			 */
			if (IsXLogFileName(file->name) && (file->external_dir_num == 0))
				wal_size_on_disk += file->write_size;
			else
			{
				backup_size_on_disk += file->write_size;
				uncompressed_size_on_disk += file->uncompressed_size;
			}
		}

		/* for files from PGDATA and external files use rel_path
		 * streamed WAL files has rel_path relative not to "database/"
		 * but to "database/pg_wal", so for them use path.
		 */
		if ((root && strstr(path, root) == path) ||
			(file->external_dir_num && external_list))
				path = file->rel_path;

		len = sprintf(line, "{\"path\":\"%s\", \"size\":\"" INT64_FORMAT "\", "
					 "\"mode\":\"%u\", \"is_datafile\":\"%u\", "
					 "\"is_cfs\":\"%u\", \"crc\":\"%u\", "
					 "\"compress_alg\":\"%s\", \"external_dir_num\":\"%d\", "
					 "\"dbOid\":\"%u\"",
					path, file->write_size, file->mode,
					file->is_datafile ? 1 : 0,
					file->is_cfs ? 1 : 0,
					file->crc,
					deparse_compress_alg(file->compress_alg),
					file->external_dir_num,
					file->dbOid);

		if (file->is_datafile)
			len += sprintf(line+len, ",\"segno\":\"%d\"", file->segno);

		if (file->linked)
			len += sprintf(line+len, ",\"linked\":\"%s\"", file->linked);

		if (file->n_blocks != BLOCKNUM_INVALID)
			len += sprintf(line+len, ",\"n_blocks\":\"%i\"", file->n_blocks);

		len += sprintf(line+len, "}\n");

		if (write_len + len <= BUFFERSZ)
		{
			memcpy(buf+write_len, line, len);
			write_len += len;
		}
		else
		{
			/* write buffer to file */
			if (fio_fwrite(out, buf, write_len) != write_len)
			{
				errno_temp = errno;
				fio_unlink(path_temp, FIO_BACKUP_HOST);
				elog(ERROR, "Cannot write file list \"%s\": %s",
					path_temp, strerror(errno));
			}
			/* reset write_len */
			write_len = 0;
		}
	}

	/* write what is left in the buffer to file */
	if (write_len > 0)
		if (fio_fwrite(out, buf, write_len) != write_len)
		{
			errno_temp = errno;
			fio_unlink(path_temp, FIO_BACKUP_HOST);
			elog(ERROR, "Cannot write file list \"%s\": %s",
				path_temp, strerror(errno));
		}

	if (fio_fflush(out) || fio_fclose(out))
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot write file list \"%s\": %s",
			 path_temp, strerror(errno));
	}

	if (fio_rename(path_temp, path, FIO_BACKUP_HOST) < 0)
	{
		errno_temp = errno;
		fio_unlink(path_temp, FIO_BACKUP_HOST);
		elog(ERROR, "Cannot rename configuration file \"%s\" to \"%s\": %s",
			 path_temp, path, strerror(errno_temp));
	}

	/* use extra variable to avoid reset of previous data_bytes value in case of error */
	backup->data_bytes = backup_size_on_disk;
	backup->wal_bytes = wal_size_on_disk;
	backup->uncompressed_bytes = uncompressed_size_on_disk;
}

/*
 * Read BACKUP_CONTROL_FILE and create pgBackup.
 *  - Comment starts with ';'.
 *  - Do not care section.
 */
static pgBackup *
readBackupControlFile(const char *path)
{
	pgBackup   *backup = pgut_new(pgBackup);
	char	   *backup_mode = NULL;
	char	   *start_lsn = NULL;
	char	   *stop_lsn = NULL;
	char	   *status = NULL;
	char	   *parent_backup = NULL;
	char	   *program_version = NULL;
	char	   *server_version = NULL;
	char	   *compress_alg = NULL;
	int			parsed_options;

	ConfigOption options[] =
	{
		{'s', 0, "backup-mode",			&backup_mode, SOURCE_FILE_STRICT},
		{'u', 0, "timelineid",			&backup->tli, SOURCE_FILE_STRICT},
		{'s', 0, "start-lsn",			&start_lsn, SOURCE_FILE_STRICT},
		{'s', 0, "stop-lsn",			&stop_lsn, SOURCE_FILE_STRICT},
		{'t', 0, "start-time",			&backup->start_time, SOURCE_FILE_STRICT},
		{'t', 0, "merge-time",			&backup->merge_time, SOURCE_FILE_STRICT},
		{'t', 0, "end-time",			&backup->end_time, SOURCE_FILE_STRICT},
		{'U', 0, "recovery-xid",		&backup->recovery_xid, SOURCE_FILE_STRICT},
		{'t', 0, "recovery-time",		&backup->recovery_time, SOURCE_FILE_STRICT},
		{'I', 0, "data-bytes",			&backup->data_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "wal-bytes",			&backup->wal_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "uncompressed-bytes",	&backup->uncompressed_bytes, SOURCE_FILE_STRICT},
		{'I', 0, "pgdata-bytes",		&backup->pgdata_bytes, SOURCE_FILE_STRICT},
		{'u', 0, "block-size",			&backup->block_size, SOURCE_FILE_STRICT},
		{'u', 0, "xlog-block-size",		&backup->wal_block_size, SOURCE_FILE_STRICT},
		{'u', 0, "checksum-version",	&backup->checksum_version, SOURCE_FILE_STRICT},
		{'s', 0, "program-version",		&program_version, SOURCE_FILE_STRICT},
		{'s', 0, "server-version",		&server_version, SOURCE_FILE_STRICT},
		{'b', 0, "stream",				&backup->stream, SOURCE_FILE_STRICT},
		{'s', 0, "status",				&status, SOURCE_FILE_STRICT},
		{'s', 0, "parent-backup-id",	&parent_backup, SOURCE_FILE_STRICT},
		{'s', 0, "compress-alg",		&compress_alg, SOURCE_FILE_STRICT},
		{'u', 0, "compress-level",		&backup->compress_level, SOURCE_FILE_STRICT},
		{'b', 0, "from-replica",		&backup->from_replica, SOURCE_FILE_STRICT},
		{'s', 0, "primary-conninfo",	&backup->primary_conninfo, SOURCE_FILE_STRICT},
		{'s', 0, "external-dirs",		&backup->external_dir_str, SOURCE_FILE_STRICT},
		{0}
	};

	pgBackupInit(backup);
	if (fio_access(path, F_OK, FIO_BACKUP_HOST) != 0)
	{
		elog(WARNING, "Control file \"%s\" doesn't exist", path);
		pgBackupFree(backup);
		return NULL;
	}

	parsed_options = config_read_opt(path, options, WARNING, true, true);

	if (parsed_options == 0)
	{
		elog(WARNING, "Control file \"%s\" is empty", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup->start_time == 0)
	{
		elog(WARNING, "Invalid ID/start-time, control file \"%s\" is corrupted", path);
		pgBackupFree(backup);
		return NULL;
	}

	if (backup_mode)
	{
		backup->backup_mode = parse_backup_mode(backup_mode);
		free(backup_mode);
	}

	if (start_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(start_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->start_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid START_LSN \"%s\"", start_lsn);
		free(start_lsn);
	}

	if (stop_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(stop_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->stop_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "Invalid STOP_LSN \"%s\"", stop_lsn);
		free(stop_lsn);
	}

	if (status)
	{
		if (strcmp(status, "OK") == 0)
			backup->status = BACKUP_STATUS_OK;
		else if (strcmp(status, "ERROR") == 0)
			backup->status = BACKUP_STATUS_ERROR;
		else if (strcmp(status, "RUNNING") == 0)
			backup->status = BACKUP_STATUS_RUNNING;
		else if (strcmp(status, "MERGING") == 0)
			backup->status = BACKUP_STATUS_MERGING;
		else if (strcmp(status, "DELETING") == 0)
			backup->status = BACKUP_STATUS_DELETING;
		else if (strcmp(status, "DELETED") == 0)
			backup->status = BACKUP_STATUS_DELETED;
		else if (strcmp(status, "DONE") == 0)
			backup->status = BACKUP_STATUS_DONE;
		else if (strcmp(status, "ORPHAN") == 0)
			backup->status = BACKUP_STATUS_ORPHAN;
		else if (strcmp(status, "CORRUPT") == 0)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			elog(WARNING, "Invalid STATUS \"%s\"", status);
		free(status);
	}

	if (parent_backup)
	{
		backup->parent_backup = base36dec(parent_backup);
		free(parent_backup);
	}

	if (program_version)
	{
		StrNCpy(backup->program_version, program_version,
				sizeof(backup->program_version));
		pfree(program_version);
	}

	if (server_version)
	{
		StrNCpy(backup->server_version, server_version,
				sizeof(backup->server_version));
		pfree(server_version);
	}

	if (compress_alg)
		backup->compress_alg = parse_compress_alg(compress_alg);

	return backup;
}

BackupMode
parse_backup_mode(const char *value)
{
	const char *v = value;
	size_t		len;

	/* Skip all spaces detected */
	while (IsSpace(*v))
		v++;
	len = strlen(v);

	if (len > 0 && pg_strncasecmp("full", v, len) == 0)
		return BACKUP_MODE_FULL;
	else if (len > 0 && pg_strncasecmp("page", v, len) == 0)
		return BACKUP_MODE_DIFF_PAGE;
	else if (len > 0 && pg_strncasecmp("ptrack", v, len) == 0)
		return BACKUP_MODE_DIFF_PTRACK;
	else if (len > 0 && pg_strncasecmp("delta", v, len) == 0)
		return BACKUP_MODE_DIFF_DELTA;

	/* Backup mode is invalid, so leave with an error */
	elog(ERROR, "invalid backup-mode \"%s\"", value);
	return BACKUP_MODE_INVALID;
}

const char *
deparse_backup_mode(BackupMode mode)
{
	switch (mode)
	{
		case BACKUP_MODE_FULL:
			return "full";
		case BACKUP_MODE_DIFF_PAGE:
			return "page";
		case BACKUP_MODE_DIFF_PTRACK:
			return "ptrack";
		case BACKUP_MODE_DIFF_DELTA:
			return "delta";
		case BACKUP_MODE_INVALID:
			return "invalid";
	}

	return NULL;
}

CompressAlg
parse_compress_alg(const char *arg)
{
	size_t		len;

	/* Skip all spaces detected */
	while (isspace((unsigned char)*arg))
		arg++;
	len = strlen(arg);

	if (len == 0)
		elog(ERROR, "compress algorithm is empty");

	if (pg_strncasecmp("zlib", arg, len) == 0)
		return ZLIB_COMPRESS;
	else if (pg_strncasecmp("pglz", arg, len) == 0)
		return PGLZ_COMPRESS;
	else if (pg_strncasecmp("none", arg, len) == 0)
		return NONE_COMPRESS;
	else
		elog(ERROR, "invalid compress algorithm value \"%s\"", arg);

	return NOT_DEFINED_COMPRESS;
}

const char*
deparse_compress_alg(int alg)
{
	switch (alg)
	{
		case NONE_COMPRESS:
		case NOT_DEFINED_COMPRESS:
			return "none";
		case ZLIB_COMPRESS:
			return "zlib";
		case PGLZ_COMPRESS:
			return "pglz";
	}

	return NULL;
}

/*
 * Fill PGNodeInfo struct with default values.
 */
void
pgNodeInit(PGNodeInfo *node)
{
	node->block_size = 0;
	node->wal_block_size = 0;
	node->checksum_version = 0;

	node->is_superuser = false;

	node->server_version = 0;
	node->server_version_str[0] = '\0';
}

/*
 * Fill pgBackup struct with default values.
 */
void
pgBackupInit(pgBackup *backup)
{
	backup->backup_id = INVALID_BACKUP_ID;
	backup->backup_mode = BACKUP_MODE_INVALID;
	backup->status = BACKUP_STATUS_INVALID;
	backup->tli = 0;
	backup->start_lsn = 0;
	backup->stop_lsn = 0;
	backup->start_time = (time_t) 0;
	backup->merge_time = (time_t) 0;
	backup->end_time = (time_t) 0;
	backup->recovery_xid = 0;
	backup->recovery_time = (time_t) 0;

	backup->data_bytes = BYTES_INVALID;
	backup->wal_bytes = BYTES_INVALID;
	backup->uncompressed_bytes = 0;
	backup->pgdata_bytes = 0;

	backup->compress_alg = COMPRESS_ALG_DEFAULT;
	backup->compress_level = COMPRESS_LEVEL_DEFAULT;

	backup->block_size = BLCKSZ;
	backup->wal_block_size = XLOG_BLCKSZ;
	backup->checksum_version = 0;

	backup->stream = false;
	backup->from_replica = false;
	backup->parent_backup = INVALID_BACKUP_ID;
	backup->parent_backup_link = NULL;
	backup->primary_conninfo = NULL;
	backup->program_version[0] = '\0';
	backup->server_version[0] = '\0';
	backup->external_dir_str = NULL;
}

/* free pgBackup object */
void
pgBackupFree(void *backup)
{
	pgBackup *b = (pgBackup *) backup;

	pfree(b->primary_conninfo);
	pfree(b->external_dir_str);
	pfree(backup);
}

/* Compare two pgBackup with their IDs (start time) in ascending order */
int
pgBackupCompareId(const void *l, const void *r)
{
	pgBackup *lp = *(pgBackup **)l;
	pgBackup *rp = *(pgBackup **)r;

	if (lp->start_time > rp->start_time)
		return 1;
	else if (lp->start_time < rp->start_time)
		return -1;
	else
		return 0;
}

/* Compare two pgBackup with their IDs in descending order */
int
pgBackupCompareIdDesc(const void *l, const void *r)
{
	return -pgBackupCompareId(l, r);
}

/*
 * Construct absolute path of the backup directory.
 * If subdir is not NULL, it will be appended after the path.
 */
void
pgBackupGetPath(const pgBackup *backup, char *path, size_t len, const char *subdir)
{
	pgBackupGetPath2(backup, path, len, subdir, NULL);
}

/*
 * Construct absolute path of the backup directory.
 * Append "subdir1" and "subdir2" to the backup directory.
 */
void
pgBackupGetPath2(const pgBackup *backup, char *path, size_t len,
				 const char *subdir1, const char *subdir2)
{
	/* If "subdir1" is NULL do not check "subdir2" */
	if (!subdir1)
		snprintf(path, len, "%s/%s", backup_instance_path,
				 base36enc(backup->start_time));
	else if (!subdir2)
		snprintf(path, len, "%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1);
	/* "subdir1" and "subdir2" is not NULL */
	else
		snprintf(path, len, "%s/%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1, subdir2);
}

/*
 * independent from global variable backup_instance_path
 * Still depends from backup_path
 */
void
pgBackupGetPathInInstance(const char *instance_name,
				 const pgBackup *backup, char *path, size_t len,
				 const char *subdir1, const char *subdir2)
{
	char		backup_instance_path[MAXPGPATH];

	sprintf(backup_instance_path, "%s/%s/%s",
				backup_path, BACKUPS_DIR, instance_name);

	/* If "subdir1" is NULL do not check "subdir2" */
	if (!subdir1)
		snprintf(path, len, "%s/%s", backup_instance_path,
				 base36enc(backup->start_time));
	else if (!subdir2)
		snprintf(path, len, "%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1);
	/* "subdir1" and "subdir2" is not NULL */
	else
		snprintf(path, len, "%s/%s/%s/%s", backup_instance_path,
				 base36enc(backup->start_time), subdir1, subdir2);
}

/*
 * Check if multiple backups consider target backup to be their direct parent
 */
bool
is_prolific(parray *backup_list, pgBackup *target_backup)
{
	int i;
	int child_counter = 0;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		/* consider only OK and DONE backups */
		if (tmp_backup->parent_backup == target_backup->start_time &&
			(tmp_backup->status == BACKUP_STATUS_OK ||
			 tmp_backup->status == BACKUP_STATUS_DONE))
		{
			child_counter++;
			if (child_counter > 1)
				return true;
		}
	}

	return false;
}

/*
 * Find parent base FULL backup for current backup using parent_backup_link
 */
pgBackup*
find_parent_full_backup(pgBackup *current_backup)
{
	pgBackup   *base_full_backup = NULL;
	base_full_backup = current_backup;

	/* sanity */
	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	while (base_full_backup->parent_backup_link != NULL)
	{
		base_full_backup = base_full_backup->parent_backup_link;
	}

	if (base_full_backup->backup_mode != BACKUP_MODE_FULL)
	{
		if (base_full_backup->parent_backup)
			elog(WARNING, "Backup %s is missing",
				 base36enc(base_full_backup->parent_backup));
		else
			elog(WARNING, "Failed to find parent FULL backup for %s",
				 base36enc(current_backup->start_time));
		return NULL;
	}

	return base_full_backup;
}

/*
 * Iterate over parent chain and look for any problems.
 * Return 0 if chain is broken.
 *  result_backup must contain oldest existing backup after missing backup.
 *  we have no way to know if there are multiple missing backups.
 * Return 1 if chain is intact, but at least one backup is !OK.
 *  result_backup must contain oldest !OK backup.
 * Return 2 if chain is intact and all backups are OK.
 *	result_backup must contain FULL backup on which chain is based.
 */
int
scan_parent_chain(pgBackup *current_backup, pgBackup **result_backup)
{
	pgBackup   *target_backup = NULL;
	pgBackup   *invalid_backup = NULL;

	if (!current_backup)
		elog(ERROR, "Target backup cannot be NULL");

	target_backup = current_backup;

	while (target_backup->parent_backup_link)
	{
		if (target_backup->status != BACKUP_STATUS_OK &&
			  target_backup->status != BACKUP_STATUS_DONE)
			/* oldest invalid backup in parent chain */
			invalid_backup = target_backup;


		target_backup = target_backup->parent_backup_link;
	}

	/* Previous loop will skip FULL backup because his parent_backup_link is NULL */
	if (target_backup->backup_mode == BACKUP_MODE_FULL &&
		(target_backup->status != BACKUP_STATUS_OK &&
		target_backup->status != BACKUP_STATUS_DONE))
	{
		invalid_backup = target_backup;
	}

	/* found chain end and oldest backup is not FULL */
	if (target_backup->backup_mode != BACKUP_MODE_FULL)
	{
		/* Set oldest child backup in chain */
		*result_backup = target_backup;
		return 0;
	}

	/* chain is ok, but some backups are invalid */
	if (invalid_backup)
	{
		*result_backup = invalid_backup;
		return 1;
	}

	*result_backup = target_backup;
	return 2;
}

/*
 * Determine if child_backup descend from parent_backup
 * This check DO NOT(!!!) guarantee that parent chain is intact,
 * because parent_backup can be missing.
 * If inclusive is true, then child_backup counts as a child of himself
 * if parent_backup_time is start_time of child_backup.
 */
bool
is_parent(time_t parent_backup_time, pgBackup *child_backup, bool inclusive)
{
	if (!child_backup)
		elog(ERROR, "Target backup cannot be NULL");

	if (inclusive && child_backup->start_time == parent_backup_time)
		return true;

	while (child_backup->parent_backup_link &&
			child_backup->parent_backup != parent_backup_time)
	{
		child_backup = child_backup->parent_backup_link;
	}

	if (child_backup->parent_backup == parent_backup_time)
		return true;

	//if (inclusive && child_backup->start_time == parent_backup_time)
	//	return true;

	return false;
}

/*
 * Return backup index number.
 * Note: this index number holds true until new sorting of backup list
 */
int
get_backup_index_number(parray *backup_list, pgBackup *backup)
{
	int i;

	for (i = 0; i < parray_num(backup_list); i++)
	{
		pgBackup   *tmp_backup = (pgBackup *) parray_get(backup_list, i);

		if (tmp_backup->start_time == backup->start_time)
			return i;
	}
	elog(WARNING, "Failed to find backup %s", base36enc(backup->start_time));
	return -1;
}
