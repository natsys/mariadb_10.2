/*
   Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2020, MariaDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <stddef.h>
#include <limits.h>
#include <my_attribute.h>
#include <my_global.h>
#include "../storage/innobase/include/fts0tokenize.h"

#define U_LOW_LINE      0x5f            // Code point for underscore '_'.
#define is_alnum(ct)    ((ct) & (_MY_U | _MY_L | _MY_NMR))

static int ngram_var_ngram_token_size;

static MYSQL_SYSVAR_INT(token_size, ngram_var_ngram_token_size,
  0,
  "Ngram full text plugin parser token size in characters",
  NULL, NULL, 2, 1, 10, 0);

static int my_ci_charlen(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  return my_charlen(cs, reinterpret_cast<const char *>(str),
                    reinterpret_cast<const char *>(end));
}

static int my_ci_ctype(CHARSET_INFO *cs, int *char_type, const uchar *str,
                       const uchar *end)
{
  return cs->cset->ctype(cs, char_type, str, end);
}

static bool is_underscore(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  my_wc_t wc;
  cs->cset->mb_wc(cs, &wc, str, end);
  return wc == U_LOW_LINE;
}

// Splits a string into ngrams and emits them.
static int split_into_ngrams(MYSQL_FTPARSER_PARAM *param, int ngram_length,
                             const uchar *doc, int len,
                             MYSQL_FTPARSER_BOOLEAN_INFO *bool_info)
{
  const CHARSET_INFO *cs= param->cs;
  const uchar *start= doc;
  const uchar *end= doc + len;
  const uchar *next= start;
  int n_chars= 0;
  int ret= 0;
  bool is_first= true;

  while (next < end) {
    int char_type;
    int char_len= my_ci_ctype(cs, &char_type, next, end);

    // Broken data?
    if (next + char_len > end || char_len == 0)
      break;

    // Avoid creating partial n-grams by stopping at word boundaries.
    // Underscore is treated as a word character too, since identifiers in
    // programming languages often contain them.
    if (!is_alnum(char_type) && !is_underscore(cs, next, end)) {
      start= next + char_len;
      next= start;
      n_chars= 0;
      continue;
    }

    next += char_len;
    n_chars++;

    if (n_chars == ngram_length) {
      bool_info->position= static_cast<uint>(start - doc);
      ret= param->mysql_add_word(param, reinterpret_cast<const char *>(start),
                                 static_cast<int>(next - start), bool_info);
      if (ret != 0)
        return ret;

      start += my_ci_charlen(cs, start, end);
      n_chars= ngram_length - 1;
      is_first= false;
    }
  }

  // If nothing was emitted yet, emit the remainder.
  switch (param->mode) {
    case MYSQL_FTPARSER_FULL_BOOLEAN_INFO:
    case MYSQL_FTPARSER_WITH_STOPWORDS:
      if (n_chars > 0 && is_first) {
        assert(next > start);
        assert(n_chars < ngram_length);

        bool_info->position= static_cast<uint>(start - doc);
        ret= param->mysql_add_word(param, reinterpret_cast<const char *>(start),
                                   static_cast<int>(next - start), bool_info);
      }
      break;

    default:
      break;
  }

  return ret;
}

static int number_of_chars(const CHARSET_INFO *cs, const uchar *buf, int len)
{
  const uchar *ptr= buf;
  const uchar *end= buf + len;
  int size= 0;

  while (ptr < end) {
    ptr += my_ci_charlen(cs, ptr, end);
    size += 1;
  }

  return size;
}

// Convert term into phrase and handle wildcard.
static int ngram_term_convert(MYSQL_FTPARSER_PARAM *param, int ngram_length,
                              const uchar *token, int len,
                              MYSQL_FTPARSER_BOOLEAN_INFO *bool_info)
{
  MYSQL_FTPARSER_BOOLEAN_INFO token_info=
    { FT_TOKEN_WORD, 0, 0, 0, 0, 0, ' ', 0 };
  const CHARSET_INFO *cs= param->cs;
  int ret= 0;

  assert(bool_info->type == FT_TOKEN_WORD);
  assert(bool_info->quot == NULL);

  /* Convert rules:
   * 1. if term with wildcard and term length is less than ngram_length,
   *    we keep it as normal term search.
   * 2. otherwise, term is converted to phrase and wildcard is ignored.
   *    e.g. 'abc' and 'abc*' are both equivalent to '"ab bc"'.
   */

  if (bool_info->trunc && number_of_chars(cs, token, len) < ngram_length) {
    ret= param->mysql_add_word(param, reinterpret_cast<const char *>(token),
                               len, bool_info);
  } else {
    bool_info->type= FT_TOKEN_LEFT_PAREN;
    bool_info->quot= reinterpret_cast<char*>(1);

    ret= param->mysql_add_word(param, NULL, 0, bool_info);
    if (ret != 0)
      return ret;

    ret= split_into_ngrams(param, ngram_length, token, len, &token_info);
    if (ret != 0)
      return ret;

    bool_info->type= FT_TOKEN_RIGHT_PAREN;
    ret= param->mysql_add_word(param, NULL, 0, bool_info);

    assert(bool_info->quot == NULL);
    bool_info->type= FT_TOKEN_WORD;
  }

  return ret;
}

