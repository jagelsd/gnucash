/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
\********************************************************************/

/*
 * FILE:
 * gnc-book.c
 *
 * FUNCTION:
 * Encapsulate all the information about a gnucash dataset, including
 * the methods used to read and write them to datastores.
 *
 * HISTORY:
 * Created by Linas Vepstas December 1998
 * Copyright (c) 1998-2000 Linas Vepstas
 * Copyright (c) 2000 Dave Peticolas
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "BackendP.h"
#include "FileIO.h"
#include "Group.h"
#include "gnc-book.h"
#include "gnc-engine-util.h"


struct _gnc_book
{
  AccountGroup *topgroup;

  /* the requested book id, in the form or a URI, such as
   * file:/some/where, or sql:server.host.com:555
   */
  char *book_id;

  /* if book_begin failed, this records the failure reason 
   * (file not found, etc).
   * the standard errno values are used.
   */
  int errtype;

  /* FIXME: This is a hack.  I'm trying to move us away from static
   * global vars. This may be a temp fix if we decide to integrate
   * FileIO errors into GNCBook errors. */
  GNCFileIOError last_file_err;

  /* ---------------------------------------------------- */
  /* the following struct members apply only for file-io */
  /* the fully-resolved path to the file */
  char *fullpath;

  /* name of lockfile, and filedescr */
  char * lockfile;
  char * linkfile;
  int lockfd;

  /* ---------------------------------------------------- */
  /* this struct member applies only for SQL i/o */
  Backend *backend;
};

/* ============================================================== */

static void
gnc_book_init (GNCBook *book)
{
  if (!book) return;

  book->topgroup = xaccMallocAccountGroup ();
  book->last_file_err = ERR_FILEIO_NONE;
  book->lockfd = -1;
};

GNCBook *
gnc_book_new (void)
{
  GNCBook *book;

  book = g_new0(GNCBook, 1);

  gnc_book_init (book);

  return book;
}

/* ============================================================== */

int
gnc_book_get_error (GNCBook * book)
{
  int retval;

  if (!book) return EINVAL;

  retval = book->errtype;
  book->errtype = 0;

  return retval;
}

/* ============================================================== */

GNCFileIOError
gnc_book_get_file_error (GNCBook * book)
{
  if (!book) return ERR_FILEIO_MISC;

  return book->last_file_err;
}

/* ============================================================== */

AccountGroup * 
gnc_book_get_group (GNCBook *book)
{
   if (!book) return NULL;

   return book->topgroup;
}

void
gnc_book_set_group (GNCBook *book, AccountGroup *grp)
{
   if (!book) return;

   book->topgroup = grp;
}

/* ============================================================== */

Backend * 
xaccGNCBookGetBackend (GNCBook *book)
{
   if (!book) return NULL;

   return book->backend;
}

/* ============================================================== */

const char *
gnc_book_get_file_path (GNCBook *book)
{
   if (!book) return NULL;

   return book->fullpath;
}

/* ============================================================== */

gboolean
gnc_book_begin (GNCBook *book, const char * book_id)
{
  if (!book) return FALSE;

  /* clear the error condition of previous errors */
  book->errtype = 0;
  book->last_file_err = ERR_FILEIO_NONE;

  /* check to see if this session is already open */
  if (book->book_id)
  {
    book->errtype = ETXTBSY;
    return FALSE;
  }

  /* seriously invalid */
  if (!book_id)
  {
    book->errtype = EINVAL;
    return FALSE;
  }

  /* check to see if this is a type we know how to handle */
  if (strncmp(book_id, "file:", 5) != 0)
  {
    book->errtype = ENOSYS;
    return FALSE;
  }

  /* add 5 to space past 'file:' */
  return gnc_book_begin_file (book, book_id + 5, NULL);
}

/* ============================================================== */

