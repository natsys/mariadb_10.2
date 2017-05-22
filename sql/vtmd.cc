#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_table.h"
#include "rpl_utility.h" // auto_afree_ptr
#include "key.h"

LEX_STRING VERS_VTMD_NAME= {C_STRING_WITH_LEN("vtmd")};

bool VTMD_table::create(THD *thd)
{
  Table_specification_st create_info;
  TABLE_LIST src_table, table;
  create_info.init(DDL_options_st::OPT_LIKE);
  create_info.alias= "vtmd0";
  table.init_one_table(
    STRING_WITH_LEN("test"),
    create_info.alias,
    strlen(create_info.alias),
    create_info.alias,
    TL_READ);
  src_table.init_one_table(
    MYSQL_SCHEMA_NAME.str,
    MYSQL_SCHEMA_NAME.length,
    STRING_WITH_LEN("vtmd"),
    "vtmd",
    TL_READ);

  mysql_create_like_table(thd, &table, &src_table, &create_info);

  return false;
}

bool VTMD_table::find_alive(THD *thd, TABLE *table, bool &found)
{
  int error;
  int keynum= 1;
  auto_afree_ptr<char> key(NULL);
  found= false;

  if (key.get() == NULL)
  {
    key.assign(static_cast<char*>(my_alloca(table->s->max_unique_length)));
    if (key.get() == NULL)
    {
      my_message(ER_VERS_VTMD_ERROR, "failed to allocate key buffer", MYF(0));
      return true;
    }
  }

  table->vers_end_field()->set_max();
  key_copy((uchar*)key.get(), table->record[0], table->key_info + keynum, 0);

  error= table->file->ha_index_read_idx_map(table->record[1], keynum,
                                            (const uchar*)key.get(),
                                            HA_WHOLE_KEY,
                                            HA_READ_KEY_EXACT);
  if (error)
  {
    if (error == HA_ERR_RECORD_DELETED || error == HA_ERR_KEY_NOT_FOUND)
      return false;
    table->file->print_error(error, MYF(0));
    return true;
  }

  restore_record(table, record[1]);

  found= true;
  return false;
}

bool VTMD_table::write_row(THD *thd, TABLE_LIST &about)
{
  TABLE_LIST table_list;
  TABLE *vtmd;
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

  table_list.init_one_table(about.db, about.db_length,
                            vtmd_name.ptr(), vtmd_name.length(),
                            vtmd_name.ptr(),
                            TL_WRITE_CONCURRENT_INSERT);

  if (!(vtmd= open_log_table(thd, &table_list, &open_tables_backup)))
  {
    if (new_stmt_da.is_error() && new_stmt_da.sql_errno() == ER_NO_SUCH_TABLE)
    {
      if (VTMD_table::create(thd))
        goto err;
      if (!(vtmd= open_log_table(thd, &table_list, &open_tables_backup)))
        goto err;
    }
    else
      goto err;
  }
  need_close= true;

  if (!vtmd->versioned())
  {
    my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
    goto err;
  }

  if (VTMD_table::find_alive(thd, vtmd, found))
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

  vtmd->field[OLD_NAME]->set_null();
  {
    time_t t= time(NULL);
    char *tmp= ctime(&t);
    vtmd->field[NAME]->store(tmp, strlen(tmp) - 5, system_charset_info);
  }
  vtmd->field[NAME]->set_notnull();
  vtmd->field[FRM_IMAGE]->set_null();
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
