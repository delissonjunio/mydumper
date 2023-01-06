/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#ifdef ZWRAP_USE_ZSTD
#include "../zstd/zstd_zlibwrapper.h"
#else
#include <zlib.h>
#endif
#include "common.h"
#include <errno.h>
#include "myloader.h"
#include "myloader_jobs_manager.h"
#include "myloader_common.h"
extern guint errors;
extern guint commit_count;
extern gchar *directory;
extern gchar *compress_extension;
extern guint rows;

gboolean skip_definer = FALSE;

int restore_data_in_gstring_by_statement(struct thread_data *td, GString *data, gboolean is_schema, guint *query_counter)
{
  if (mysql_real_query(td->thrconn, data->str, data->len)) {
    if (is_schema)
      g_critical("Thread %d: Error restoring: %s %s", td->thread_id, data->str, mysql_error(td->thrconn));
    else
      g_critical("Thread %d: Error restoring: %s", td->thread_id, mysql_error(td->thrconn));
    errors++;
    return 1;
  }
  *query_counter=*query_counter+1;
  if (is_schema==FALSE) {
    if (commit_count > 1) {
      if (*query_counter == commit_count) {
        *query_counter= 0;
        if (mysql_query(td->thrconn, "COMMIT")) {
          errors++;
          return 2;
        }
        mysql_query(td->thrconn, "START TRANSACTION");
      }
    }
  }
  g_string_set_size(data, 0);
  return 0;
}

int restore_data_in_gstring(struct thread_data *td, GString *data, gboolean is_schema, guint *query_counter)
{
  int i=0;
  int r=0;
  if (data != NULL && data->len > 4){
    gchar** line=g_strsplit(data->str, ";\n", -1);
    for (i=0; i < (int)g_strv_length(line);i++){
       if (strlen(line[i])>2){
         GString *str=g_string_new(line[i]);
         g_string_append_c(str,';');
         r+=restore_data_in_gstring_by_statement(td, str, is_schema, query_counter);
         g_string_free(str,TRUE);
       }
    }
    g_strfreev(line);
  }
  return r;
}

int split_and_restore_data_in_gstring_by_statement(struct thread_data *td,
                  GString *data, gboolean is_schema, guint *query_counter, guint offset_line)
{
  char *next_line=g_strstr_len(data->str,-1,"VALUES") + 6;
  char *insert_statement_prefix=g_strndup(data->str,next_line - data->str);
  guint insert_statement_prefix_len=strlen(insert_statement_prefix);
  int r=0;
  guint tr=0,current_offset_line=offset_line-1;
  gchar *current_line=next_line;
  next_line=g_strstr_len(current_line, -1, "\n");
  GString * new_insert=g_string_sized_new(strlen(insert_statement_prefix));
  guint current_rows=0;
  do {
    current_rows=0;
    g_string_set_size(new_insert, 0);
    new_insert=g_string_append(new_insert,insert_statement_prefix);
    do {
      char *line=g_strndup(current_line, next_line - current_line);
      g_string_append(new_insert, line);
      g_free(line);
      current_rows++;
      current_line=next_line+1;
      next_line=g_strstr_len(current_line, -1, "\n");
      current_offset_line++;
    } while (current_rows < rows && next_line != NULL);
    if (new_insert->len > insert_statement_prefix_len)
      tr=restore_data_in_gstring_by_statement(td, new_insert, is_schema, query_counter);
    else
      tr=0;
    r+=tr;
    if (tr > 0){
      g_critical("Error occurs between lines: %d and %d in a splited INSERT: %s",offset_line,current_offset_line,mysql_error(td->thrconn));
    }
    offset_line=current_offset_line+1;
    current_line++; // remove trailing ,
  } while (next_line != NULL);
  g_string_free(new_insert,TRUE);
  g_free(insert_statement_prefix);
  g_string_set_size(data, 0);
  return r;

}

