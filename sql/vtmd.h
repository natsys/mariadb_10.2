#ifndef VTMD_INCLUDED
#define VTMD_INCLUDED

#include "table.h"
#include "unireg.h"
#include <mysqld_error.h>

extern LEX_STRING VERS_VTMD_NAME;

class THD;

class VTMD_table
{
public:
  enum {
    TRX_ID_START= 0,
    TRX_ID_END,
    OLD_NAME,
    NAME,
    FRM_IMAGE,
    COL_RENAMES
  };

  static bool write_row(THD *thd);
};

#endif // VTMD_INCLUDED
