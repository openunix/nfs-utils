/*
 * Copyright (C) 2011  Red Hat, Jeff Layton <jlayton@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Explanation:
 *
 * This file contains the code to manage the sqlite backend database for the
 * nfsdcld client tracking daemon.
 *
 * The main database is called main.sqlite and contains the following tables:
 *
 * parameters: simple key/value pairs for storing database info
 *
 * grace: a "current" column containing an INTEGER representing the current
 *        epoch (where should new values be stored) and a "recovery" column
 *        containing an INTEGER representing the recovery epoch (from what
 *        epoch are we allowed to recover).  A recovery epoch of 0 means
 *        normal operation (grace period not in force).  Note: sqlite stores
 *        integers as signed values, so these must be cast to a uint64_t when
 *        retrieving them from the database and back to an int64_t when storing
 *        them in the database.
 *
 * rec-CCCCCCCCCCCCCCCC (where C is the hex representation of the epoch value):
 *        a single "id" column containing a BLOB with the long-form clientid
 *        as sent by the client.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <dirent.h>
#include <errno.h>
#include <event.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <sqlite3.h>
#include <linux/limits.h>

#include "xlog.h"
#include "sqlite.h"
#include "cld.h"
#include "cld-internal.h"

#define CLD_SQLITE_LATEST_SCHEMA_VERSION 3

/* in milliseconds */
#define CLD_SQLITE_BUSY_TIMEOUT 10000

/* private data structures */

/* global variables */

/* reusable pathname and sql command buffer */
static char buf[PATH_MAX];

/* global database handle */
static sqlite3 *dbh;

/* forward declarations */

/* make a directory, ignoring EEXIST errors unless it's not a directory */
static int
mkdir_if_not_exist(const char *dirname)
{
	int ret;
	struct stat statbuf;

	ret = mkdir(dirname, S_IRWXU);
	if (ret && errno != EEXIST)
		return -errno;

	ret = stat(dirname, &statbuf);
	if (ret)
		return -errno;

	if (!S_ISDIR(statbuf.st_mode))
		ret = -ENOTDIR;

	return ret;
}

static int
sqlite_query_schema_version(void)
{
	int ret;
	sqlite3_stmt *stmt = NULL;

	/* prepare select query */
	ret = sqlite3_prepare_v2(dbh,
		"SELECT value FROM parameters WHERE key == \"version\";",
		 -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		xlog(D_GENERAL, "Unable to prepare select statement: %s",
			sqlite3_errmsg(dbh));
		ret = 0;
		goto out;
	}

	/* query schema version */
	ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		xlog(D_GENERAL, "Select statement execution failed: %s",
				sqlite3_errmsg(dbh));
		ret = 0;
		goto out;
	}

	ret = sqlite3_column_int(stmt, 0);
out:
	sqlite3_finalize(stmt);
	return ret;
}

static int
sqlite_maindb_update_schema(int oldversion)
{
	int ret, ret2;
	char *err;

	/* begin transaction */
	ret = sqlite3_exec(dbh, "BEGIN EXCLUSIVE TRANSACTION;", NULL, NULL,
				&err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to begin transaction: %s", err);
		goto rollback;
	}

	/*
	 * Check schema version again. This time, under an exclusive
	 * transaction to guard against racing DB setup attempts
	 */
	ret = sqlite_query_schema_version();
	if (ret != oldversion) {
		if (ret == CLD_SQLITE_LATEST_SCHEMA_VERSION)
			/* Someone else raced in and set it up */
			ret = 0;
		else
			/* Something went wrong -- fail! */
			ret = -EINVAL;
		goto rollback;
	}

	/* Still at old version -- do conversion */

	/* create grace table */
	ret = sqlite3_exec(dbh, "CREATE TABLE grace "
				"(current INTEGER , recovery INTEGER);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to create grace table: %s", err);
		goto rollback;
	}

	/* insert initial epochs into grace table */
	ret = sqlite3_exec(dbh, "INSERT OR FAIL INTO grace "
				"values (1, 0);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to set initial epochs: %s", err);
		goto rollback;
	}

	/* create recovery table for current epoch */
	ret = sqlite3_exec(dbh, "CREATE TABLE \"rec-0000000000000001\" "
				"(id BLOB PRIMARY KEY);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to create recovery table "
				"for current epoch: %s", err);
		goto rollback;
	}

	/* copy records from old clients table */
	ret = sqlite3_exec(dbh, "INSERT INTO \"rec-0000000000000001\" "
				"SELECT id FROM clients;",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to copy client records: %s", err);
		goto rollback;
	}

	/* drop the old clients table */
	ret = sqlite3_exec(dbh, "DROP TABLE clients;",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to drop old clients table: %s", err);
		goto rollback;
	}

	ret = snprintf(buf, sizeof(buf), "UPDATE parameters SET value = %d "
			"WHERE key = \"version\";",
			CLD_SQLITE_LATEST_SCHEMA_VERSION);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		goto rollback;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		ret = -EINVAL;
		goto rollback;
	}

	ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to update schema version: %s", err);
		goto rollback;
	}

	ret = sqlite3_exec(dbh, "COMMIT TRANSACTION;", NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to commit transaction: %s", err);
		goto rollback;
	}