void *send_file_to_fifo(gchar *compressed_filename){
  gchar *fifo_name=g_strndup(compressed_filename,g_strrstr(compressed_filename,".")-compressed_filename);
  FILE * fd = g_fopen(fifo_name, "w");
  FILE *file=NULL;
  gboolean is_compressed = FALSE;
  gchar *path = g_build_filename(directory, compressed_filename, NULL);
  ml_open(&file,path,&is_compressed);
  char buffer[256];
  gboolean eof=FALSE;
  guint l;
  do {
      if (!gzgets((gzFile)file, buffer, 256)) {
        if (gzeof((gzFile)file)) {
          eof = TRUE;
          buffer[0] = '\0';
        }
      }
    l=write(fileno(fd), buffer, strlen(buffer));
    if (l!=strlen(buffer))
      g_critical("Incomplete data transferred to FIFO: %s", fifo_name);
  } while (eof == FALSE);
  fclose(fd);
  return NULL;
}

int restore_data_from_file(struct thread_data *td, char *database, char *table,
                  const char *filename, gboolean is_schema){
  FILE *infile=NULL;
  int r=0;
  gboolean is_compressed = FALSE;
  gboolean eof = FALSE;
  guint query_counter = 0;
  GString *data = g_string_sized_new(256);
  guint line=0,preline=0;
  gchar *path = g_build_filename(directory, filename, NULL);
  ml_open(&infile,path,&is_compressed);

/*  if (!g_str_has_suffix(path, compress_extension)) {
    infile = g_fopen(path, "r");
    is_compressed = FALSE;
  } else {
    infile = (void *)gzopen(path, "r");
    is_compressed = TRUE;
  }*/

  if (!infile) {
    g_critical("cannot open file %s (%d)", filename, errno);
    errors++;
    return 1;
  }
  if (!is_schema && (commit_count > 1) )
    mysql_query(td->thrconn, "START TRANSACTION");
  guint tr=0;
  while (eof == FALSE) {
    if (read_data(infile, is_compressed, data, &eof, &line)) {
      if (g_strrstr(&data->str[data->len >= 5 ? data->len - 5 : 0], ";\n")) {
        if ( skip_definer && g_str_has_prefix(data->str,"CREATE")){
          remove_definer(data);
        }
        if (rows > 0 && g_strrstr_len(data->str,6,"INSERT"))
          tr=split_and_restore_data_in_gstring_by_statement(td,
            data, is_schema, &query_counter,preline);
        else{
          if (g_strrstr_len(data->str,10,"LOAD DATA ")){
            gchar *from = g_strstr_len(data->str, -1, "'");
            from++;
            gchar *to = g_strstr_len(from, -1, "'");
            gchar *fff=g_strndup(from, to-from);
            if (has_compession_extension(fff)){
              gchar *fifo_name=g_strndup(fff,g_strrstr(fff,".")-fff);
              mkfifo(fifo_name,0666);
              g_thread_create((GThreadFunc)send_file_to_fifo, fff, TRUE, NULL);
              char *start_point = g_strstr_len(to-4,-1,".");
              *start_point = '\'';
              for(from=start_point+1; from<to ; from++){
                *from=' ';
              }
            }
          }
          tr=restore_data_in_gstring_by_statement(td, data, is_schema, &query_counter);
        }
        r+=tr;
        if (tr > 0){
            g_critical("Error occurs between lines: %d and %d on file %s: %s",preline,line,filename,mysql_error(td->thrconn));
        }
        g_string_set_size(data, 0);
        preline=line+1;
      }
    } else {
      g_critical("error reading file %s (%d)", filename, errno);
      errors++;
      return r;
    }
  }
  if (!is_schema && (commit_count > 1) && mysql_query(td->thrconn, "COMMIT")) {
    g_critical("Error committing data for %s.%s from file %s: %s",
               database, table, filename, mysql_error(td->thrconn));
    errors++;
  }
  g_string_free(data, TRUE);
  if (!is_compressed) {
    fclose(infile);
  } else {
    gzclose((gzFile)infile);
  }

  m_remove(directory,filename);
  g_free(path);
  return r;
}

