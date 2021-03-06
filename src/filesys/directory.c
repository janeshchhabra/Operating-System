#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "devices/block.h"
#include "threads/thread.h"

#define ASCII_SLASH 47

/* A directory. */
struct dir 
{
  struct inode *inode;                /* Backing store. */
  off_t pos;                          /* Current position. */
};

/* A single directory entry. */
struct dir_entry 
{
  block_sector_t inode_sector;        /* Sector number of header. */
  char name[NAME_MAX + 1];            /* Null terminated file name. */
  bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
   //printf("dir_ope function\n\n");

  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
  {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  }
  else
  {
    inode_close (inode);
    free (dir);
    return NULL; 
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
  {
    inode_close (dir->inode);
    free (dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
    struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
      ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
    {
      if (ep != NULL)
	*ep = e;
      if (ofsp != NULL)
	*ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
    struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  if(!inode_add_parent(inode_get_inumber(dir_get_inode(dir)), inode_sector)) goto done;
  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
      ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* disallow deletion of a directory that is open by a process 
     or in use as a process's current working directory */ 
  if(inode_is_dir(inode) && inode_get_open_cnt(inode) > 1)
    goto done;

  /* disallow deletion of root dir */
  if(inode_is_dir(inode) && inode_get_inumber(inode) == 1)
    goto done;
  
  /* a directory can be deleted only when it is empty */
  if(inode_is_dir(inode) && !dir_is_empty(inode))
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
  {
    dir->pos += sizeof e;
    if (e.in_use)
    {
      strlcpy (name, e.name, NAME_MAX + 1);
      return true;
    } 
  }
  return false;
}

/* Returns true if dir is root. Otherwise returns false */
bool dir_is_root (struct dir *dir)
{ 
  if(!dir) return false;
  if(inode_get_inumber(dir_get_inode(dir)) == ROOT_DIR_SECTOR) return true;
  return false;
}

/* Returns true if it succeeds in saving parent dir inode. Otherwise returns false */
bool dir_get_parent (struct dir *dir, struct inode **inode)
{
  block_sector_t sector = inode_get_parent(dir_get_inode(dir));
  *inode = inode_open(sector);
  return *inode != NULL;
}

/* returns true if dir is empty. returns false otherwise */
bool dir_is_empty(struct inode *inode)
{
  struct dir_entry e;
  off_t pos = 0;

  while(inode_read_at(inode, &e, sizeof e, pos) == sizeof e)
  {
    pos += sizeof e;
    if(e.in_use) return false;
  }
  return true;
}


/* return the directory which contains the file */
struct dir *get_containing_dir(const char * path) 
{
  char path_string[strlen(path) + 1];
  struct dir *dir;
  struct thread *cur = thread_current();
  char *save_ptr;
  char *token;
  char *next_token;
  struct inode *inode;

  memcpy(path_string, path, strlen(path) + 1);

  if(path_string[0] == ASCII_SLASH || !cur->cwd) // if the pathstarts with slash or cwd is null
    dir = dir_open_root(); // absolute path
  else dir = dir_reopen(cur->cwd); // relative path

  token = strtok_r(path_string, "/", &save_ptr);

  if(token) next_token = strtok_r(NULL, "/", &save_ptr);

  while(next_token)
  {
    if(strcmp(token, ".") != 0) // token doesn't mean current path
    {
      if(strcmp(token, "..") == 0) // token indicates parent directory
      {
	if(!dir_get_parent(dir, &inode)) return NULL; // save parent inode. if fails, return NULL
      }
      else
      {
	if(!dir_lookup(dir, token, &inode)) return NULL; // save inode corresponding to token. if fails, return NULL
      }

      if(inode_is_dir(inode))
      {
	dir_close(dir);
	dir = dir_open(inode);
      }
      else inode_close(inode);
    }

    token = next_token;
    next_token = strtok_r(NULL, "/", &save_ptr);
  }
  return dir;
}


/* extract file name from the whole path argument */
char *get_file_name(const char *path)
{
  char path_string[strlen(path) + 1];
  char *save_ptr;
  char *token;
  char *next_token;

  memcpy(path_string, path, strlen(path) + 1);

  token = strtok_r(path_string, "/", &save_ptr);
  while(token)
  {
    next_token = strtok_r(NULL, "/", &save_ptr);
    if(!next_token) break;
    token = next_token;
  }

  char *file_name = malloc(strlen(token) + 1);
  memcpy(file_name, token, strlen(token) + 1);
  return file_name;
}