#if 0
static AccountGroup *
xaccSessionBeginSQL (Session *sess, const char * dbname)
{
   Backend *be = NULL;
   AccountGroup *grp = NULL;

   if (!sess) return NULL;

/* #define SQLHACK */
#ifdef SQLHACK
   {
     /* for testing the sql, just a hack, remove later ... */
extern Backend * pgendNew (void);
     be = pgendNew ();
   }
#endif

   sess->backend = be;

   if (be && be->session_begin) {
      grp = (be->session_begin) (sess, dbname);
   }
   /* comment out until testing done, else clobber file ...*/
   /* sess->topgroup = grp; */
   xaccGroupSetBackend (sess->topgroup, be);

   return (sess->topgroup);
}
#endif

/* ============================================================== */

static gboolean
gnc_book_get_file_lock (GNCBook *book)
{
  struct stat statbuf;
  char pathbuf[PATH_MAX];
  char *path = NULL;
  int rc;

  rc = stat (book->lockfile, &statbuf);
  if (!rc)
  {
    /* oops .. file is all locked up  .. */
    book->errtype = ETXTBSY;  
    return FALSE;
  }

  book->lockfd = open (book->lockfile, O_RDWR | O_CREAT | O_EXCL , 0);
  if (book->lockfd < 0)
  {
    /* oops .. file is all locked up  .. */
    book->errtype = ETXTBSY;
    return FALSE;
  }

  /* OK, now work around some NFS atomic lock race condition 
   * mumbo-jumbo.  We do this by linking a unique file, and 
   * then examing the link count.  At least that's what the 
   * NFS programmers guide suggests. 
   * Note: the "unique filename" must be unique for the
   * triplet filename-host-process, otherwise accidental 
   * aliases can occur.
   */

  /* apparently, even this code may not work for some NFS
   * implementations. In the long run, I am told that 
   * ftp.debian.org
   *  /pub/debian/dists/unstable/main/source/libs/liblockfile_0.1-6.tar.gz
   * provides a better long-term solution.
   */

  strcpy (pathbuf, book->lockfile);
  path = strrchr (pathbuf, '.');
  sprintf (path, ".%lx.%d.LNK", gethostid(), getpid());
  link (book->lockfile, pathbuf);
  rc = stat (book->lockfile, &statbuf);
  if (rc)
  {
    /* oops .. stat failed!  This can't happen! */
    book->errtype = ETXTBSY;
    unlink (pathbuf);
    close (book->lockfd);
    unlink (book->lockfile);
    return FALSE;
  }

  if (statbuf.st_nlink != 2)
  {
    /* oops .. stat failed!  This can't happen! */
    book->errtype = ETXTBSY;
    unlink (pathbuf);
    close (book->lockfd);
    unlink (book->lockfile);
    return FALSE;
  }

  book->linkfile = g_strdup (pathbuf);

  return TRUE;
}

/* ============================================================== */

gboolean
gnc_book_begin_file (GNCBook *book, const char * filefrag,
                     GNCBookLockFailHandler handler)
{
  if (!book) return FALSE;
  if (!book) return FALSE;

  /* clear the error condition of previous errors */
  book->errtype = 0;
  book->last_file_err = ERR_FILEIO_NONE;

  /* check to see if this session is already open */
  if (book->book_id)
  {
    book->errtype = ETXTBSY;
    return FALSE;
  }

  /* seriously invalid */
  if (!filefrag)
  {
    book->errtype = EINVAL;
    return FALSE;
  }

  /* ---------------------------------------------------- */
  /* OK, now we try to find or build an absolute file path */

  book->fullpath = xaccResolveFilePath (filefrag);
  if (!book->fullpath)
  {
    book->errtype = ERANGE;  
    return FALSE;    /* ouch */
  }

  /* Store the sessionid URL also ... */
  book->book_id = g_strconcat ("file:", filefrag, NULL);

  /* ---------------------------------------------------- */
  /* We should now have a fully resolved path name.
   * Lets see if we can get a lock on it. */

  book->lockfile = g_strconcat(book->fullpath, ".LCK", NULL);

  if (!gnc_book_get_file_lock (book))
  {
    if (!handler || !handler (book->fullpath))
    {
      g_free (book->book_id);  book->book_id = NULL;
      g_free (book->fullpath); book->fullpath = NULL;
      g_free (book->lockfile); book->lockfile = NULL;
      return FALSE;
    }
  }

  return TRUE;
}

