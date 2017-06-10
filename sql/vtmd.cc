#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_handler.h" // mysql_ha_rm_tables()
#include "sql_table.h"
#include "table_cache.h" // tdc_remove_table()
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

struct Compare_strncmp
{
  int operator()(const LEX_STRING& a, const LEX_STRING& b) const
  {
    return strncmp(a.str, b.str, a.length);
  }
  static CHARSET_INFO* charset()
  {
    return system_charset_info;
  }
};

template <CHARSET_INFO* &CS= system_charset_info>
struct Compare_my_strcasecmp
{
  int operator()(const LEX_STRING& a, const LEX_STRING& b) const
  {
    DBUG_ASSERT(a.str[a.length] == 0 && b.str[b.length] == 0);
    return my_strcasecmp(CS, a.str, b.str);
  }
  static CHARSET_INFO* charset()
  {
    return CS;
  }
};

typedef Compare_my_strcasecmp<files_charset_info> Compare_fs;

struct LEX_STRING_u : public LEX_STRING
{
  LEX_STRING_u()
  {
    str= NULL;
    LEX_STRING::length= 0;
  }
  LEX_STRING_u(const char *_str, uint32 _len, CHARSET_INFO *)
  {
    str= const_cast<char *>(_str);
    LEX_STRING::length= _len;
  }
  uint32 length() const
  {
    return LEX_STRING::length;
  }
};

template <class Compare= Compare_strncmp, class Storage= LEX_STRING_u>
struct CString_any : public Storage
{
public:
  CString_any() {}
  CString_any(char *_str, size_t _len) :
    Storage(_str, _len, Compare::charset())
  {
  }
  CString_any(LEX_STRING& src) :
    Storage(src.str, src.length, Compare::charset())
  {
  }
  bool operator== (const CString_any& b) const
  {
    return Storage::length() == b.length() && 0 == Compare()(*this, b);
  }
  bool operator!= (const CString_any& b) const
  {
    return !(*this == b);
  }
};

typedef CString_any<> CString;
typedef CString_any<Compare_fs> CString_fs;

typedef CString_any<Compare_fs, String> String_fs;


