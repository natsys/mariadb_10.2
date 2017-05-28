#ifndef VTMD_INCLUDED
#define VTMD_INCLUDED

#include "table.h"
#include "unireg.h"
#include <mysqld_error.h>

class THD;

class VTMD_table
{
  TABLE *vtmd;
  TABLE_LIST &about;

private:
  VTMD_table(const VTMD_table&); // prohibit copying references

public:
  enum {
    FLD_START= 0,
    FLD_END,
    FLD_NAME,
    FLD_ARCHIVE_NAME,
    FLD_COL_RENAMES,
    FIELD_COUNT
  };

  enum {
    IDX_END= 0
  };

  VTMD_table(TABLE_LIST &_about) : vtmd(NULL), about(_about) {}

  bool create(THD *thd, String &vtmd_name);
  bool find_record(THD *thd, ulonglong sys_trx_end, bool &found);
  bool write_row(THD *thd, const char* archive_name= NULL);
  bool try_rename(THD *thd, const char* new_db, const char *new_alias);
};

#endif // VTMD_INCLUDED
