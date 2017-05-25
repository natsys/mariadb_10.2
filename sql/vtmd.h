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
    TRX_ID_START= 0,
    TRX_ID_END,
    OLD_NAME,
    NAME,
    FRM_IMAGE,
    COL_RENAMES
  };

  VTMD_table(TABLE_LIST &_about) : vtmd(NULL), about(_about) {}

  bool create(THD *thd, String &vtmd_name);
  bool find_record(THD *thd, ulonglong sys_trx_end, bool &found);
  bool write_row(THD *thd);
};

#endif // VTMD_INCLUDED