out:
	sqlite3_free(err);
	return ret;
rollback:
	ret2 = sqlite3_exec(dbh, "ROLLBACK TRANSACTION;", NULL, NULL, &err);
	if (ret2 != SQLITE_OK)
		xlog(L_ERROR, "Unable to rollback transaction: %s", err);
	goto out;
}

/*
 * Start an exclusive transaction and recheck the DB schema version. If it's
 * still zero (indicating a new database) then set it up. If that all works,
 * then insert schema version into the parameters table and commit the
 * transaction. On any error, rollback the transaction.
 */
static int
sqlite_maindb_init_v3(void)
{
	int ret, ret2;
	char *err = NULL;

	/* Start a transaction */
	ret = sqlite3_exec(dbh, "BEGIN EXCLUSIVE TRANSACTION;", NULL, NULL,
				&err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to begin transaction: %s", err);
		return ret;
	}

	/*
	 * Check schema version again. This time, under an exclusive
	 * transaction to guard against racing DB setup attempts
	 */
	ret = sqlite_query_schema_version();
	switch (ret) {
	case 0:
		/* Query failed again -- set up DB */
		break;
	case CLD_SQLITE_LATEST_SCHEMA_VERSION:
		/* Someone else raced in and set it up */
		ret = 0;
		goto rollback;
	default:
		/* Something went wrong -- fail! */
		ret = -EINVAL;
		goto rollback;
	}

	ret = sqlite3_exec(dbh, "CREATE TABLE parameters "
				"(key TEXT PRIMARY KEY, value TEXT);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to create parameter table: %s", err);
		goto rollback;
	}

	/* create grace table */
	ret = sqlite3_exec(dbh, "CREATE TABLE grace "
				"(current INTEGER , recovery INTEGER);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to create grace table: %s", err);
		goto rollback;
	}

	/* insert initial epochs into grace table */
	ret = sqlite3_exec(dbh, "INSERT OR FAIL INTO grace "
				"values (1, 0);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to set initial epochs: %s", err);
		goto rollback;
	}

	/* create recovery table for current epoch */
	ret = sqlite3_exec(dbh, "CREATE TABLE \"rec-0000000000000001\" "
				"(id BLOB PRIMARY KEY);",
				NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to create recovery table "
				"for current epoch: %s", err);
		goto rollback;
	}

	/* insert version into parameters table */
	ret = snprintf(buf, sizeof(buf), "INSERT OR FAIL INTO parameters "
			"values (\"version\", \"%d\");",
			CLD_SQLITE_LATEST_SCHEMA_VERSION);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		goto rollback;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		ret = -EINVAL;
		goto rollback;
	}

	ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to insert into parameter table: %s", err);
		goto rollback;
	}

	ret = sqlite3_exec(dbh, "COMMIT TRANSACTION;", NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to commit transaction: %s", err);
		goto rollback;
	}
out:
	sqlite3_free(err);
	return ret;

rollback:
	/* Attempt to rollback the transaction */
	ret2 = sqlite3_exec(dbh, "ROLLBACK TRANSACTION;", NULL, NULL, &err);
	if (ret2 != SQLITE_OK)
		xlog(L_ERROR, "Unable to rollback transaction: %s", err);
	goto out;
}

