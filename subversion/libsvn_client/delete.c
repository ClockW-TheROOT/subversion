/*
 * delete.c:  wrappers around wc delete functionality.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include <apr_file_io.h>
#include "svn_types.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "client.h"



/*** Code. ***/

svn_error_t *
svn_client__can_delete (const char *path,
                        svn_wc_adm_access_t *adm_access,
                        apr_pool_t *pool)
{
  apr_hash_t *hash = apr_hash_make (pool);
  apr_hash_index_t *hi;
  svn_node_kind_t kind;
  svn_wc_adm_access_t *dir_access;

  SVN_ERR (svn_io_check_path (path, &kind, pool));
  if (kind == svn_node_dir)
    SVN_ERR (svn_wc_adm_retrieve (&dir_access, adm_access, path, pool));
  else
    dir_access = adm_access;

  SVN_ERR (svn_wc_statuses (hash, path, dir_access, TRUE, FALSE, FALSE,
                            NULL, NULL, pool));
  for (hi = apr_hash_first (pool, hash); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      const char *name;
      const svn_wc_status_t *statstruct;

      apr_hash_this (hi, &key, NULL, &val);
      name = key;
      statstruct = val;


      if (statstruct->text_status == svn_wc_status_obstructed)
        {
          return svn_error_createf (SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                                    "'%s' is in the way of the resource "
                                    "actually under revision control.",
                                    name);
        }

      if (!statstruct->entry)
        {
          return svn_error_createf (SVN_ERR_CLIENT_UNVERSIONED, NULL,
                                    "'%s' is not under revision control",
                                    name);
        }

      if ((statstruct->text_status != svn_wc_status_normal
           && statstruct->text_status != svn_wc_status_deleted
           && statstruct->text_status != svn_wc_status_absent)
          ||
          (statstruct->prop_status != svn_wc_status_none
           && statstruct->prop_status != svn_wc_status_normal))
        {
          return svn_error_createf (SVN_ERR_CLIENT_MODIFIED, NULL,
                                    "'%s' has local modifications",
                                    name);
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_delete (svn_client_commit_info_t **commit_info,
                   const char *path,
                   svn_wc_adm_access_t *optional_adm_access,
                   svn_boolean_t force, 
                   svn_client_auth_baton_t *auth_baton,
                   svn_client_get_commit_log_t log_msg_func,
                   void *log_msg_baton,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;

  if (svn_path_is_url (path))
    {
      /* This is a remote removal.  */
      void *ra_baton, *session;
      svn_ra_plugin_t *ra_lib;
      const char *anchor, *target;
      const svn_delta_editor_t *editor;
      void *edit_baton;
      void *root_baton;
      svn_revnum_t committed_rev = SVN_INVALID_REVNUM;
      const char *committed_date = NULL;
      const char *committed_author = NULL;
      const char *log_msg;
      svn_node_kind_t kind;

      /* Create a new commit item and add it to the array. */
      if (log_msg_func)
        {
          svn_client_commit_item_t *item;
          const char *tmp_file;
          apr_array_header_t *commit_items 
            = apr_array_make (pool, 1, sizeof (item));
          
          item = apr_pcalloc (pool, sizeof (*item));
          item->url = apr_pstrdup (pool, path);
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_DELETE;
          (*((svn_client_commit_item_t **) apr_array_push (commit_items))) 
            = item;
          
          SVN_ERR ((*log_msg_func) (&log_msg, &tmp_file, commit_items, 
                                    log_msg_baton, pool));
          if (! log_msg)
            return SVN_NO_ERROR;
        }
      else
        log_msg = "";

      svn_path_split (path, &anchor, &target, pool);
      target = svn_path_uri_decode (target, pool);

      /* Get the RA vtable that matches URL. */
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, anchor, pool));

      /* Open an RA session for the URL. Note that we don't have a local
         directory, nor a place to put temp files or store the auth data. */
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, anchor, NULL,
                                            NULL, NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));

      /* Verify that the thing to be deleted actually exists. */
      SVN_ERR (ra_lib->check_path (&kind, session, target, 
                                   SVN_INVALID_REVNUM));
      if (kind == svn_node_none)
        return svn_error_createf (SVN_ERR_FS_NOT_FOUND, NULL,
                                  "URL `%s' does not exist", path);

      /* Fetch RA commit editor */
      SVN_ERR (ra_lib->get_commit_editor (session, &editor, &edit_baton,
                                          &committed_rev,
                                          &committed_date,
                                          &committed_author,
                                          log_msg));

      /* Drive the editor to delete the TARGET. */
      SVN_ERR (editor->open_root (edit_baton, SVN_INVALID_REVNUM, pool,
                                  &root_baton));
      SVN_ERR (editor->delete_entry (target, SVN_INVALID_REVNUM, 
                                     root_baton, pool));
      SVN_ERR (editor->close_directory (root_baton, pool));
      SVN_ERR (editor->close_edit (edit_baton, pool));

      /* Fill in the commit_info structure. */
      *commit_info = svn_client__make_commit_info (committed_rev,
                                                   committed_author,
                                                   committed_date,
                                                   pool);
      
      /* Free the RA session */
      SVN_ERR (ra_lib->close (session));

      return SVN_NO_ERROR;
    }
  

  if (! optional_adm_access)
    {
      const char *parent_path;
  
      parent_path = svn_path_dirname (path, pool);
      SVN_ERR (svn_wc_adm_open (&adm_access, NULL, parent_path, TRUE, TRUE,
                                pool));
    }
  else
    adm_access = optional_adm_access;

  if (!force)
    {
      /* Verify that there are no "awkward" files */
      SVN_ERR (svn_client__can_delete (path, adm_access, pool));
    }

  /* Mark the entry for commit deletion and perform wc deletion */
  SVN_ERR (svn_wc_delete (path, adm_access, notify_func, notify_baton, pool));

  if (! optional_adm_access)
    SVN_ERR (svn_wc_adm_close (adm_access));

  return SVN_NO_ERROR;
}