static int ngram_parse(MYSQL_FTPARSER_PARAM *param, int ngram_length)
{
  MYSQL_FTPARSER_BOOLEAN_INFO bool_info=
    { FT_TOKEN_WORD, 0, 0, 0, 0, 0, ' ', 0 };
  const CHARSET_INFO *cs= param->cs;
  uchar *start= reinterpret_cast<uchar *>(const_cast<char *>(param->doc));
  uchar *end= start + param->length;
  FT_WORD word= {NULL, 0, 0};
  int ret= 0;

  switch (param->mode) {
    case MYSQL_FTPARSER_SIMPLE_MODE:
    case MYSQL_FTPARSER_WITH_STOPWORDS:
      ret= split_into_ngrams(param, ngram_length, start,
                             static_cast<int>(end - start), &bool_info);
      break;

    case MYSQL_FTPARSER_FULL_BOOLEAN_INFO:
      // Reusing InnoDB's boolean mode parser to handle all special characters.
      while (fts_get_word(cs, &start, end, &word, &bool_info)) {
        if (bool_info.type == FT_TOKEN_WORD) {
          if (bool_info.quot != NULL) {
            ret= split_into_ngrams(param, ngram_length, word.pos, word.len,
                                   &bool_info);
          } else {
            ret= ngram_term_convert(param, ngram_length, word.pos, word.len,
                                    &bool_info);
          }
        } else {
          ret= param->mysql_add_word(param, reinterpret_cast<char*>(word.pos),
                                     word.len, &bool_info);
        }

        if (ret != 0)
          return ret;
      }

      break;
  }

  return ret;
}

static int ngram_parse_n(MYSQL_FTPARSER_PARAM *param)
{
  return ngram_parse(param, ngram_var_ngram_token_size);
}

static int ngram_parse_2(MYSQL_FTPARSER_PARAM *param)
{
  return ngram_parse(param, 2);
}

static int ngram_parse_3(MYSQL_FTPARSER_PARAM *param)
{
  return ngram_parse(param, 3);
}

static struct st_mysql_ftparser ngram_ngram_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  ngram_parse_n, NULL, NULL,
};

static struct st_mysql_ftparser ngram_2gram_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  ngram_parse_2, NULL, NULL,
};

static struct st_mysql_ftparser ngram_3gram_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  ngram_parse_3, NULL, NULL,
};

static struct st_mysql_sys_var *ngram_system_variables[]=
{
  MYSQL_SYSVAR(token_size),
  NULL
};

maria_declare_plugin(ngram_parser)
{
  MYSQL_FTPARSER_PLUGIN,        // Type.
  &ngram_ngram_descriptor,      // Descriptor.
  "ngram",                      // Name.
  "Oracle Corp",                // Author.
  "Ngram Full-Text Parser",     // Description.
  PLUGIN_LICENSE_GPL,           // License.
  NULL,                         // Initialization function.
  NULL,                         // Deinitialization function.
  0x0100,                       // Numeric version.
  NULL,                         // Status variables.
  ngram_system_variables,       // System variables.
  "1.0",                        // String version representation.
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL, // Maturity.
},
{
  MYSQL_FTPARSER_PLUGIN,        // Type.
  &ngram_2gram_descriptor,      // Descriptor.
  "2gram",                      // Name.
  "Oracle Corp",                // Author.
  "Ngram Full-Text Parser",     // Description.
  PLUGIN_LICENSE_GPL,           // License.
  NULL,                         // Initialization function.
  NULL,                         // Deinitialization function.
  0x0100,                       // Numeric version.
  NULL,                         // Status variables.
  NULL,                         // System variables.
  "1.0",                        // String version representation.
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL, // Maturity.
},
{
  MYSQL_FTPARSER_PLUGIN,        // Type.
  &ngram_3gram_descriptor,      // Descriptor.
  "3gram",                      // Name.
  "Oracle Corp",                // Author.
  "Ngram Full-Text Parser",     // Description.
  PLUGIN_LICENSE_GPL,           // License.
  NULL,                         // Initialization function.
  NULL,                         // Deinitialization function.
  0x0100,                       // Numeric version.
  NULL,                         // Status variables.
  NULL,                         // System variables.
  "1.0",                        // String version representation.
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL, // Maturity.
}
maria_declare_plugin_end;