static int
sqlite_startup_query_grace(void)
{
	int ret;
	uint64_t tcur;
	uint64_t trec;
	sqlite3_stmt *stmt = NULL;

	/* prepare select query */
	ret = sqlite3_prepare_v2(dbh, "SELECT * FROM grace;", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		xlog(D_GENERAL, "Unable to prepare select statement: %s",
			sqlite3_errmsg(dbh));
		goto out;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		xlog(D_GENERAL, "Select statement execution failed: %s",
				sqlite3_errmsg(dbh));
		goto out;
	}

	tcur = (uint64_t)sqlite3_column_int64(stmt, 0);
	trec = (uint64_t)sqlite3_column_int64(stmt, 1);

	current_epoch = tcur;
	recovery_epoch = trec;
	ret = 0;
	xlog(D_GENERAL, "%s: current_epoch=%lu recovery_epoch=%lu",
		__func__, current_epoch, recovery_epoch);
out:
	sqlite3_finalize(stmt);
	return ret;
}

/* Open the database and set up the database handle for it */
int
sqlite_prepare_dbh(const char *topdir)
{
	int ret;

	/* Do nothing if the database handle is already set up */
	if (dbh)
		return 0;

	ret = snprintf(buf, PATH_MAX - 1, "%s/main.sqlite", topdir);
	if (ret < 0)
		return ret;

	buf[PATH_MAX - 1] = '\0';

	/* open a new DB handle */
	ret = sqlite3_open(buf, &dbh);
	if (ret != SQLITE_OK) {
		/* try to create the dir */
		ret = mkdir_if_not_exist(topdir);
		if (ret)
			goto out_close;

		/* retry open */
		ret = sqlite3_open(buf, &dbh);
		if (ret != SQLITE_OK)
			goto out_close;
	}

	/* set busy timeout */
	ret = sqlite3_busy_timeout(dbh, CLD_SQLITE_BUSY_TIMEOUT);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to set sqlite busy timeout: %s",
				sqlite3_errmsg(dbh));
		goto out_close;
	}

	ret = sqlite_query_schema_version();
	switch (ret) {
	case CLD_SQLITE_LATEST_SCHEMA_VERSION:
		/* DB is already set up. Do nothing */
		ret = 0;
		break;
	case 2:
		/* Old DB -- update to new schema */
		ret = sqlite_maindb_update_schema(2);
		if (ret)
			goto out_close;
		break;

	case 1:
		/* Old DB -- update to new schema */
		ret = sqlite_maindb_update_schema(1);
		if (ret)
			goto out_close;
		break;
	case 0:
		/* Query failed -- try to set up new DB */
		ret = sqlite_maindb_init_v3();
		if (ret)
			goto out_close;
		break;
	default:
		/* Unknown DB version -- downgrade? Fail */
		xlog(L_ERROR, "Unsupported database schema version! "
			"Expected %d, got %d.",
			CLD_SQLITE_LATEST_SCHEMA_VERSION, ret);
		ret = -EINVAL;
		goto out_close;
	}

	ret = sqlite_startup_query_grace();

	return ret;
out_close:
	sqlite3_close(dbh);
	dbh = NULL;
	return ret;
}

/*
 * Create a client record
 *
 * Returns a non-zero sqlite error code, or SQLITE_OK (aka 0)
 */
int
sqlite_insert_client(const unsigned char *clname, const size_t namelen)
{
	int ret;
	sqlite3_stmt *stmt = NULL;

	ret = snprintf(buf, sizeof(buf), "INSERT OR REPLACE INTO \"rec-%016lx\" "
				"VALUES (?);", current_epoch);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		return ret;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(dbh, buf, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: insert statement prepare failed: %s",
			__func__, sqlite3_errmsg(dbh));
		return ret;
	}

	ret = sqlite3_bind_blob(stmt, 1, (const void *)clname, namelen,
				SQLITE_STATIC);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: bind blob failed: %s", __func__,
				sqlite3_errmsg(dbh));
		goto out_err;
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_DONE)
		ret = SQLITE_OK;
	else
		xlog(L_ERROR, "%s: unexpected return code from insert: %s",
				__func__, sqlite3_errmsg(dbh));

out_err:
	xlog(D_GENERAL, "%s: returning %d", __func__, ret);
	sqlite3_finalize(stmt);
	return ret;
}