#ifdef SQLHACK
  /* for testing the sql, just a hack, remove later ... */
  /* this should never ever appear here ...  */
  xaccSessionBeginSQL (sess, "postgres://localhost/gnc_bogus");
#endif
 
/* ============================================================== */

gboolean
gnc_book_load (GNCBook *book)
{
  if (!book) return FALSE;
  if (!book->book_id) return FALSE;

  if (strncmp(book->book_id, "file:", 5) == 0)
  {
    /* file: */

    if (!book->lockfile)
    {
      book->errtype = ENOLCK;
      return FALSE;
    }
      
    /* At this point, we should have a valid book id and a lock on
     * the file. */

    xaccFreeAccountGroup (book->topgroup);
    book->topgroup = NULL;

    book->errtype = 0;
    book->last_file_err = ERR_FILEIO_NONE;
    book->topgroup = xaccReadAccountGroupFile (book->fullpath,
                                               &(book->last_file_err));

    if (!book->topgroup || (book->last_file_err != ERR_FILEIO_NONE))
    {
      book->errtype = EIO;
      return FALSE;
    }

    return TRUE;
  }
  else
  {
    book->errtype = ENOSYS;
    return FALSE;
  }  
}

/* ============================================================== */

gboolean
gnc_book_save_may_clobber_data (GNCBook *book)
{
  /* FIXME: Make sure this doesn't need more sophisticated semantics
   * in the face of special file, devices, pipes, symlinks, etc. */

  struct stat statbuf;

  if (!book) return FALSE;
  if (!book->fullpath) return FALSE;
  if (stat(book->fullpath, &statbuf) == 0) return TRUE;

  return FALSE;

#ifdef SQLHACK
  /* for testing the sql, just a hack, remove later ... */
  /* this should never ever appear here ...  */
  xaccSessionBeginSQL (sess, "postgres://localhost/gnc_bogus");
#endif
}

/* ============================================================== */

void
gnc_book_save (GNCBook *book)
{
  if (!book) return;

  /* if the fullpath doesn't exist, either the user failed to initialize,
   * or the lockfile was never obtained. Either way, we can't write. */
  book->errtype = 0;
  book->last_file_err = ERR_FILEIO_NONE;

  if (!book->fullpath)
  {
    book->errtype = ENOLCK;
    return;
  }

  if (book->topgroup)
  {
    gboolean write_ok = xaccWriteAccountGroupFile (book->fullpath,
                                                   book->topgroup,
                                                   TRUE,
                                                   &(book->last_file_err));
    if (!write_ok) book->errtype = errno;
  }
}

/* ============================================================== */

void
gnc_book_end (GNCBook *book)
{
  if (!book) return;

  book->errtype = 0;
  book->last_file_err = ERR_FILEIO_NONE;

  if (book->linkfile)
    unlink (book->linkfile);

  if (book->lockfd > 0)
    close (book->lockfd);

  if (book->lockfile)
    unlink (book->lockfile);

  g_free (book->book_id);
  book->book_id = NULL;

  g_free (book->fullpath);
  book->fullpath = NULL;

  g_free (book->lockfile);
  book->lockfile = NULL;

  g_free (book->linkfile);
  book->linkfile = NULL;
}

void 
gnc_book_destroy (GNCBook *book) 
{
  if (!book) return;

  gnc_book_end (book);

  xaccFreeAccountGroup (book->topgroup);
  book->topgroup = NULL;

  g_free (book);
}


/* ============================================================== */
/* 
 * If $HOME/.gnucash/data directory doesn't exist, then create it.
 */

