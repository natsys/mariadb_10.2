#ifndef VTMD_INCLUDED
#define VTMD_INCLUDED

#include "table.h"
#include "unireg.h"
#include <mysqld_error.h>

class THD;

class VTMD_table
{
protected:
  TABLE *vtmd;
  const TABLE_LIST &about;
  String vtmd_name;

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

  VTMD_table(TABLE_LIST &_about) :
    vtmd(NULL),
    about(_about)
  {}

  bool create(THD *thd, String &vtmd_name);
  bool find_record(THD *thd, ulonglong sys_trx_end, bool &found);
  bool update(THD *thd, const char* archive_name= NULL);

  static void archive_name(THD *thd, const char *table_name, char *new_name, size_t new_name_size);
  void archive_name(THD *thd, char *new_name, size_t new_name_size)
  {
    archive_name(thd, about.table_name, new_name, new_name_size);
  }

};

class VTMD_exists : public VTMD_table
{
protected:
  handlerton *hton;

public:
  bool exists;

public:
  VTMD_exists(TABLE_LIST &_about) :
    VTMD_table(_about),
    hton(NULL),
    exists(false)
  {}

  bool check_exists(THD *thd); // returns error status
};

class VTMD_rename : public VTMD_exists
{
  String vtmd_new_name;

public:
  VTMD_rename(TABLE_LIST &_about) :
    VTMD_exists(_about)
  {}

  bool try_rename(THD *thd, LEX_STRING &new_db, LEX_STRING &new_alias);
  bool revert_rename(THD *thd, LEX_STRING &new_db);
};

class VTMD_drop : public VTMD_exists
{
  char archive_name_[FN_LEN];

public:
  VTMD_drop(TABLE_LIST &_about) :
    VTMD_exists(_about)
  {
    *archive_name_= 0;
  }

  const char* archive_name(THD *thd)
  {
    VTMD_table::archive_name(thd, archive_name_, sizeof(archive_name_));
    return archive_name_;
  }

  const char* archive_name() const
  {
    DBUG_ASSERT(*archive_name_);
    return archive_name_;
  }

  bool update(THD *thd)
  {
    DBUG_ASSERT(*archive_name_);
    return VTMD_exists::update(thd, archive_name_);
  }
};


#endif // VTMD_INCLUDED