class Local_da : public Diagnostics_area
{
  THD *thd;
  uint sql_error;
  Diagnostics_area *saved_da;

public:
  Local_da(THD *_thd, uint _sql_error= 0) :
    Diagnostics_area(_thd->query_id, false, true),
    thd(_thd),
    sql_error(_sql_error),
    saved_da(_thd->get_stmt_da())
  {
    thd->set_stmt_da(this);
  }
  ~Local_da()
  {
    if (saved_da)
      finish();
  }
  void finish()
  {
    DBUG_ASSERT(saved_da && thd);
    thd->set_stmt_da(saved_da);
    if (is_error())
      my_error(sql_error ? sql_error : sql_errno(), MYF(0), message());
    if (warn_count() > error_count())
      saved_da->copy_non_errors_from_wi(thd, get_warning_info());
    saved_da= NULL;
  }
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
VTMD_table::find_record(ulonglong sys_trx_end, bool &found)
{
  int error;
  key_buf_t key;
  found= false;

  DBUG_ASSERT(vtmd);

  if (key.allocate(vtmd->s->max_unique_length))
    return true;

  DBUG_ASSERT(sys_trx_end);
  vtmd->vers_end_field()->set_notnull();
  vtmd->vers_end_field()->store(sys_trx_end, true);
  key_copy(key, vtmd->record[0], vtmd->key_info + IDX_TRX_END, 0);

  error= vtmd->file->ha_index_read_idx_map(vtmd->record[1], IDX_TRX_END,
                                            key,
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
VTMD_table::update(THD *thd, const char* archive_name)
{
  TABLE_LIST vtmd_tl;
  bool result= true;
  bool close_log= false;
  bool found= false;
  bool created= false;
  int error;
  size_t an_len= 0;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  {
    Local_da local_da(thd, ER_VERS_VTMD_ERROR);

    save_thd_options= thd->variables.option_bits;
    thd->variables.option_bits&= ~OPTION_BIN_LOG;

    if (about.vers_vtmd_name(vtmd_name))
      goto quit;

    while (true) // max 2 iterations
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
          goto quit;
        created= true;
        continue;
      }
      goto quit;
    }
    close_log= true;

    if (!vtmd->versioned())
    {
      my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
      goto quit;
    }

    if (!created && find_record(ULONGLONG_MAX, found))
      goto quit;

    if ((error= vtmd->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)))
    {
      vtmd->file->print_error(error, MYF(0));
      goto quit;
    }

    /* Honor next number columns if present */
    vtmd->next_number_field= vtmd->found_next_number_field;

    if (vtmd->s->fields != FIELD_COUNT)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` unexpected fields count: %d", MYF(0),
                      vtmd->s->db.str, vtmd->s->table_name.str, vtmd->s->fields);
      goto quit;
    }

    vtmd->field[FLD_NAME]->store(about.table_name, about.table_name_length, system_charset_info);
    vtmd->field[FLD_NAME]->set_notnull();
    if (archive_name)
    {
      an_len= strlen(archive_name);
      vtmd->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
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
        goto quit;
      }
      vtmd->mark_columns_needed_for_update(); // not needed?
      if (archive_name)
      {
        vtmd->s->versioned= false;
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
        vtmd->s->versioned= true;

        if (!error)
        {
          if (thd->lex->sql_command == SQLCOM_DROP_TABLE)
          {
            error= vtmd->file->ha_delete_row(vtmd->record[0]);
          }
          else
          {
            DBUG_ASSERT(thd->lex->sql_command == SQLCOM_ALTER_TABLE);
            ulonglong sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            store_record(vtmd, record[1]);
            vtmd->field[FLD_ARCHIVE_NAME]->set_null();
            error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
            if (error)
              goto err;

            DBUG_ASSERT(an_len);
            while (true)
            { // fill archive_name of last sequential renames
              bool found;
              if (find_record(sys_trx_end, found))
                goto quit;
              if (!found || !vtmd->field[FLD_ARCHIVE_NAME]->is_null())
                break;

              store_record(vtmd, record[1]);
              vtmd->field[FLD_ARCHIVE_NAME]->store(archive_name, an_len, table_alias_charset);
              vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
              vtmd->s->versioned= false;
              error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
              vtmd->s->versioned= true;
              if (error)
                goto err;
              sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            } // while (true)
          } // else (thd->lex->sql_command != SQLCOM_DROP_TABLE)
        } // if (!error)
      } // if (archive_name)
      else
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
    } // if (found)
    else
    {
      vtmd->mark_columns_needed_for_insert(); // not needed?
      error= vtmd->file->ha_write_row(vtmd->record[0]);
    }

    if (error)
    {
err:
      vtmd->file->print_error(error, MYF(0));
      goto quit;
    }

    result= false;
  }

quit:
  if (close_log)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  return result;
}

inline
bool
VTMD_exists::check_exists(THD *thd)
{
  if (about.vers_vtmd_name(vtmd_name))
    return true;

  exists= ha_table_exists(thd, about.db, vtmd_name.ptr(), &hton);

  if (exists && !hton)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` handlerton empty!", MYF(0),
                        about.db, vtmd_name.ptr());
    return true;
  }
  return false;
}

bool
VTMD_rename::move_archives(THD *thd, LEX_STRING &new_db)
{
  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    about.db, about.db_length,
    vtmd_name.ptr(), vtmd_name.length(),
    vtmd_name.ptr(),
    TL_READ);
  int error;
  bool rc= false;
  String_fs archive;
  bool end_keyread= false;
  bool index_end= false;
  Open_tables_backup open_tables_backup;
  key_buf_t key;

  vtmd= open_log_table(thd, &vtmd_tl, &open_tables_backup);
  if (!vtmd)
    return true;

  if (key.allocate(vtmd->key_info[IDX_ARCHIVE_NAME].key_length))
    return true;

  if ((error= vtmd->file->ha_start_keyread(IDX_ARCHIVE_NAME)))
    goto err;
  end_keyread= true;

  if ((error= vtmd->file->ha_index_init(IDX_ARCHIVE_NAME, true)))
    goto err;
  index_end= true;

  error= vtmd->file->ha_index_first(vtmd->record[0]);
  while (!error)
  {
    if (!vtmd->field[FLD_ARCHIVE_NAME]->is_null())
    {
      vtmd->field[FLD_ARCHIVE_NAME]->val_str(&archive);
      key_copy(key,
                vtmd->record[0],
                &vtmd->key_info[IDX_ARCHIVE_NAME],
                vtmd->key_info[IDX_ARCHIVE_NAME].key_length,
                false);
      error= vtmd->file->ha_index_read_map(
        vtmd->record[0],
        key,
        vtmd->key_info[IDX_ARCHIVE_NAME].ext_key_part_map,
        HA_READ_PREFIX_LAST);
      if (!error)
      {
        if ((rc= move_table(thd, archive, new_db)))
          break;

        error= vtmd->file->ha_index_next(vtmd->record[0]);
      }
    }
    else
    {
      archive.length(0);
      error= vtmd->file->ha_index_next(vtmd->record[0]);
    }
  }

  if (error && error != HA_ERR_END_OF_FILE)
  {
err:
    vtmd->file->print_error(error, MYF(0));
    rc= true;
  }

  if (index_end)
    vtmd->file->ha_index_end();
  if (end_keyread)
    vtmd->file->ha_end_keyread();

  close_log_table(thd, &open_tables_backup);
  return rc;
}

