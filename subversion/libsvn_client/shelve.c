/*
 * shelve.c:  implementation of the 'shelve' commands
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */

/* We define this here to remove any further warnings about the usage of
   experimental functions in this file. */
#define SVN_EXPERIMENTAL

#include "svn_client.h"
#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_utf.h"

#include "client.h"
#include "private/svn_wc_private.h"
#include "private/svn_sorts_private.h"
#include "svn_private_config.h"


/* Throw an error if NAME does not conform to our naming rules. */
static svn_error_t *
validate_name(const char *name,
              apr_pool_t *scratch_pool)
{
  if (name[0] == '\0' || strchr(name, '/'))
    return svn_error_createf(SVN_ERR_BAD_CHANGELIST_NAME, NULL,
                             _("Shelve: Bad name '%s'"), name);

  return SVN_NO_ERROR;
}

/* Set *PATCH_ABSPATH to the abspath of the patch file for SHELF
 * version VERSION, no matter whether it exists.
 */
static svn_error_t *
get_patch_abspath(const char **abspath,
                  svn_client_shelf_t *shelf,
                  int version,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  const char *filename;

  filename = apr_psprintf(scratch_pool, "%s-%03d.patch", shelf->name, version);
  *abspath = svn_dirent_join(shelf->shelves_dir, filename, result_pool);
  return SVN_NO_ERROR;
}

/* Set *PATCH_ABSPATH to the abspath of the patch file for SHELF
 * version VERSION. Error if VERSION is invalid or nonexistent.
 */