/* Remove a client record */
int
sqlite_remove_client(const unsigned char *clname, const size_t namelen)
{
	int ret;
	sqlite3_stmt *stmt = NULL;

	ret = snprintf(buf, sizeof(buf), "DELETE FROM \"rec-%016lx\" "
				"WHERE id==?;", current_epoch);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		return ret;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(dbh, buf, -1, &stmt, NULL);

	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: statement prepare failed: %s",
				__func__, sqlite3_errmsg(dbh));
		goto out_err;
	}

	ret = sqlite3_bind_blob(stmt, 1, (const void *)clname, namelen,
				SQLITE_STATIC);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: bind blob failed: %s", __func__,
				sqlite3_errmsg(dbh));
		goto out_err;
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_DONE)
		ret = SQLITE_OK;
	else
		xlog(L_ERROR, "%s: unexpected return code from delete: %d",
				__func__, ret);

out_err:
	xlog(D_GENERAL, "%s: returning %d", __func__, ret);
	sqlite3_finalize(stmt);
	return ret;
}

/*
 * Is the given clname in the clients table? If so, then update its timestamp
 * and return success. If the record isn't present, or the update fails, then
 * return an error.
 */
int
sqlite_check_client(const unsigned char *clname, const size_t namelen)
{
	int ret;
	sqlite3_stmt *stmt = NULL;

	ret = snprintf(buf, sizeof(buf), "SELECT count(*) FROM  \"rec-%016lx\" "
				"WHERE id==?;", recovery_epoch);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		return ret;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(dbh, buf, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: select statement prepare failed: %s",
			__func__, sqlite3_errmsg(dbh));
		return ret;
	}

	ret = sqlite3_bind_blob(stmt, 1, (const void *)clname, namelen,
				SQLITE_STATIC);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: bind blob failed: %s",
				__func__, sqlite3_errmsg(dbh));
		goto out_err;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		xlog(L_ERROR, "%s: unexpected return code from select: %d",
				__func__, ret);
		goto out_err;
	}

	ret = sqlite3_column_int(stmt, 0);
	xlog(D_GENERAL, "%s: select returned %d rows", __func__, ret);
	if (ret != 1) {
		ret = -EACCES;
		goto out_err;
	}

	sqlite3_finalize(stmt);

	/* Now insert the client into the table for the current epoch */
	return sqlite_insert_client(clname, namelen);

out_err:
	xlog(D_GENERAL, "%s: returning %d", __func__, ret);
	sqlite3_finalize(stmt);
	return ret;
}

int
sqlite_grace_start(void)
{
	int ret, ret2;
	char *err;
	uint64_t tcur = current_epoch;
	uint64_t trec = recovery_epoch;

	/* begin transaction */
	ret = sqlite3_exec(dbh, "BEGIN EXCLUSIVE TRANSACTION;", NULL, NULL,
				&err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to begin transaction: %s", err);
		goto rollback;
	}

	if (trec == 0) {
		/*
		 * A normal grace start - update the epoch values in the grace
		 * table and create a new table for the current reboot epoch.
		 */
		trec = tcur;
		tcur++;

		ret = snprintf(buf, sizeof(buf), "UPDATE grace "
				"SET current = %ld, recovery = %ld;",
				(int64_t)tcur, (int64_t)trec);
		if (ret < 0) {
			xlog(L_ERROR, "sprintf failed!");
			goto rollback;
		} else if ((size_t)ret >= sizeof(buf)) {
			xlog(L_ERROR, "sprintf output too long! (%d chars)",
				ret);
			ret = -EINVAL;
			goto rollback;
		}

		ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			xlog(L_ERROR, "Unable to update epochs: %s", err);
			goto rollback;
		}

		ret = snprintf(buf, sizeof(buf), "CREATE TABLE \"rec-%016lx\" "
				"(id BLOB PRIMARY KEY);",
				tcur);
		if (ret < 0) {
			xlog(L_ERROR, "sprintf failed!");
			goto rollback;
		} else if ((size_t)ret >= sizeof(buf)) {
			xlog(L_ERROR, "sprintf output too long! (%d chars)",
				ret);
			ret = -EINVAL;
			goto rollback;
		}

		ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			xlog(L_ERROR, "Unable to create table for current epoch: %s",
				err);
			goto rollback;
		}
	} else {
		/* Server restarted while in grace - don't update the epoch
		 * values in the grace table, just clear out the records for
		 * the current reboot epoch.
		 */
		ret = snprintf(buf, sizeof(buf), "DELETE FROM \"rec-%016lx\";",
				tcur);
		if (ret < 0) {
			xlog(L_ERROR, "sprintf failed!");
			goto rollback;
		} else if ((size_t)ret >= sizeof(buf)) {
			xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
			ret = -EINVAL;
			goto rollback;
		}

		ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			xlog(L_ERROR, "Unable to clear table for current epoch: %s",
				err);
			goto rollback;
		}
	}

	ret = sqlite3_exec(dbh, "COMMIT TRANSACTION;", NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to commit transaction: %s", err);
		goto rollback;
	}

	current_epoch = tcur;
	recovery_epoch = trec;
	xlog(D_GENERAL, "%s: current_epoch=%lu recovery_epoch=%lu",
		__func__, current_epoch, recovery_epoch);