bool
VTMD_rename::move_table(THD *thd, String &table_name, LEX_STRING &new_db)
{
  handlerton *table_hton= NULL;
  if (!ha_table_exists(thd, about.db, table_name.ptr(), &table_hton) || !table_hton)
  {
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` archive doesn't exist",
        about.db, table_name.ptr());
    return false;
  }

  if (ha_table_exists(thd, new_db.str, table_name.ptr()))
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` archive already exists!", MYF(0),
                        new_db.str, table_name.ptr());
    return true;
  }

  TABLE_LIST tl;
  tl.init_one_table(
    about.db, about.db_length,
    table_name.ptr(), table_name.length(),
    table_name.ptr(),
    TL_WRITE_ONLY);
  tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &tl);
  if (lock_table_names(thd, &tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db, table_name.ptr(), false);

  bool rc= mysql_rename_table(
    table_hton,
    about.db, table_name.ptr(),
    new_db.str, table_name.ptr(),
    NO_FK_CHECKS);
  if (!rc)
    query_cache_invalidate3(thd, &tl, 0);

  return rc;
}

bool
VTMD_rename::try_rename(THD *thd, LEX_STRING &new_db, LEX_STRING &new_alias)
{
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  TABLE_LIST new_table;

  if (check_exists(thd))
    return true;

  new_table.init_one_table(
    LEX_STRING_WITH_LEN(new_db),
    LEX_STRING_WITH_LEN(new_alias),
    new_alias.str, TL_READ);

  if (new_table.vers_vtmd_name(vtmd_new_name))
    return true;

  if (ha_table_exists(thd, new_db.str, vtmd_new_name.ptr()))
  {
    if (exists)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` table already exists!", MYF(0),
                          new_db.str, vtmd_new_name.ptr());
      return true;
    }
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` table already exists!",
        new_db.str, vtmd_new_name.ptr());
    return false;
  }

  if (exists)
  {
    if (CString_fs(about.db, about.db_length) != new_db)
    {
      // Move archives before VTMD so if the operation is interrupted, it could be continued.
      if (move_archives(thd, new_db))
        return true;
    }

    TABLE_LIST vtmd_tl;
    vtmd_tl.init_one_table(
      about.db, about.db_length,
      vtmd_name.ptr(), vtmd_name.length(),
      vtmd_name.ptr(),
      TL_WRITE_ONLY);
    vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);

    mysql_ha_rm_tables(thd, &vtmd_tl);
    if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
      return true;
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about.db, vtmd_name.ptr(), false);
    bool rc= mysql_rename_table(
      hton,
      about.db, vtmd_name.ptr(),
      new_db.str, vtmd_new_name.ptr(),
      NO_FK_CHECKS);
    if (!rc)
    {
      query_cache_invalidate3(thd, &vtmd_tl, 0);
      local_da.finish();
      VTMD_table new_vtmd(new_table);
      rc= new_vtmd.update(thd);
    }
    return rc;
  }
  return false;
}

bool
VTMD_rename::revert_rename(THD *thd, LEX_STRING &new_db)
{
  DBUG_ASSERT(hton);
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);

  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    about.db, about.db_length,
    vtmd_new_name.ptr(), vtmd_new_name.length(),
    vtmd_new_name.ptr(),
    TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);
  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, new_db.str, vtmd_new_name.ptr(), false);

  bool rc= mysql_rename_table(
    hton,
    new_db.str, vtmd_new_name.ptr(),
    new_db.str, vtmd_name.ptr(),
    NO_FK_CHECKS);

  if (!rc)
    query_cache_invalidate3(thd, &vtmd_tl, 0);

  return rc;
}

void VTMD_table::archive_name(
  THD* thd,
  const char* table_name,
  char* new_name,
  size_t new_name_size)
{
  const MYSQL_TIME now= thd->query_start_TIME();
  my_snprintf(new_name, new_name_size, "%s_%04d%02d%02d_%02d%02d%02d_%06d",
              table_name, now.year, now.month, now.day, now.hour, now.minute,
              now.second, now.second_part);
}