static void 
MakeHomeDir (void) 
{
  int rc;
  struct stat statbuf;
  char *home;
  char *path;
  char *data;

  /* Punt. Can't figure out where home is. */
  home = getenv ("HOME");
  if (!home) return;

  path = g_strconcat(home, "/.gnucash", NULL);

  rc = stat (path, &statbuf);
  if (rc)
  {
    /* assume that the stat failed only because the dir is absent,
     * and not because its read-protected or other error.
     * Go ahead and make it. Don't bother much with checking mkdir 
     * for errors; seems pointless. */
    mkdir (path, S_IRWXU);   /* perms = S_IRWXU = 0700 */
  }

  data = g_strconcat (path, "/data", NULL);
  rc = stat (data, &statbuf);
  if (rc)
    mkdir (data, S_IRWXU);

  g_free (path);
  g_free (data);
}

/* ============================================================== */

/* XXX hack alert -- we should be yanking this out of some config file */
static char * searchpaths[] =
{
   "/usr/share/gnucash/data/",
   NULL,
};

char * 
xaccResolveFilePath (const char * filefrag)
{
  struct stat statbuf;
  char pathbuf[PATH_MAX];
  char *path = NULL;
  int namelen, len;
  int i, rc;

  /* seriously invalid */
  if (!filefrag) return NULL;

  /* ---------------------------------------------------- */
  /* OK, now we try to find or build an absolute file path */

  /* check for an absolute file path */
  if ('/' == *filefrag)
    return strdup (filefrag);

  /* get conservative on the length so that sprintf(getpid()) works ... */
  /* strlen ("/.LCK") + sprintf (%x%d) */
  namelen = strlen (filefrag) + 25; 

  for (i=-2; 1 ; i++) 
  {
    switch (i)
    {
      case -2:
        /* try to find a file by this name in the cwd ... */
        path = getcwd (pathbuf, PATH_MAX);
        if (!path) continue;
        len = strlen (path) + namelen;
        if (PATH_MAX <= len) continue;
        strcat (path, "/");
        break;

      case -1:
        /* look for something in $HOME/.gnucash/data */
        path = getenv ("HOME");
        if (!path) continue;
        len = strlen (path) + namelen + 20;
        if (PATH_MAX <= len) continue;
        strcpy (pathbuf, path);
        strcat (pathbuf, "/.gnucash/data/");
        path = pathbuf;
        break;

      default:
        /* OK, check the user-configured paths */
        path = searchpaths[i];
        if (path)
        {
          len = strlen (path) + namelen;
          if (PATH_MAX <= len) continue;
          strcpy (pathbuf, path);
          path = pathbuf;
        }
        break;
    }

    if (!path) break;

    /* lets see if we found the file here ... */
    /* haral: if !S_ISREG: there is something with that name 
     * but it's not a regular file */
    strcat (path, filefrag);
    rc = stat (path, &statbuf);
    if ((!rc) && (S_ISREG(statbuf.st_mode)))
      return (strdup (path));
  }

  /* make sure that the gnucash home dir exists. */
  MakeHomeDir();

  /* OK, we didn't find the file. */
  /* If the user specified a simple filename (i.e. no slashes in it)
   * then create the file.  But if it has slashes in it, then creating
   * a bunch of directories seems like a bad idea; more likely, the user
   * specified a bad filename.  So return with error. */
  if (strchr (filefrag, '/'))
    return NULL;

  /* Lets try creating a new file in $HOME/.gnucash/data */
  path = getenv ("HOME");
  if (path)
  {
    len = strlen (path) + namelen + 50;
    if (PATH_MAX > len)
    {
      strcpy (pathbuf, path);
      strcat (pathbuf, "/.gnucash/data/");
      strcat (pathbuf, filefrag);
      return (strdup (pathbuf));
    }
  } 

  /* OK, we still didn't find the file */
  /* Lets try creating a new file in the cwd */
  path = getcwd (pathbuf, PATH_MAX);
  if (path)
  {
    len = strlen (path) + namelen;
    if (PATH_MAX > len)
    {
      strcat (path, "/");
      strcat (path, filefrag);
      return (strdup (path));
    }
  }

  return NULL;
}

/* ==================== END OF FILE ================== */
