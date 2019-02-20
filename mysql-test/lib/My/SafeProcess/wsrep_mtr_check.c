/* Copyright (c) 2009, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>

#include "../../wsrep-lib/wsrep-API/v26/wsrep_api.h"

/**************************************************************************
 * Library loader
 **************************************************************************/

static int wsrep_check_iface_version(const char* found, const char* iface_ver)
{
    if (strcmp(found, iface_ver)) {
        return EINVAL;
    }
    return 0;
}

typedef int (*wsrep_loader_fun)(wsrep_t*);

static wsrep_loader_fun wsrep_dlf(void *dlh, const char *sym)
{
    union {
        wsrep_loader_fun dlfun;
        void *obj;
    } alias;
    alias.obj = dlsym(dlh, sym);
    return alias.dlfun;
}

static int wsrep_check_version_symbol(void *dlh)
{
    char** dlversion = NULL;
    dlversion = (char**) dlsym(dlh, "wsrep_interface_version");
    if (dlversion == NULL)
        return 0;
    return wsrep_check_iface_version(*dlversion, WSREP_INTERFACE_VERSION);
}

int main(void)
{
    int ret = 0;
    void *dlh = NULL;
    wsrep_loader_fun dlfun;

    if (!(dlh = dlopen(getenv("WSREP_PROVIDER"), RTLD_NOW | RTLD_LOCAL))) {
       goto err;
    }

    if (!(dlfun = wsrep_dlf(dlh, "wsrep_loader"))) {
       goto err;
    }

    if (wsrep_check_version_symbol(dlh) != 0) {
       return 2;
    }

    return 0;

err:
    if (dlh) dlclose(dlh);
    return 1;
}