out:
	sqlite3_free(err);
	return ret;
rollback:
	ret2 = sqlite3_exec(dbh, "ROLLBACK TRANSACTION;", NULL, NULL, &err);
	if (ret2 != SQLITE_OK)
		xlog(L_ERROR, "Unable to rollback transaction: %s", err);
	goto out;
}

int
sqlite_grace_done(void)
{
	int ret, ret2;
	char *err;

	/* begin transaction */
	ret = sqlite3_exec(dbh, "BEGIN EXCLUSIVE TRANSACTION;", NULL, NULL,
				&err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to begin transaction: %s", err);
		goto rollback;
	}

	ret = sqlite3_exec(dbh, "UPDATE grace SET recovery = \"0\";",
			NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to clear recovery epoch: %s", err);
		goto rollback;
	}

	ret = snprintf(buf, sizeof(buf), "DROP TABLE \"rec-%016lx\";",
		recovery_epoch);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		goto rollback;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		ret = -EINVAL;
		goto rollback;
	}

	ret = sqlite3_exec(dbh, (const char *)buf, NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to drop table for recovery epoch: %s",
			err);
		goto rollback;
	}

	ret = sqlite3_exec(dbh, "COMMIT TRANSACTION;", NULL, NULL, &err);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "Unable to commit transaction: %s", err);
		goto rollback;
	}

	recovery_epoch = 0;
	xlog(D_GENERAL, "%s: current_epoch=%lu recovery_epoch=%lu",
		__func__, current_epoch, recovery_epoch);

out:
	sqlite3_free(err);
	return ret;
rollback:
	ret2 = sqlite3_exec(dbh, "ROLLBACK TRANSACTION;", NULL, NULL, &err);
	if (ret2 != SQLITE_OK)
		xlog(L_ERROR, "Unable to rollback transaction: %s", err);
	goto out;
}


int
sqlite_iterate_recovery(int (*cb)(struct cld_client *clnt), struct cld_client *clnt)
{
	int ret;
	sqlite3_stmt *stmt = NULL;
	struct cld_msg *cmsg = &clnt->cl_msg;

	if (recovery_epoch == 0) {
		xlog(D_GENERAL, "%s: not in grace!", __func__);
		return -EINVAL;
	}

	ret = snprintf(buf, sizeof(buf), "SELECT * FROM \"rec-%016lx\";",
		recovery_epoch);
	if (ret < 0) {
		xlog(L_ERROR, "sprintf failed!");
		return ret;
	} else if ((size_t)ret >= sizeof(buf)) {
		xlog(L_ERROR, "sprintf output too long! (%d chars)", ret);
		return -EINVAL;
	}

	ret = sqlite3_prepare_v2(dbh, buf, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		xlog(L_ERROR, "%s: select statement prepare failed: %s",
			__func__, sqlite3_errmsg(dbh));
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		memcpy(&cmsg->cm_u.cm_name.cn_id, sqlite3_column_blob(stmt, 0),
			NFS4_OPAQUE_LIMIT);
		cmsg->cm_u.cm_name.cn_len = sqlite3_column_bytes(stmt, 0);
		cb(clnt);
	}
	if (ret == SQLITE_DONE)
		ret = 0;
	sqlite3_finalize(stmt);
	return ret;
}