static svn_error_t *
get_existing_patch_abspath(const char **abspath,
                           svn_client_shelf_t *shelf,
                           int version,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  if (shelf->max_version <= 0)
    return svn_error_createf(SVN_ERR_CLIENT_BAD_REVISION, NULL,
                             _("shelf '%s': no versions available"),
                             shelf->name);
  if (version <= 0 || version > shelf->max_version)
    return svn_error_createf(SVN_ERR_CLIENT_BAD_REVISION, NULL,
                             _("shelf '%s' has no version %d: max version is %d"),
                             shelf->name, version, shelf->max_version);

  SVN_ERR(get_patch_abspath(abspath, shelf, version,
                            result_pool, scratch_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
shelf_delete_patch_file(svn_client_shelf_t *shelf,
                        int version,
                        apr_pool_t *scratch_pool)
{
  const char *patch_abspath;

  SVN_ERR(get_existing_patch_abspath(&patch_abspath, shelf, version,
                                     scratch_pool, scratch_pool));
  SVN_ERR(svn_io_remove_file2(patch_abspath, TRUE /*ignore_enoent*/,
                              scratch_pool));
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
get_log_abspath(char **log_abspath,
                svn_client_shelf_t *shelf,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  const char *filename;

  filename = apr_pstrcat(scratch_pool, shelf->name, ".log", SVN_VA_NULL);
  *log_abspath = svn_dirent_join(shelf->shelves_dir, filename, result_pool);
  return SVN_NO_ERROR;
}

/* Set SHELF->revprops by reading from its file storage.
 */
static svn_error_t *
shelf_read_revprops(svn_client_shelf_t *shelf,
                    apr_pool_t *scratch_pool)
{
  char *log_abspath;
  svn_error_t *err;
  svn_stream_t *stream;

  SVN_ERR(get_log_abspath(&log_abspath, shelf, scratch_pool, scratch_pool));

  shelf->revprops = apr_hash_make(shelf->pool);
  err = svn_stream_open_readonly(&stream, log_abspath,
                                 scratch_pool, scratch_pool);
  if (err && err->apr_err == APR_ENOENT)
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);
  SVN_ERR(svn_hash_read2(shelf->revprops, stream, "PROPS-END", shelf->pool));
  SVN_ERR(svn_stream_close(stream));
  return SVN_NO_ERROR;
}

/* Write SHELF's revprops to its file storage.
 */
static svn_error_t *
shelf_write_revprops(svn_client_shelf_t *shelf,
                     apr_pool_t *scratch_pool)
{
  char *log_abspath;
  apr_file_t *file;
  svn_stream_t *stream;

  SVN_ERR(get_log_abspath(&log_abspath, shelf, scratch_pool, scratch_pool));

  SVN_ERR(svn_io_file_open(&file, log_abspath,
                           APR_FOPEN_WRITE | APR_FOPEN_CREATE | APR_FOPEN_TRUNCATE,
                           APR_FPROT_OS_DEFAULT, scratch_pool));
  stream = svn_stream_from_aprfile2(file, FALSE /*disown*/, scratch_pool);

  SVN_ERR(svn_hash_write2(shelf->revprops, stream, "PROPS-END", scratch_pool));
  SVN_ERR(svn_stream_close(stream));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__shelf_revprop_set(svn_client_shelf_t *shelf,
                               const char *prop_name,
                               const svn_string_t *prop_val,
                               apr_pool_t *scratch_pool)
{
  svn_hash_sets(shelf->revprops, prop_name,
                svn_string_dup(prop_val, shelf->pool));
  SVN_ERR(shelf_write_revprops(shelf, scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__shelf_revprop_get(svn_string_t **prop_val,
                               svn_client_shelf_t *shelf,
                               const char *prop_name,
                               apr_pool_t *result_pool)
{
  *prop_val = svn_hash_gets(shelf->revprops, prop_name);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__shelf_revprop_list(apr_hash_t **props,
                               svn_client_shelf_t *shelf,
                               apr_pool_t *result_pool)
{
  *props = shelf->revprops;
  return SVN_NO_ERROR;
}

/*  */
static char *
get_current_abspath(svn_client_shelf_t *shelf,
                    apr_pool_t *result_pool)
{
  const char *current_filename
    = apr_psprintf(result_pool, "%s.current", shelf->name);
  return svn_dirent_join(shelf->shelves_dir, current_filename, result_pool);
}

/*  */
static svn_error_t *
shelf_read_current(svn_client_shelf_t *shelf,
                   apr_pool_t *scratch_pool)
{
  const char *current_abspath = get_current_abspath(shelf, scratch_pool);
  FILE *fp = fopen(current_abspath, "r");

  if (! fp)
    {
      shelf->max_version = 0;
      return SVN_NO_ERROR;
    }
  fscanf(fp, "%d", &shelf->max_version);
  fclose(fp);
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
shelf_write_current(svn_client_shelf_t *shelf,
                    apr_pool_t *scratch_pool)
{
  const char *current_abspath = get_current_abspath(shelf, scratch_pool);
  FILE *fp = fopen(current_abspath, "w");

  fprintf(fp, "%d", shelf->max_version);
  fclose(fp);
  return SVN_NO_ERROR;
}

/** Write local changes to a patch file.
 *
 * @a paths, @a depth, @a changelists: The selection of local paths to diff.
 *
 * @a paths are relative to CWD (or absolute). Paths in patch are relative
 * to WC root (@a wc_root_abspath).
 *
 * ### TODO: Ignore any external diff cmd as configured in config file.
 *     This might also solve the buffering problem.
 */
static svn_error_t *
write_patch(const char *patch_abspath,
            const apr_array_header_t *paths,
            svn_depth_t depth,
            const apr_array_header_t *changelists,
            const char *wc_root_abspath,
            svn_client_ctx_t *ctx,
            apr_pool_t *scratch_pool)
{
  apr_int32_t flag;
  apr_file_t *outfile;
  svn_stream_t *outstream;
  svn_stream_t *errstream;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;
  svn_opt_revision_t peg_revision = {svn_opt_revision_unspecified, {0}};
  svn_opt_revision_t start_revision = {svn_opt_revision_base, {0}};
  svn_opt_revision_t end_revision = {svn_opt_revision_working, {0}};

  /* Get streams for the output and any error output of the diff. */
  /* ### svn_stream_open_writable() doesn't work here: the buffering
         goes wrong so that diff headers appear after their hunks.
         For now, fix by opening the file without APR_BUFFERED. */
  flag = APR_FOPEN_WRITE | APR_FOPEN_CREATE | APR_FOPEN_TRUNCATE;
  SVN_ERR(svn_io_file_open(&outfile, patch_abspath,
                           flag, APR_FPROT_OS_DEFAULT, scratch_pool));
  outstream = svn_stream_from_aprfile2(outfile, FALSE /*disown*/, scratch_pool);
  errstream = svn_stream_empty(scratch_pool);

  for (i = 0; i < paths->nelts; i++)
    {
      const char *path = APR_ARRAY_IDX(paths, i, const char *);

      if (svn_path_is_url(path))
        return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                 _("'%s' is not a local path"), path);
      SVN_ERR(svn_dirent_get_absolute(&path, path, scratch_pool));

      SVN_ERR(svn_client_diff_peg6(
                     NULL /*options*/,
                     path,
                     &peg_revision,
                     &start_revision,
                     &end_revision,
                     wc_root_abspath,
                     depth,
                     TRUE /*notice_ancestry*/,
                     FALSE /*no_diff_added*/,
                     FALSE /*no_diff_deleted*/,
                     TRUE /*show_copies_as_adds*/,
                     FALSE /*ignore_content_type: FALSE -> omit binary files*/,
                     FALSE /*ignore_properties*/,
                     FALSE /*properties_only*/,
                     FALSE /*use_git_diff_format*/,
                     SVN_APR_LOCALE_CHARSET,
                     outstream,
                     errstream,
                     changelists,
                     ctx, iterpool));
    }
  SVN_ERR(svn_stream_close(outstream));
  SVN_ERR(svn_stream_close(errstream));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_open(svn_client_shelf_t **shelf_p,
                      const char *name,
                      const char *local_abspath,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *result_pool)
{
  svn_client_shelf_t *shelf = apr_palloc(result_pool, sizeof(*shelf));
  char *shelves_dir;

  SVN_ERR(validate_name(name, result_pool));

  SVN_ERR(svn_client_get_wc_root(&shelf->wc_root_abspath,
                                 local_abspath, ctx,
                                 result_pool, result_pool));
  SVN_ERR(svn_wc__get_shelves_dir(&shelves_dir,
                                  ctx->wc_ctx, local_abspath,
                                  result_pool, result_pool));
  shelf->shelves_dir = shelves_dir;
  shelf->ctx = ctx;
  shelf->pool = result_pool;

  shelf->name = apr_pstrdup(result_pool, name);
  SVN_ERR(shelf_read_revprops(shelf, result_pool));
  SVN_ERR(shelf_read_current(shelf, result_pool));

  *shelf_p = shelf;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_close(svn_client_shelf_t *shelf,
                       apr_pool_t *scratch_pool)
{
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_delete(const char *name,
                        const char *local_abspath,
                        svn_boolean_t dry_run,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *scratch_pool)
{
  svn_client_shelf_t *shelf;
  int i;
  char *abspath;

  SVN_ERR(validate_name(name, scratch_pool));

  SVN_ERR(svn_client_shelf_open(&shelf,
                                name, local_abspath, ctx, scratch_pool));

  /* Remove the patches. */
  for (i = shelf->max_version; i > 0; i--)
    {
      SVN_ERR(shelf_delete_patch_file(shelf, i, scratch_pool));
    }

  /* Remove the other files */
  SVN_ERR(get_log_abspath(&abspath, shelf, scratch_pool, scratch_pool));
  SVN_ERR(svn_io_remove_file2(abspath, TRUE /*ignore_enoent*/, scratch_pool));
  abspath = get_current_abspath(shelf, scratch_pool);
  SVN_ERR(svn_io_remove_file2(abspath, TRUE /*ignore_enoent*/, scratch_pool));

  SVN_ERR(svn_client_shelf_close(shelf, scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_get_paths(apr_hash_t **affected_paths,
                           svn_client_shelf_t *shelf,
                           int version,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  const char *patch_abspath;
  svn_patch_file_t *patch_file;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_t *paths = apr_hash_make(result_pool);

  SVN_ERR(get_existing_patch_abspath(&patch_abspath, shelf, version,
                                     result_pool, result_pool));
  SVN_ERR(svn_diff_open_patch_file(&patch_file, patch_abspath, result_pool));

  while (1)
    {
      svn_patch_t *patch;

      svn_pool_clear(iterpool);
      SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file,
                                        FALSE /*reverse*/,
                                        FALSE /*ignore_whitespace*/,
                                        iterpool, iterpool));
      if (! patch)
        break;
      svn_hash_sets(paths,
                    apr_pstrdup(result_pool, patch->old_filename),
                    apr_pstrdup(result_pool, patch->new_filename));
    }
  SVN_ERR(svn_diff_close_patch_file(patch_file, iterpool));
  svn_pool_destroy(iterpool);

  *affected_paths = paths;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_has_changes(svn_boolean_t *has_changes,
                             svn_client_shelf_t *shelf,
                             int version,
                             apr_pool_t *scratch_pool)
{
  apr_hash_t *patch_paths;

  SVN_ERR(svn_client_shelf_get_paths(&patch_paths, shelf, version,
                                     scratch_pool, scratch_pool));
  *has_changes = (apr_hash_count(patch_paths) != 0);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_apply(svn_client_shelf_t *shelf,
                       int version,
                       svn_boolean_t dry_run,
                       apr_pool_t *scratch_pool)
{
  const char *patch_abspath;

  SVN_ERR(get_existing_patch_abspath(&patch_abspath, shelf, version,
                                     scratch_pool, scratch_pool));
  SVN_ERR(svn_client_patch(patch_abspath, shelf->wc_root_abspath,
                           dry_run, 0 /*strip*/,
                           FALSE /*reverse*/,
                           FALSE /*ignore_whitespace*/,
                           TRUE /*remove_tempfiles*/, NULL, NULL,
                           shelf->ctx, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_unapply(svn_client_shelf_t *shelf,
                         int version,
                         svn_boolean_t dry_run,
                         apr_pool_t *scratch_pool)
{
  const char *patch_abspath;

  SVN_ERR(get_existing_patch_abspath(&patch_abspath, shelf, version,
                                     scratch_pool, scratch_pool));
  SVN_ERR(svn_client_patch(patch_abspath, shelf->wc_root_abspath,
                           dry_run, 0 /*strip*/,
                           TRUE /*reverse*/,
                           FALSE /*ignore_whitespace*/,
                           TRUE /*remove_tempfiles*/, NULL, NULL,
                           shelf->ctx, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_set_current_version(svn_client_shelf_t *shelf,
                                     int version,
                                     apr_pool_t *scratch_pool)
{
  int i;

  /* Delete any newer checkpoints */
  for (i = shelf->max_version; i > version; i--)
    {
      SVN_ERR(shelf_delete_patch_file(shelf, i, scratch_pool));
    }

  shelf->max_version = version;
  SVN_ERR(shelf_write_current(shelf, scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_export_patch(svn_client_shelf_t *shelf,
                              int version,
                              svn_stream_t *outstream,
                              apr_pool_t *scratch_pool)
{
  const char *patch_abspath;
  svn_stream_t *instream;

  SVN_ERR(get_existing_patch_abspath(&patch_abspath, shelf, version,
                                     scratch_pool, scratch_pool));
  SVN_ERR(svn_stream_open_readonly(&instream, patch_abspath,
                                   scratch_pool, scratch_pool));
  SVN_ERR(svn_stream_copy3(instream,
                           svn_stream_disown(outstream, scratch_pool),
                           NULL, NULL, scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_save_new_version(svn_client_shelf_t *shelf,
                                  const apr_array_header_t *paths,
                                  svn_depth_t depth,
                                  const apr_array_header_t *changelists,
                                  apr_pool_t *scratch_pool)
{
  int next_version = shelf->max_version + 1;
  const char *patch_abspath;
  apr_finfo_t file_info;

  SVN_ERR(get_patch_abspath(&patch_abspath, shelf, next_version,
                            scratch_pool, scratch_pool));
  SVN_ERR(write_patch(patch_abspath,
                      paths, depth, changelists,
                      shelf->wc_root_abspath,
                      shelf->ctx, scratch_pool));

  SVN_ERR(svn_io_stat(&file_info, patch_abspath, APR_FINFO_MTIME, scratch_pool));
  if (file_info.size > 0)
    {
      SVN_ERR(svn_client_shelf_set_current_version(shelf, next_version,
                                                   scratch_pool));
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_get_log_message(char **log_message,
                                 svn_client_shelf_t *shelf,
                                 apr_pool_t *result_pool)
{
  svn_string_t *propval = svn_hash_gets(shelf->revprops, "svn:log");

  if (propval)
    *log_message = apr_pstrdup(result_pool, propval->data);
  else
    *log_message = "";
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_set_log_message(svn_client_shelf_t *shelf,
                                 svn_boolean_t dry_run,
                                 apr_pool_t *scratch_pool)
{
  svn_client_ctx_t *ctx = shelf->ctx;
  const char *message = "";

  /* Fetch the log message and any other revprops */
  if (SVN_CLIENT__HAS_LOG_MSG_FUNC(ctx))
    {
      const char *tmp_file;
      apr_array_header_t *commit_items
        = apr_array_make(scratch_pool, 1, sizeof(void *));

      SVN_ERR(svn_client__get_log_msg(&message, &tmp_file, commit_items,
                                      ctx, scratch_pool));
      if (! message)
        return SVN_NO_ERROR;
    }
  if (message && !dry_run)
    {
      svn_string_t *propval = svn_string_create(message, shelf->pool);

      SVN_ERR(svn_client__shelf_revprop_set(shelf, "svn:log", propval,
                                            scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelves_list(apr_hash_t **shelved_patch_infos,
                        const char *local_abspath,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  const char *wc_root_abspath;
  char *shelves_dir;
  apr_hash_t *dirents;
  apr_hash_index_t *hi;

  SVN_ERR(svn_wc__get_wcroot(&wc_root_abspath, ctx->wc_ctx, local_abspath,
                             scratch_pool, scratch_pool));
  SVN_ERR(svn_wc__get_shelves_dir(&shelves_dir, ctx->wc_ctx, local_abspath,
                                  scratch_pool, scratch_pool));
  SVN_ERR(svn_io_get_dirents3(&dirents, shelves_dir, FALSE /*only_check_type*/,
                              result_pool, scratch_pool));

  *shelved_patch_infos = apr_hash_make(result_pool);

  /* Remove non-shelves */
  for (hi = apr_hash_first(scratch_pool, dirents); hi; hi = apr_hash_next(hi))
    {
      const char *filename = apr_hash_this_key(hi);
      svn_io_dirent2_t *dirent = apr_hash_this_val(hi);
      size_t len = strlen(filename);

      if (len > 6 && strcmp(filename + len - 8, ".current") == 0)
        {
          const char *name = apr_pstrndup(result_pool, filename, len - 8);
          svn_client_shelf_info_t *info
            = apr_palloc(result_pool, sizeof(*info));

          info->mtime = dirent->mtime;
          svn_hash_sets(*shelved_patch_infos, name, info);
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelves_any(svn_boolean_t *any_shelved,
                       const char *local_abspath,
                       svn_client_ctx_t *ctx,
                       apr_pool_t *scratch_pool)
{
  apr_hash_t *shelved_patch_infos;

  SVN_ERR(svn_client_shelves_list(&shelved_patch_infos, local_abspath,
                                  ctx, scratch_pool, scratch_pool));
  *any_shelved = apr_hash_count(shelved_patch_infos) != 0;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_shelf_version_get_info(svn_client_shelf_version_info_t **info_p,
                                  svn_client_shelf_t *shelf,
                                  int version,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  svn_client_shelf_version_info_t *info
    = apr_palloc(result_pool, sizeof(*info));
  const svn_io_dirent2_t *dirent;

  SVN_ERR(get_existing_patch_abspath(&info->patch_abspath, shelf, version,
                                     result_pool, scratch_pool));
  SVN_ERR(svn_io_stat_dirent2(&dirent,
                              info->patch_abspath,
                              FALSE /*verify_truename*/,
                              TRUE /*ignore_enoent*/,
                              result_pool, scratch_pool));
  info->mtime = dirent->mtime;
  *info_p = info;
  return SVN_NO_ERROR;
}
