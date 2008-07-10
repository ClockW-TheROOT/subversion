/*
 * crop.c: Cropping the WC 
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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

#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_client.h"
#include "svn_error_codes.h"
#include "svn_path.h"
#include "entries.h"

#include "svn_private_config.h"

/* Evaluate EXPR.  If it returns an error, return that error, unless
   the error's code is SVN_ERR_WC_LEFT_LOCAL_MOD, in which case clear
   the error and do not return. */
#define IGNORE_LOCAL_MOD(expr)                                   \
  do {                                                           \
    svn_error_t *__temp = (expr);                                \
    if (__temp)                                                  \
      {                                                          \
        if (__temp->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD)        \
          svn_error_clear(__temp);                               \
        else                                                     \
          return __temp;                                         \
      }                                                          \
  } while (0)

/* Helper function that crops the children of the DIR_PATH, under the constraint
 * of DEPTH. The DIR_PATH itself will never be cropped. The ADM_ACCESS is the
 * access baton that contains DIR_PATH. And the whole subtree should have been
 * locked.
 *
 * If NOTIFY_FUNC is not null, each file and ROOT of subtree will be reported
 * upon remove.
 */
svn_error_t *
crop_children(svn_wc_adm_access_t *adm_access,
              const char *dir_path,
              svn_depth_t depth,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *pool)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_wc_adm_access_t *dir_access;
  svn_wc_entry_t *dot_entry;
  apr_pool_t *subpool = svn_pool_create(pool), *iterpool;

  SVN_ERR(svn_wc_adm_retrieve(&dir_access, adm_access, dir_path, subpool));
  SVN_ERR(svn_wc_entries_read(&entries, dir_access, TRUE, subpool));
  dot_entry = apr_hash_get(entries, SVN_WC_ENTRY_THIS_DIR,
                           APR_HASH_KEY_STRING);

  /* Update the depth of target first, if needed. */
  if (dot_entry->depth > depth)
    {
      /* TODO(2843): Do we need to restore the modified depth if the user cancel this
         operation? */
      dot_entry->depth = depth;
      SVN_ERR(svn_wc__entries_write(entries, dir_access, subpool));
    }

  /* Looping over current directory's SVN entries: */
  iterpool = svn_pool_create(subpool);

  for (hi = apr_hash_first(subpool, entries); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      const char *this_path;
      void *val;
      apr_ssize_t klen;
      svn_wc_entry_t *current_entry;
      svn_pool_clear(iterpool);

      /* Get the next entry */
      apr_hash_this(hi, &key, &klen, &val);
      if (! strcmp(key, SVN_WC_ENTRY_THIS_DIR))
        continue;

      current_entry = val;
      this_path = svn_path_join(dir_path, current_entry->name, iterpool); 

      if (current_entry->kind == svn_node_file)
        {
          /* We currently crop on a directory basis. So don't worry about
             svn_depth_exclude here. And even we permit excluding a single
             file in the future, svn_wc_remove_from_revision_control() can
             also handle it. We only need to skip the notification in that
             case. */
          if (depth == svn_depth_empty)
            IGNORE_LOCAL_MOD
              (svn_wc_remove_from_revision_control(dir_access,
                                                   current_entry->name,
                                                   TRUE, /* destroy */
                                                   FALSE, /* instant error */
                                                   cancel_func,
                                                   cancel_baton,
                                                   iterpool));
          else
            continue;

        }
      else if (current_entry->kind == svn_node_dir)
        {
          if (current_entry->depth == svn_depth_exclude)
            {
              /* Preserve the excluded entry if the parent need it.
                 Anyway, don't report on excluded subdir, since they are
                 logically not exist. */
              if (depth < svn_depth_immediates)
                {
                  svn_wc__entry_remove(entries, current_entry->name);
                  SVN_ERR(svn_wc__entries_write(entries, dir_access, iterpool));
                }
              continue;
            }
          else if (depth < svn_depth_immediates)
            {
              svn_wc_adm_access_t *child_access;
              SVN_ERR(svn_wc_adm_retrieve(&child_access, dir_access,
                                          this_path, iterpool));

              IGNORE_LOCAL_MOD
                (svn_wc_remove_from_revision_control(child_access,
                                                     SVN_WC_ENTRY_THIS_DIR,
                                                     TRUE, /* destroy */
                                                     FALSE, /* instant error */
                                                     cancel_func,
                                                     cancel_baton,
                                                     iterpool));
            }
          else
            {
              SVN_ERR(crop_children(dir_access,
                                    this_path, 
                                    svn_depth_empty, 
                                    notify_func,
                                    notify_baton,
                                    cancel_func, 
                                    cancel_baton, 
                                    iterpool));
              continue;
            }
        }
      else
        {
          return svn_error_createf
            (SVN_ERR_NODE_UNKNOWN_KIND, NULL, _("Unknown entry kind for '%s'"),
             svn_path_local_style(this_path, pool));
        }

      if (notify_func)
        {
          svn_wc_notify_t *notify;
          notify = svn_wc_create_notify(this_path,
                                        svn_wc_notify_delete,
                                        iterpool);
          (*notify_func)(notify_baton, notify, iterpool);
        }
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_crop_tree(svn_wc_adm_access_t *anchor,
                 const char *target,
                 svn_depth_t depth,
                 svn_wc_notify_func2_t notify_func,
                 void *notify_baton,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;
  const char *full_path;
  svn_wc_adm_access_t *dir_access;

  /* Only makes sense when the depth is restrictive. */
  if (!(depth >= svn_depth_exclude && depth < svn_depth_infinity))
    return SVN_NO_ERROR;

  /* Only makes sense to crop a dir target. */
  full_path = svn_path_join(svn_wc_adm_access_path(anchor), target, pool);
  SVN_ERR(svn_wc_entry(&entry, full_path, anchor, FALSE, pool));
  if (!entry || entry->kind != svn_node_dir)
    return SVN_NO_ERROR;

  /* TODO(#2843): Re-consider the behavior of cropping items with scheduled
     add/delete. Maybe we don't need to setup the exclude flag when the taget
     is just added without history. */

  /* Crop the target itself if we are requested to. */
  if (depth == svn_depth_exclude)
    {
      svn_boolean_t is_root;
      svn_boolean_t entry_in_repos;

      /* If the target entry is just added without history, it does not exist
         in the repos. Does not setup the exclude flag in this situation. */
      entry_in_repos = ! ((entry->schedule == svn_wc_schedule_add 
                           || entry->schedule == svn_wc_schedule_replace) 
                          && !entry->copied);
      svn_wc_is_wc_root(&is_root, full_path, anchor, pool);
      if (! is_root && entry_in_repos)
        {
          const svn_wc_entry_t *parent_entry;
          apr_hash_t * parent_entries;
          svn_wc_entry_t *target_entry;

          SVN_ERR(svn_wc_entries_read(&parent_entries, anchor, FALSE, pool));
          parent_entry = apr_hash_get(parent_entries,
                                      SVN_WC_ENTRY_THIS_DIR,
                                      APR_HASH_KEY_STRING);
          /* Mark the target as excluded, if the parent require it by default.
          */
          if (parent_entry && parent_entry->depth > svn_depth_files)
            {
              target_entry = apr_hash_get(parent_entries, 
                                          svn_path_basename(full_path, pool),
                                          APR_HASH_KEY_STRING);

              target_entry->depth = svn_depth_exclude;
              SVN_ERR(svn_wc__entries_write(parent_entries, anchor, pool));
            }
        }

      SVN_ERR(svn_wc_adm_retrieve(&dir_access, anchor, full_path, pool));
      IGNORE_LOCAL_MOD
        (svn_wc_remove_from_revision_control(dir_access,
                                             SVN_WC_ENTRY_THIS_DIR,
                                             TRUE, /* destroy */
                                             FALSE, /* instant error */
                                             cancel_func,
                                             cancel_baton,
                                             pool));

      if (notify_func)
        {
          svn_wc_notify_t *notify;
          notify = svn_wc_create_notify(full_path,
                                        svn_wc_notify_delete,
                                        pool);
          (*notify_func)(notify_baton, notify, pool);
        }
      return SVN_NO_ERROR;
    }

  return crop_children(anchor, full_path, depth,
                       notify_func, notify_baton,
                       cancel_func, cancel_baton, pool);
}
