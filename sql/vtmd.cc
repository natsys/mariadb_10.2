#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_table.h"
#include "rpl_utility.h" // auto_afree_ptr
#include "key.h"

LEX_STRING VERS_VTMD_TEMPLATE= {C_STRING_WITH_LEN("vtmd_template")};

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
    LEX_STRING_WITH_LEN(MYSQL_SCHEMA_NAME),
    LEX_STRING_WITH_LEN(VERS_VTMD_TEMPLATE),
    VERS_VTMD_TEMPLATE.str,
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
  key_copy((uchar*)key.get(), vtmd->record[0], vtmd->key_info + IDX_END, 0);

  error= vtmd->file->ha_index_read_idx_map(vtmd->record[1], IDX_END,
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
VTMD_table::write_row(THD *thd, const char* archive_name)
{
  TABLE_LIST vtmd_tl;
  bool result= true;
  bool need_close= false;
  bool need_rnd_end= false;
  bool found;
  bool created= false;
  int error;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  Diagnostics_area local_da(thd->query_id, false, true);
  Diagnostics_area *saved_da= thd->get_stmt_da();
  thd->set_stmt_da(&local_da);

  save_thd_options= thd->variables.option_bits;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;

  String vtmd_name;
  if (about.vers_vtmd_name(vtmd_name))
    goto err;

  while (true)
  {
    vtmd_tl.init_one_table(
      about.db, about.db_length,
      vtmd_name.ptr(), vtmd_name.length(),
      vtmd_name.ptr(),
      TL_WRITE_CONCURRENT_INSERT);

    vtmd= open_log_table(thd, &vtmd_tl, &open_tables_backup);
    if (vtmd)
      break;

    if (!created && local_da.is_error() && local_da.sql_errno() == ER_NO_SUCH_TABLE)
    {
      local_da.reset_diagnostics_area();
      if (create(thd, vtmd_name))
        goto err;
      created= true;
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

  if (!created && find_record(thd, ULONGLONG_MAX, found))
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

  if (vtmd->s->fields != FIELD_COUNT)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` unexpected fields count: %d", MYF(0),
                    vtmd->s->db.str, vtmd->s->table_name.str, vtmd->s->fields);
    goto err;
  }

  vtmd->field[FLD_NAME]->store(about.table_name, about.table_name_length, system_charset_info);
  vtmd->field[FLD_NAME]->set_notnull();
  if (archive_name)
  {
    vtmd->field[FLD_ARCHIVE_NAME]->store(archive_name, strlen(archive_name), files_charset_info);
    vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
  }
  else
  {
    vtmd->field[FLD_ARCHIVE_NAME]->set_null();
  }
  vtmd->field[FLD_COL_RENAMES]->set_null();

  if (found)
  {
    if (thd->lex->sql_command == SQLCOM_CREATE_TABLE)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` exists and not empty!", MYF(0),
                      vtmd->s->db.str, vtmd->s->table_name.str);
      goto err;
    }
    vtmd->mark_columns_needed_for_update(); // not needed?
    if (archive_name)
    {
      vtmd->s->versioned= false;
      error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
      vtmd->s->versioned= true;
      if (!error)
      {
        store_record(vtmd, record[1]);
        vtmd->field[FLD_ARCHIVE_NAME]->set_null();
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
      }
    }
    else
      error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
  }
  else
  {
    vtmd->mark_columns_needed_for_insert(); // not needed?
    error= vtmd->file->ha_write_row(vtmd->record[0]);
  }

  if (error)
  {
    vtmd->file->print_error(error, MYF(0));
    goto err;
  }

  result= false;

err:
  thd->set_stmt_da(saved_da);

  if (result)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), local_da.message());

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

bool VTMD_table::try_rename(THD *thd, const char *new_db, const char *new_alias)
{
  DBUG_ASSERT(new_db && new_alias);
  bool result= true;
  bool vtmd_exists;

  TABLE_LIST new_table;
  String new_vtmd_name;
  handlerton *hton;

  Diagnostics_area local_da(thd->query_id, false, true);
  Diagnostics_area *saved_da= thd->get_stmt_da();
  thd->set_stmt_da(&local_da);

  String vtmd_name;
  if (about.vers_vtmd_name(vtmd_name))
    goto err;

  vtmd_exists= ha_table_exists(thd, about.db, vtmd_name.ptr(), &hton);

  if (!hton) {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` handlerton empty!", MYF(0),
                        about.db, vtmd_name.ptr());
    goto err;
  }

  new_table.init_one_table(
    new_db, strlen(new_db),
    new_alias, strlen(new_alias),
    new_alias, TL_READ);

  if (new_table.vers_vtmd_name(new_vtmd_name))
    goto err;

  if (ha_table_exists(thd, new_db, new_vtmd_name.ptr()))
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` table already exists!", MYF(0),
                        new_db, new_vtmd_name.ptr());
    goto err;
  }

  if (vtmd_exists)
    result= mysql_rename_table(
      hton,
      new_db, vtmd_name.ptr(),
      new_db, new_vtmd_name.ptr(),
      NO_FK_CHECKS);

err:
  thd->set_stmt_da(saved_da);

  if (result)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), local_da.message());

  return result;
}
