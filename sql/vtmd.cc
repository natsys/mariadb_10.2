#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "rpl_utility.h" // auto_afree_ptr
#include "key.h"

LEX_STRING VERS_VTMD_NAME= {C_STRING_WITH_LEN("vtmd")};

bool VTMD_table::find_alive(THD *thd, TABLE *table)
{
  int error;
  int UNINIT_VAR(keynum);
  auto_afree_ptr<char> key(NULL);

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
    if (error == HA_ERR_RECORD_DELETED)
      error= HA_ERR_KEY_NOT_FOUND;
    table->file->print_error(error, MYF(0));
    return true;
  }
  return false;
}

bool VTMD_table::write_row(THD *thd)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= true;
  bool need_close= false;
  bool need_rnd_end= false;
  int error;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  Diagnostics_area new_stmt_da(thd->query_id, false, true);
  Diagnostics_area *save_stmt_da= thd->get_stmt_da();
  thd->set_stmt_da(&new_stmt_da);

  save_thd_options= thd->variables.option_bits;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;

  table_list.init_one_table(MYSQL_SCHEMA_NAME.str, MYSQL_SCHEMA_NAME.length,
                            VERS_VTMD_NAME.str, VERS_VTMD_NAME.length,
                            VERS_VTMD_NAME.str,
                            TL_WRITE_CONCURRENT_INSERT);

  if (!(table= open_log_table(thd, &table_list, &open_tables_backup)))
    goto err;
  need_close= true;

  if ((error= table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)) ||
      (error= table->file->ha_rnd_init(0)))
  {
    table->file->print_error(error, MYF(0));
    goto err;
  }
  need_rnd_end= true;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  if (table->s->fields != 6)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "unexpected fields count: %d", MYF(0), table->s->fields);
    goto err;
  }

  table->field[OLD_NAME]->set_null();
  table->field[NAME]->store(STRING_WITH_LEN("name"), system_charset_info);
  table->field[NAME]->set_notnull();
  table->field[FRM_IMAGE]->set_null();
  table->field[COL_RENAMES]->set_null();

  if ((error= table->file->ha_write_row(table->record[0]))  )
  {
    table->file->print_error(error, MYF(0));
    goto err;
  }

  result= false;

err:
  thd->set_stmt_da(save_stmt_da);

  if (result)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), new_stmt_da.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }

  if (need_close)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  return result;
}
