#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_table.h"
#include "rpl_utility.h" // auto_afree_ptr
#include "key.h"

class MDL_auto_lock
{
  THD *thd;
  TABLE_LIST &table;
  bool error;

public:
  MDL_auto_lock(THD *_thd, TABLE_LIST &_table) :
    thd(_thd), table(_table)
  {
    DBUG_ASSERT(thd);
    table.mdl_request.init(MDL_key::TABLE, table.db, table.table_name, MDL_EXCLUSIVE, MDL_EXPLICIT);
    error= thd->mdl_context.acquire_lock(&table.mdl_request, thd->variables.lock_wait_timeout);
  }
  ~MDL_auto_lock()
  {
    if (!error)
    {
      DBUG_ASSERT(table.mdl_request.ticket);
      thd->mdl_context.release_lock(table.mdl_request.ticket);
      table.mdl_request.ticket= NULL;
    }
  }
  bool acquire_error() const { return error; }
};

bool
VTMD_table::create(THD *thd, String &vtmd_name)
{
  Table_specification_st create_info;
  TABLE_LIST src_table, table;
  create_info.init(DDL_options_st::OPT_LIKE);
  create_info.options|= HA_VTMD;
  create_info.alias= vtmd_name.ptr();
  table.init_one_table(
    about.db, about.db_length,
    vtmd_name.ptr(), vtmd_name.length(),
    vtmd_name.ptr(),
    TL_READ);
  src_table.init_one_table(
    MYSQL_SCHEMA_NAME.str,
    MYSQL_SCHEMA_NAME.length,
    STRING_WITH_LEN("vtmd"),
    "vtmd",
    TL_READ);

  Query_tables_backup backup(thd);
  thd->lex->sql_command= backup.get().sql_command;
  thd->lex->add_to_query_tables(&src_table);

  MDL_auto_lock mdl_lock(thd, table);
  if (mdl_lock.acquire_error())
    return true;

  return mysql_create_like_table(thd, &table, &src_table, &create_info);
}

bool
VTMD_table::find_record(THD *thd, ulonglong sys_trx_end, bool &found)
{
  int error;
  int keynum= 1;
  auto_afree_ptr<char> key(NULL);
  found= false;

  DBUG_ASSERT(vtmd);

  if (key.get() == NULL)
  {
    key.assign(static_cast<char*>(my_alloca(vtmd->s->max_unique_length)));
    if (key.get() == NULL)
    {
      my_message(ER_VERS_VTMD_ERROR, "failed to allocate key buffer", MYF(0));
      return true;
    }
  }

  DBUG_ASSERT(sys_trx_end);
  vtmd->vers_end_field()->set_notnull();
  vtmd->vers_end_field()->store(sys_trx_end, true);
  key_copy((uchar*)key.get(), vtmd->record[0], vtmd->key_info + keynum, 0);

  error= vtmd->file->ha_index_read_idx_map(vtmd->record[1], keynum,
                                            (const uchar*)key.get(),
                                            HA_WHOLE_KEY,
                                            HA_READ_KEY_EXACT);
  if (error)
  {
    if (error == HA_ERR_RECORD_DELETED || error == HA_ERR_KEY_NOT_FOUND)
      return false;
    vtmd->file->print_error(error, MYF(0));
    return true;
  }

  restore_record(vtmd, record[1]);

  found= true;
  return false;
}

bool
VTMD_table::write_row(THD *thd)
{
  TABLE_LIST vtmd_tl;
  bool result= true;
  bool need_close= false;
  bool need_rnd_end= false;
  bool found;
  int error;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  Diagnostics_area new_stmt_da(thd->query_id, false, true);
  Diagnostics_area *save_stmt_da= thd->get_stmt_da();
  thd->set_stmt_da(&new_stmt_da);

  save_thd_options= thd->variables.option_bits;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;

  String vtmd_name;
  if (about.vers_vtmd_name(vtmd_name))
    goto err;

  for (int tries= 2; tries; --tries)
  {
    vtmd_tl.init_one_table(
      about.db, about.db_length,
      vtmd_name.ptr(), vtmd_name.length(),
      vtmd_name.ptr(),
      TL_WRITE_CONCURRENT_INSERT);

    vtmd= open_log_table(thd, &vtmd_tl, &open_tables_backup);
    if (vtmd)
      break;

    if (tries == 2 && new_stmt_da.is_error() && new_stmt_da.sql_errno() == ER_NO_SUCH_TABLE)
    {
      if (create(thd, vtmd_name))
        goto err;
      continue;
    }
    goto err;
  }
  need_close= true;

  if (!vtmd->versioned())
  {
    my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
    goto err;
  }

  if (find_record(thd, ULONGLONG_MAX, found))
    goto err;

  if ((error= vtmd->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)) ||
      (error= vtmd->file->ha_rnd_init(0)))
  {
    vtmd->file->print_error(error, MYF(0));
    goto err;
  }
  need_rnd_end= true;

  /* Honor next number columns if present */
  vtmd->next_number_field= vtmd->found_next_number_field;

  if (vtmd->s->fields != 6)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "unexpected fields count: %d", MYF(0), vtmd->s->fields);
    goto err;
  }

  {
    time_t t= time(NULL);
    char *tmp= ctime(&t);
    vtmd->field[NAME]->store(tmp, strlen(tmp) - 5, system_charset_info);
  }
  vtmd->field[NAME]->set_notnull();
  vtmd->field[ARCHIVE_NAME]->set_null();
  vtmd->field[COL_RENAMES]->set_null();

  if (found)
  {
    vtmd->mark_columns_needed_for_update();
    error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
  }
  else
  {
    vtmd->mark_columns_needed_for_insert();
    error= vtmd->file->ha_write_row(vtmd->record[0]);
  }

  if (error)
  {
    vtmd->file->print_error(error, MYF(0));
    goto err;
  }

  result= false;

err:
  thd->set_stmt_da(save_stmt_da);

  if (result)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), new_stmt_da.message());

  if (need_rnd_end)
  {
    vtmd->file->ha_rnd_end();
    vtmd->file->ha_release_auto_increment();
  }

  if (need_close)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  return result;
}
